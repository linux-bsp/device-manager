#define _GNU_SOURCE
#include "boot-record.h"
#include "dm/transfer.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#define RECORD_CAP 1024
#define JOURNAL_LIMIT 65536
struct record {
    char boot[64], previous[64], phase[32], intent[32], previous_phase[32];
    char previous_intent[32], previous_integrity[16], previous_health[16], reset_label[16];
    unsigned healthy;
    unsigned long long sequence, started_wall, started_mono, wall, mono;
};

static int safe(const char *s)
{
    if (!*s || strlen(s)>63) return 0;
    for (;*s;s++) if (!((*s>='a'&&*s<='z')||(*s>='0'&&*s<='9')||*s=='-'||*s=='_')) return 0;
    return 1;
}
static int known(const char *value,const char *const *values)
{
    for (;*values;values++) if (!strcmp(value,*values)) return 1;
    return 0;
}
static const char *const phases[]={"booting","healthy","reboot-requested","shutdown-started","unknown",NULL};
static const char *const intents[]={"none","ota-update","health-rejected","operator","unspecified-shutdown",NULL};
static const char *const integrity[]={"none","valid","missing","invalid",NULL};
static const char *const health[]={"confirmed","pending","unknown",NULL};
static const char *const labels[]={"POR","ESM","WDOG","RST","DMSC","JTAG","THERMAL","PIN","UNKNOWN",NULL};

static int path(char *out,const char *root,const char *boot,const char *name)
{
    if (!safe(boot)) return -EINVAL;
    return snprintf(out,PATH_MAX,"%s/%s/%s",root,boot,name)>=PATH_MAX?-ENAMETOOLONG:0;
}
static int encode(const struct record *r,char *out)
{
    char body[RECORD_CAP];
    int n=snprintf(body,sizeof(body),"DMBOOT1 %s %s %s %s %s %s %s %s %s %u %llu %llu %llu %llu %llu",
        r->boot,r->previous,r->phase,r->intent,r->previous_phase,r->previous_intent,
        r->previous_integrity,r->previous_health,r->reset_label,r->healthy,r->sequence,r->started_wall,r->started_mono,r->wall,r->mono);
    if (n<0||n>=RECORD_CAP-16) return -EOVERFLOW;
    unsigned checksum=(unsigned)crc32(0,(const unsigned char *)body,(uInt)n);
    return snprintf(out,RECORD_CAP,"%08x %s\n",checksum,body);
}
static int decode(char *line,struct record *r)
{
    size_t length=strlen(line);
    if (length<11||line[8]!=' '||line[length-1]!='\n') return -EBADMSG;
    unsigned checksum;int end=0;
    if (sscanf(line,"%8x%n",&checksum,&end)!=1||end!=8) return -EBADMSG;
    if (checksum!=(unsigned)crc32(0,(unsigned char *)line+9,(uInt)(length-10))) return -EBADMSG;
    end=0;
    if (sscanf(line+9,"DMBOOT1 %63s %63s %31s %31s %31s %31s %15s %15s %15s %u %llu %llu %llu %llu %llu%n",
        r->boot,r->previous,r->phase,r->intent,r->previous_phase,r->previous_intent,
        r->previous_integrity,r->previous_health,r->reset_label,&r->healthy,&r->sequence,&r->started_wall,&r->started_mono,&r->wall,&r->mono,&end)!=15 ||
        strcmp(line+9+end,"\n")||!safe(r->boot)||!safe(r->previous)||!r->sequence||r->healthy>1||
        !known(r->phase,phases)||!known(r->previous_phase,phases)||!known(r->intent,intents)||
        !known(r->previous_intent,intents)||!known(r->previous_health,health)||!known(r->previous_integrity,integrity)||!known(r->reset_label,labels)) return -EBADMSG;
    return 0;
}
static int load(const char *root,const char *boot,struct record *r)
{
    char filename[PATH_MAX],line[RECORD_CAP];
    int rc=path(filename,root,boot,"boot-state");if (rc) return rc;
    int fd=open(filename,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if (fd<0) return -errno;
    struct stat st;
    if (fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_nlink!=1||st.st_size<=0||st.st_size>=RECORD_CAP) {close(fd);return -EBADMSG;}
    size_t used=0;
    while (used<(size_t)st.st_size) {
        ssize_t n=read(fd,line+used,(size_t)st.st_size-used);
        if (n<0&&errno==EINTR) continue;
        if (n<=0) {close(fd);return -EIO;}
        used+=(size_t)n;
    }
    close(fd);line[used]=0;
    if (memchr(line,0,used)) return -EBADMSG;
    rc=decode(line,r);
    return rc?rc:strcmp(r->boot,boot)?-EBADMSG:0;
}
static int write_all(int fd,const char *text,size_t size)
{
    while (size) {
        ssize_t n=write(fd,text,size);
        if (n<0&&errno==EINTR) continue;
        if (n<=0) return n?-errno:-EIO;
        text+=n;size-=(size_t)n;
    }
    return 0;
}
static int open_regular(const char *name,int flags)
{
    int fd=open(name,flags|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);
    if (fd<0) return -errno;
    struct stat st;
    if (fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_nlink!=1) {close(fd);return -EINVAL;}
    return fd;
}
static int lock(const char *root,const char *boot)
{
    char filename[PATH_MAX];int rc=path(filename,root,boot,"boot-record.lock");
    if (rc) return rc;
    int fd=open_regular(filename,O_RDWR|O_CREAT);
    if (fd<0) return fd;
    if (flock(fd,LOCK_EX|LOCK_NB)) {rc=-errno;close(fd);return rc;}
    return fd;
}
static void clock_record(struct record *r)
{
    struct timespec now;
    r->wall=(unsigned long long)time(NULL);
    r->mono=!clock_gettime(CLOCK_BOOTTIME,&now)?(unsigned long long)now.tv_sec:0;
}
static int save(const char *root,const struct record *r)
{
    char state[PATH_MAX],temp[PATH_MAX],journal[PATH_MAX],older[PATH_MAX],line[RECORD_CAP];
    int rc=path(state,root,r->boot,"boot-state");
    if (!rc) rc=path(temp,root,r->boot,"boot-state.tmp");
    if (!rc) rc=path(journal,root,r->boot,"boot-events");
    if (!rc) rc=path(older,root,r->boot,"boot-events.1");
    int size=encode(r,line);
    if (rc||size<0) return rc?rc:size;
    int fd=open_regular(temp,O_WRONLY|O_CREAT);
    if (fd<0) return fd;
    if (ftruncate(fd,0)) rc=-errno;
    if (!rc) rc=write_all(fd,line,(size_t)size);
    if (!rc&&fsync(fd)) rc=-errno;
    if (close(fd)&&!rc) rc=-errno;
    if (!rc&&rename(temp,state)) rc=-errno;
    if (!rc) rc=dm_transfer_sync_parent(state);
    if (rc) return rc;
    /* The current state commits first. A journal failure is reported even if
     * state is visible; never claim the event history is complete in that case. */
    struct stat st;
    if (!lstat(journal,&st)) {
        if (!S_ISREG(st.st_mode)||st.st_nlink!=1) return -EINVAL;
        if (st.st_size>=JOURNAL_LIMIT-size&&rename(journal,older)) return -errno;
    } else if (errno!=ENOENT) return -errno;
    fd=open_regular(journal,O_RDWR|O_APPEND|O_CREAT);
    if (fd<0) return fd;
    char last;off_t length=lseek(fd,0,SEEK_END);
    if (length<0) rc=-errno;
    else if (length&&pread(fd,&last,1,length-1)!=1) rc=-EIO;
    else if (length&&last!='\n') rc=write_all(fd,"\n",1);
    if (!rc) rc=write_all(fd,line,(size_t)size);
    if (!rc&&fsync(fd)) rc=-errno;
    if (close(fd)&&!rc) rc=-errno;
    if (!rc) rc=dm_transfer_sync_parent(journal);
    return rc;
}
static void reset_label(const char *root,const char *boot,char *label)
{
    char filename[PATH_MAX],text[65537];strcpy(label,"UNKNOWN");
    if (path(filename,root,boot,"uboot.log")) return;
    int fd=open(filename,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if (fd<0) return;
    struct stat st;
    if (fstat(fd,&st)||!S_ISREG(st.st_mode)) {close(fd);return;}
    ssize_t n=read(fd,text,sizeof(text)-1);close(fd);
    if (n<=0) return;
    text[n]=0;char *value=strstr(text,"Reset reason: ");
    if (value) {
        char candidate[16];
        if (sscanf(value,"Reset reason: %15s",candidate)==1&&known(candidate,labels)) strcpy(label,candidate);
    }
}

int log_boot_start(const char *root,const char *boot,const char *previous)
{
    int fd=lock(root,boot);if (fd<0) return fd;
    struct record r={0},old;
    int rc=load(root,boot,&r);
    if (rc!=-ENOENT) {close(fd);return rc;}
    snprintf(r.boot,sizeof(r.boot),"%s",boot);
    snprintf(r.previous,sizeof(r.previous),"%s",previous?previous:"-");
    if (!safe(r.previous)) {close(fd);return -EINVAL;}
    strcpy(r.phase,"booting");strcpy(r.intent,"none");
    strcpy(r.previous_phase,"unknown");strcpy(r.previous_intent,"none");
    strcpy(r.previous_integrity,"none");strcpy(r.previous_health,"unknown");
    if (previous) {
        rc=load(root,previous,&old);
        strcpy(r.previous_integrity,!rc?"valid":rc==-ENOENT?"missing":"invalid");
        if (!rc) {strcpy(r.previous_phase,old.phase);strcpy(r.previous_intent,old.intent);strcpy(r.previous_health,old.healthy?"confirmed":"pending");}
    }
    reset_label(root,boot,r.reset_label);
    clock_record(&r);r.started_wall=r.wall;r.started_mono=r.mono;r.sequence=1;
    rc=save(root,&r);close(fd);return rc;
}
int log_boot_event(const char *root,const char *boot,const char *event)
{
    if (strcmp(event,"health-confirmed")&&strcmp(event,"shutdown")&&strcmp(event,"cancel-intent")&&
        (!known(event,intents)||!strcmp(event,"none")||!strcmp(event,"unspecified-shutdown"))) return -EINVAL;
    int fd=lock(root,boot);if (fd<0) return fd;
    struct record r={0},old;int rc=load(root,boot,&r);
    if (rc) {close(fd);return rc;}
    old=r;
    if (!strcmp(event,"health-confirmed")) {
        r.healthy=1;if (!strcmp(r.phase,"booting")) strcpy(r.phase,"healthy");
    } else if (!strcmp(event,"shutdown")) {
        strcpy(r.phase,"shutdown-started");
        if (!strcmp(r.intent,"none")) strcpy(r.intent,"unspecified-shutdown");
    } else if (!strcmp(event,"cancel-intent")) {
        if (!strcmp(r.phase,"shutdown-started")) {close(fd);return -EBUSY;}
        strcpy(r.intent,"none");strcpy(r.phase,r.healthy?"healthy":"booting");
    } else {
        if (!strcmp(r.phase,"shutdown-started")) {close(fd);return -EBUSY;}
        strcpy(r.intent,event);strcpy(r.phase,"reboot-requested");
    }
    if (old.healthy==r.healthy&&!strcmp(old.phase,r.phase)&&!strcmp(old.intent,r.intent)) {close(fd);return 0;}
    if (r.sequence==~0ULL) {close(fd);return -EOVERFLOW;}
    r.sequence++;clock_record(&r);rc=save(root,&r);close(fd);return rc;
}
static void local_time(unsigned long long epoch,char *out,size_t capacity)
{
    if(epoch<1577836800ULL||epoch>=4102444800ULL){snprintf(out,capacity,"unknown");return;}
    time_t adjusted=(time_t)epoch+8*3600;struct tm tm;
    if(!gmtime_r(&adjusted,&tm)||!strftime(out,capacity,"%Y-%m-%dT%H:%M:%S+08:00",&tm))snprintf(out,capacity,"unknown");
}
static void print_record(const struct record *r)
{
    char started[40],updated[40];local_time(r->started_wall,started,sizeof(started));local_time(r->wall,updated,sizeof(updated));
    printf("{\"boot\":\"%s\",\"phase\":\"%s\",\"health_confirmed\":%s,\"intent\":\"%s\","
           "\"previous_recorded_linux_boot\":\"%s\",\"previous_phase\":\"%s\",\"previous_intent\":\"%s\","
           "\"previous_record_integrity\":\"%s\",\"previous_health\":\"%s\",\"uboot_reset_label\":\"%s\",\"reset_cause\":\"unknown\","
           "\"sequence\":%llu,\"started_wall\":%llu,\"started_monotonic\":%llu,\"updated_wall\":%llu,\"updated_monotonic\":%llu,"
           "\"started_local\":\"%s\",\"updated_local\":\"%s\"}\n",
           r->boot,r->phase,r->healthy?"true":"false",r->intent,r->previous,r->previous_phase,r->previous_intent,
           r->previous_integrity,r->previous_health,r->reset_label,r->sequence,r->started_wall,r->started_mono,r->wall,r->mono,started,updated);
}
int log_boot_show(const char *root,const char *boot)
{
    struct record r;int rc=load(root,boot,&r);if(rc)return rc;
    print_record(&r);return 0;
}
int log_boot_history(const char *root,const char *boot)
{
    unsigned long long sequence=0;unsigned invalid=0,valid=0;
    for(int rotated=1;rotated>=0;rotated--){
        char filename[PATH_MAX],line[RECORD_CAP];
        int rc=path(filename,root,boot,rotated?"boot-events.1":"boot-events");if(rc)return rc;
        int fd=open_regular(filename,O_RDONLY);
        if(fd==-ENOENT)continue;
        if(fd<0)return fd;
        struct stat st;
        if(fstat(fd,&st)||st.st_size>JOURNAL_LIMIT){close(fd);return -EFBIG;}
        FILE *stream=fdopen(fd,"r");if(!stream){rc=-errno;close(fd);return rc;}
        while(fgets(line,sizeof(line),stream)){
            if(!strchr(line,'\n')){int c;while((c=fgetc(stream))!=EOF&&c!='\n'){}invalid++;continue;}
            struct record r;
            if(decode(line,&r)||strcmp(r.boot,boot)||r.sequence<=sequence){invalid++;continue;}
            if(sequence&&r.sequence!=sequence+1)invalid++;
            sequence=r.sequence;print_record(&r);valid++;
        }
        int failed=ferror(stream);fclose(stream);if(failed)return -EIO;
    }
    if(invalid){fprintf(stderr,"[logctl] [WARN] boot journal has %u damaged records or sequence gaps\n",invalid);return -EBADMSG;}
    return valid?0:-ENODATA;
}
int log_boot_metadata(const char *root,const char *boot,char *out,size_t capacity)
{
    struct record r;int rc=load(root,boot,&r);if (rc) return rc;
    int n=snprintf(out,capacity,"reset-cause=unknown\nuboot-reset-label=%s\nboot-phase=%s\nboot-health=%s\nsoftware-intent=%s\nprevious-recorded-linux-boot=%s\nprevious-software-intent=%s\nprevious-record-integrity=%s\nprevious-boot-health=%s\n",
        r.reset_label,r.phase,r.healthy?"confirmed":"pending",r.intent,r.previous,r.previous_intent,r.previous_integrity,r.previous_health);
    return n<0||(size_t)n>=capacity?-EOVERFLOW:0;
}
