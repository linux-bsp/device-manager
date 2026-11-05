/* Clock management is a typed protocol, independent of SOL text/cursors. */
#define _GNU_SOURCE
#include "time-control.h"
#include "time-sync.h"
#include "../ota/shared/time-source.h"
#include "dm/transfer.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <time.h>
#include <unistd.h>

struct settings {long long revision;int enabled;unsigned interval;char source[512];int outcome;};
static struct settings current={.enabled=1,.interval=3600,.source="-",.outcome=-1};
static long long attempted;
static int loaded,last_error;
static volatile sig_atomic_t cancelled;
void dm_clock_cancel(void){cancelled=1;}
static const char *config_path(void)
{const char *p=getenv("TIMECTL_STATE_FILE");return p?p:"/data/timectl/source.conf";}
static int load_path(const char *path, struct settings *s)
{
    FILE *f=fopen(path,"r");if(!f)return -errno;
    char body[700];size_t n=fread(body,1,sizeof(body)-1,f);int bad=ferror(f)||!feof(f);fclose(f);body[n]=0;
    int used=0;
    if(bad||sscanf(body,"DMCLOCKLOCAL1 %lld %d %u %511s %d%n",&s->revision,&s->enabled,&s->interval,s->source,&s->outcome,&used)!=5||
       strcmp(body+used,"\n")||s->revision<0||s->enabled<0||s->enabled>1||s->interval<60||s->interval>86400||!dm_source_valid(s->source)||s->outcome < -1||s->outcome>4095)return -EINVAL;
    return 0;
}
static int save(const struct settings *s)
{
    const char *path=config_path();char temp[1024],parent[1024];
    if(strlen(path)>980||path[0]!='/')return -EINVAL;
    strcpy(parent,path);char *slash=strrchr(parent,'/');if(!slash)return -EINVAL;*slash=0;
    struct statfs fs;if(statfs(parent,&fs))return -errno;
    /* Never acknowledge durable configuration in a RAM-only fallback. */
    if(fs.f_type==0x01021994||fs.f_type==0x858458f6)return -EROFS;
    snprintf(temp,sizeof(temp),"%s.tmp",path);
    int fd=open(temp,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return -errno;
    int error=dprintf(fd,"DMCLOCKLOCAL1 %lld %d %u %s %d\n",s->revision,s->enabled,s->interval,s->source,s->outcome)<0?errno:0;
    if(fsync(fd)&&!error)error=errno;
    if(close(fd)&&!error)error=errno;
    if(!error&&rename(temp,path))error=errno;
    int directory=open(parent,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(directory<0){if(!error)error=errno;}else{if(fsync(directory)&&!error)error=errno;close(directory);}
    if(error)unlink(temp);
    return -error;
}
int dm_clock_init(int enabled, unsigned interval, const char *source)
{
    current.enabled=enabled;current.interval=interval;
    snprintf(current.source,sizeof(current.source),"%s",source);
    struct settings saved;
    int rc=load_path(config_path(),&saved);
    /* One-time import of on-device settings. Leave the old record untouched
     * for a firmware rollback; never replace a present new-format location. */
    if(rc==-ENOENT){
        const char *old=getenv("TIMECTL_IMPORT_FILE");
        if(!old&&!getenv("TIMECTL_STATE_FILE"))old="/data/log/time-source.conf";
        if(old&&*old){
            int imported=load_path(old,&saved);
            if(!imported){
                rc=save(&saved);
                if(rc)fprintf(stderr,"[timectld] [WARN] time configuration import not persisted: %s\n",strerror(-rc));
                else fprintf(stderr,"[timectld] [INFO] imported existing time source configuration\n");
                /* Even when data is read-only, honor the valid saved source. */
                current=saved;
            }else if(imported!=-ENOENT)rc=imported;
        }
    }
    if(!rc)current=saved;
    if(current.outcome>=0){attempted=current.revision;last_error=current.outcome;}
    dm_time_interval(current.interval);loaded=1;
    return rc==-ENOENT?0:rc;
}
int dm_clock_sync_now(const char *server,const char *interface)
{
    return dm_time_sync(strcmp(current.source,"-")?current.source:server,interface,1);
}
int dm_clock_set_source(const char *source)
{
    if(!dm_source_valid(source))return -EINVAL;
    struct settings desired=current;
    snprintf(desired.source,sizeof(desired.source),"%s",source);
    int rc=save(&desired);
    if(!rc)current=desired;
    return rc;
}
int dm_clock_status(const char *server,char *body,unsigned capacity)
{
    const char *source=strcmp(current.source,"-")?current.source:server;
    int n=snprintf(body,capacity,"daemon-state=running\nclock-enabled=%d\nclock-interval=%u\nclock-effective-source=%s\n",
                   current.enabled,current.interval,source);
    if(n<0||(unsigned)n>=capacity)return -ENOSPC;
    char path[1024];const char *run=getenv("TIMECTL_RUN_DIR");if(!run)run="/run/timectl";
    if(snprintf(path,sizeof(path),"%s/clock-status",run)>=(int)sizeof(path))return -ENAMETOOLONG;
    FILE *f=fopen(path,"r");
    if(!f){
        int k=snprintf(body+n,capacity-(unsigned)n,"clock-state=unsynchronized\n");
        return k<0||(unsigned)k>=capacity-(unsigned)n?-ENOSPC:0;
    }
    size_t got=fread(body+n,1,capacity-(unsigned)n-1,f);
    int rc=ferror(f)||!feof(f)?-EOVERFLOW:0;
    fclose(f);body[n+got]=0;return rc;
}
struct response {char body[2048];size_t length;};
static int transfer_cancelled(void *context) { (void)context; return cancelled != 0; }
static int exchange(const char *server,const char *device,const char *key,const char *body,struct response *r)
{
    char url[1024],auth[96];if(snprintf(url,sizeof(url),"%s/api/clock/device/%s",server,device)>=(int)sizeof(url))return -EINVAL;
    const char *headers[2];size_t n=0;
    if(*key){snprintf(auth,sizeof(auth),"Authorization: Bearer %s",key);headers[n++]=auth;}
    headers[n++]="Content-Type: text/plain";
    struct dm_transfer_request request;struct dm_transfer_result response;
    dm_transfer_request_init(&request);
    request.url=url;request.headers=headers;request.header_count=n;
    if(body){request.method="POST";request.body=body;request.body_size=strlen(body);}
    request.response=r->body;request.response_capacity=sizeof(r->body);
    request.connect_timeout_ms=1000;request.timeout_ms=2000;request.callbacks.cancelled=transfer_cancelled;
    int rc=dm_transfer_perform(&request,&response);r->length=response.response_size;
    return cancelled?-ECANCELED:!rc&&response.http_status==200?0:-EHOSTUNREACH;
}
static void report(const char *server,const char *device,const char *key,const char *source)
{
    char boot[64]="unknown",body[1900],path[1024],status[1400]="";const char *run=getenv("TIMECTL_RUN_DIR");if(!run)run="/run/timectl";
    FILE *f=fopen("/proc/sys/kernel/random/boot_id","r");if(f){if(!fgets(boot,sizeof(boot),f))strcpy(boot,"unknown");fclose(f);boot[strcspn(boot,"\r\n")]=0;}
    snprintf(path,sizeof(path),"%s/clock-status",run);f=fopen(path,"r");
    if(f){size_t n=fread(status,1,sizeof(status)-1,f);status[n]=0;fclose(f);}
    struct timespec mono;clock_gettime(CLOCK_BOOTTIME,&mono);
    int n=snprintf(body,sizeof(body),"DMCLOCKREPORT1 %lld %lld %d %lld %lld %s\nclock-enabled=%d\nclock-interval=%u\nclock-effective-source=%s\n%s",current.revision,attempted,last_error,(long long)time(NULL),(long long)mono.tv_sec,boot,current.enabled,current.interval,source,status);
    if(n<=0||n>=(int)sizeof(body))return;
    struct response response={0};(void)exchange(server,device,key,body,&response);
}
void dm_clock_cycle(const char *server,const char *interface,const char *device,const char *key)
{
    if(cancelled)return;
    if(!loaded)return;
    struct response r={0};int remote=exchange(server,device,key,NULL,&r);
    if(!remote){
        struct settings desired={.outcome=-1};int used=0;
        if(!memchr(r.body,0,r.length)&&sscanf(r.body,"DMCLOCK1 %lld %d %u %511s%n",&desired.revision,&desired.enabled,&desired.interval,desired.source,&used)==4&&
           !strcmp(r.body+used,"\n")&&desired.revision>0&&desired.enabled>=0&&desired.enabled<=1&&desired.interval>=60&&desired.interval<=86400&&dm_source_valid(desired.source)&&
           desired.revision!=attempted){
            attempted=desired.revision;
            int error=save(&desired);
            if(!error){
                current=desired;dm_time_interval(current.interval);
                const char *source=strcmp(current.source,"-")?current.source:server;
                if(current.enabled)error=dm_time_sync(source,interface,1);else dm_time_disabled(source);
                if(error==-ECANCELED)return;
                current.outcome=error<0?-error:0;
                int saved=save(&current);if(saved)error=saved;
            }
            last_error=error<0?-error:0;
            fprintf(stderr,"[timectld] [%s] clock configuration %s: source=%s interval=%u seconds%s\n",last_error?"WARN":"INFO",last_error?strerror(last_error):"applied",desired.source,desired.interval,desired.enabled?"":" (automatic synchronization disabled)");
        }
    }
    if(cancelled)return;
    const char *source=strcmp(current.source,"-")?current.source:server;
    dm_time_interval(current.interval);
    if(current.enabled)(void)dm_time_sync(source,interface,0);else dm_time_disabled(source);
    if(!remote&&!cancelled)report(server,device,key,source);
}
