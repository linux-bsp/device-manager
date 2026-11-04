/* Clock management is a typed protocol, independent of SOL text/cursors. */
#define _GNU_SOURCE
#include "time-control.h"
#include "time-sync.h"
#include "../ota/shared/time-source.h"
#include <curl/curl.h>
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
{const char *p=getenv("LOGCTL_TIME_CONFIG");return p?p:"/data/log/time-source.conf";}
static int load(struct settings *s)
{
    FILE *f=fopen(config_path(),"r");if(!f)return -errno;
    char body[700];size_t n=fread(body,1,sizeof(body)-1,f);int bad=ferror(f)||!feof(f);fclose(f);body[n]=0;
    int used=0;
    if(bad||sscanf(body,"DMCLOCKLOCAL1 %lld %d %u %511s %d%n",&s->revision,&s->enabled,&s->interval,s->source,&s->outcome,&used)!=5||
       strcmp(body+used,"\n")||s->revision<1||s->enabled<0||s->enabled>1||s->interval<60||s->interval>86400||!dm_source_valid(s->source)||s->outcome < -1||s->outcome>4095)return -EINVAL;
    return 0;
}
int dm_clock_local(char source[512],int *enabled,unsigned *interval)
{
    struct settings s;int result=load(&s);if(result)return result;
    strcpy(source,s.source);*enabled=s.enabled;*interval=s.interval;return 0;
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
struct response {char body[2048];size_t length;};
static size_t receive(char *data,size_t a,size_t b,void *arg)
{
    struct response *r=arg;size_t n=a*b;if(n>=sizeof(r->body)-r->length)return 0;
    memcpy(r->body+r->length,data,n);r->length+=n;r->body[r->length]=0;return n;
}
static int progress(void *p,curl_off_t a,curl_off_t b,curl_off_t c,curl_off_t d)
{(void)p;(void)a;(void)b;(void)c;(void)d;return cancelled?1:0;}
static int exchange(const char *server,const char *device,const char *key,const char *body,struct response *r)
{
    char url[1024],auth[96];if(snprintf(url,sizeof(url),"%s/api/clock/device/%s",server,device)>=(int)sizeof(url))return -EINVAL;
    CURL *c=curl_easy_init();if(!c)return -ENOMEM;
    struct curl_slist *h=NULL;
    if(*key){snprintf(auth,sizeof(auth),"Authorization: Bearer %s",key);h=curl_slist_append(h,auth);}
    h=curl_slist_append(h,"Content-Type: text/plain");h=curl_slist_append(h,"Expect:");
    curl_easy_setopt(c,CURLOPT_URL,url);curl_easy_setopt(c,CURLOPT_HTTPHEADER,h);
    if(body){curl_easy_setopt(c,CURLOPT_POSTFIELDS,body);curl_easy_setopt(c,CURLOPT_POSTFIELDSIZE,(long)strlen(body));}
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,receive);curl_easy_setopt(c,CURLOPT_WRITEDATA,r);
    curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT_MS,1000L);curl_easy_setopt(c,CURLOPT_TIMEOUT_MS,2000L);
    curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);curl_easy_setopt(c,CURLOPT_NOPROXY,"*");curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"http,https");
    curl_easy_setopt(c,CURLOPT_XFERINFOFUNCTION,progress);curl_easy_setopt(c,CURLOPT_NOPROGRESS,0L);
    CURLcode result=curl_easy_perform(c);long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);
    curl_slist_free_all(h);curl_easy_cleanup(c);
    return cancelled?-ECANCELED:result==CURLE_OK&&status==200?0:-EHOSTUNREACH;
}
static void report(const char *server,const char *device,const char *key,const char *source)
{
    char boot[64]="unknown",body[1900],path[1024],status[1400]="";const char *run=getenv("LOGCTL_RUN_DIR");if(!run)run="/run/logctl";
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
    if(!loaded){
        struct settings saved;if(!load(&saved)){current=saved;if(saved.outcome>=0){attempted=saved.revision;last_error=saved.outcome;}}
        loaded=1;
    }
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
            fprintf(stderr,"[logctld] [%s] clock configuration %s: source=%s interval=%u seconds%s\n",last_error?"WARN":"INFO",last_error?strerror(last_error):"applied",desired.source,desired.interval,desired.enabled?"":" (automatic synchronization disabled)");
        }
    }
    if(cancelled)return;
    const char *source=strcmp(current.source,"-")?current.source:server;
    dm_time_interval(current.interval);
    if(current.enabled)(void)dm_time_sync(source,interface,0);else dm_time_disabled(source);
    if(!remote&&!cancelled)report(server,device,key,source);
}
