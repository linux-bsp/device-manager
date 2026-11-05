/* LAN clock calibration, independent of local collection and OTA installation. */
#define _GNU_SOURCE
#include "time-sync.h"
#include "../ota/shared/time-source.h"
#include <arpa/inet.h>
#include "dm/transfer.h"
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/rtc.h>
#include <net/if.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define NS 1000000000LL
static int64_t next_attempt,last_sync,anchor_wall,anchor_mono;
static int warned,last_state;
static volatile sig_atomic_t cancelled;
void dm_time_sync_cancel(void){cancelled=1;}
static int transfer_cancelled(void *context) { (void)context; return cancelled != 0; }
static char last_rtc[24]="unavailable";
struct response {char data[160];size_t length;};
static unsigned sync_interval=3600;
static char previous_source[512];
void dm_time_interval(unsigned seconds){sync_interval=seconds;}
static int64_t now_ns(clockid_t clock)
{
    struct timespec t;if(clock_gettime(clock,&t))return -1;
    return (int64_t)t.tv_sec*NS+t.tv_nsec;
}
static int random_nonce(char nonce[33])
{
    unsigned char bytes[16];size_t got=0;
    int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -errno;
    while(got<sizeof(bytes)){
        ssize_t n=read(fd,bytes+got,sizeof(bytes)-got);
        if(n<0&&errno==EINTR)continue;
        if(n<=0){close(fd);return -EIO;}got+=(size_t)n;
    }
    close(fd);for(int i=0;i<16;i++)snprintf(nonce+i*2,3,"%02x",bytes[i]);return 0;
}
static int request_http_sample(const char *server,struct dm_time_sample *sample)
{
    char nonce[33],returned[33],url[1024];int ret=random_nonce(nonce);if(ret)return ret;
    if(snprintf(url,sizeof(url),"%s/api/time/%s",server,nonce)>=(int)sizeof(url))return -ENAMETOOLONG;
    struct response response={0};
    struct dm_transfer_request request;struct dm_transfer_result result;
    dm_transfer_request_init(&request);
    request.url=url;request.response=response.data;request.response_capacity=sizeof(response.data);
    request.connect_timeout_ms=1000;request.timeout_ms=2000;request.callbacks.cancelled=transfer_cancelled;
    int64_t start=now_ns(CLOCK_BOOTTIME);
    int exchange=dm_transfer_perform(&request,&result);
    int64_t end=now_ns(CLOCK_BOOTTIME);response.length=result.response_size;
    if(cancelled)return -ECANCELED;
    if(exchange||result.http_status!=200)return -EHOSTUNREACH;
    if(start<0||end<start||end-start>2*NS)return -ETIMEDOUT;
    long long seconds;long nanoseconds;int consumed=0;
    if(memchr(response.data,0,response.length)||
       sscanf(response.data,"DMTIME1 %32s %lld %ld%n",returned,&seconds,&nanoseconds,&consumed)!=3||
       strcmp(returned,nonce)||strcmp(response.data+consumed,"\n")||
       seconds<1577836800LL||seconds>=4102444800LL||nanoseconds<0||nanoseconds>=NS)return -EBADMSG;
    sample->rtt=end-start;sample->end=end;sample->wall=seconds*NS+nanoseconds+sample->rtt/2;return 0;
}
static int network_ready(const char *interface)
{
    struct ifaddrs *list;if(getifaddrs(&list))return 0;int ready=0;
    for(struct ifaddrs *p=list;p;p=p->ifa_next){
        if(!p->ifa_addr||strcmp(p->ifa_name,interface)||!(p->ifa_flags&IFF_UP)||!(p->ifa_flags&IFF_RUNNING))continue;
        if(p->ifa_addr->sa_family==AF_INET&&((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr!=INADDR_ANY)ready=1;
        if(p->ifa_addr->sa_family==AF_INET6&&!IN6_IS_ADDR_UNSPECIFIED(&((struct sockaddr_in6 *)p->ifa_addr)->sin6_addr))ready=1;
    }
    freeifaddrs(list);return ready;
}
static int state_path(char *path,size_t size,const char *name)
{
    const char *root=getenv("LOGCTL_RUN_DIR");if(!root)root="/run/logctl";
    return snprintf(path,size,"%s/%s",root,name)>=(int)size?-1:0;
}
static void status_file(const char *state,const char *server,long long offset,long long uncertainty)
{
    char path[1024],temp[1024];if(state_path(path,sizeof(path),"clock-status")||state_path(temp,sizeof(temp),".clock-status.tmp"))return;
    int fd=open(temp,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return;
    char when[48]="unknown",zone[16]="unknown";
    time_t seconds=last_sync?(time_t)last_sync:time(NULL);struct tm local;
    if(localtime_r(&seconds,&local)){
        strftime(zone,sizeof(zone),"%z",&local);
        if(last_sync)strftime(when,sizeof(when),"%Y-%m-%dT%H:%M:%S%z",&local);
    }
    int ret=dprintf(fd,"clock-state=%s\nclock-source=%s\nclock-server=%s\nclock-synced-at=%lld\nclock-offset-ms=%lld\nclock-uncertainty-ms=%lld\nrtc-state=%s\nclock-timezone=%s\nclock-synced-local=%s\n",state,!strncmp(server,"ntp://",6)?"ntp":"device-manager",server,(long long)last_sync,offset,uncertainty,last_rtc,zone,when);
    int failed=ret<0||fsync(fd);if(close(fd))failed=1;
    if(failed||rename(temp,path))unlink(temp);
}
static void persist_rtc(void)
{
    int fd=open("/dev/rtc0",O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0){strcpy(last_rtc,"unavailable");return;}
    time_t seconds=time(NULL);struct tm utc;struct rtc_time rtc;
    if(!gmtime_r(&seconds,&utc)){close(fd);strcpy(last_rtc,"unavailable");return;}
    memset(&rtc,0,sizeof(rtc));rtc.tm_sec=utc.tm_sec;rtc.tm_min=utc.tm_min;rtc.tm_hour=utc.tm_hour;
    rtc.tm_mday=utc.tm_mday;rtc.tm_mon=utc.tm_mon;rtc.tm_year=utc.tm_year;rtc.tm_wday=utc.tm_wday;rtc.tm_yday=utc.tm_yday;
    strcpy(last_rtc,ioctl(fd,RTC_SET_TIME,&rtc)?"write-failed":"written");close(fd);
}
void dm_time_disabled(const char *source)
{ status_file("disabled",source,0,0); }
int dm_time_sync(const char *server,const char *interface,int force)
{
    if(!dm_source_valid(server)||!strcmp(server,"-"))return -EINVAL;
    if(strcmp(previous_source,server)){
        strcpy(previous_source,server);next_attempt=last_sync=anchor_wall=anchor_mono=0;warned=last_state=0;
    }
    if(cancelled)return -ECANCELED;
    int64_t mono=now_ns(CLOCK_BOOTTIME),wall=now_ns(CLOCK_REALTIME);
    if(mono<0||wall<0)return -EIO;
    /* Detect an external clock step and promptly recheck this configured source. */
    if(last_sync&&llabs((wall-anchor_wall)-(mono-anchor_mono))>2*NS)next_attempt=0;
    if(!force&&mono<next_attempt)return 0;
    if(!network_ready(interface)){
        if(last_state!=1)status_file(last_sync?"holdover":"waiting-network",server,0,0);
        last_state=1;return -EAGAIN;
    }
    char path[1024];if(state_path(path,sizeof(path),".clock-lock"))return -ENAMETOOLONG;
    int lock=open(path,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);if(lock<0)return -errno;
    if(flock(lock,LOCK_EX|LOCK_NB)){close(lock);return -EAGAIN;}
    struct dm_time_sample best={.rtt=INT64_MAX};int error=-EHOSTUNREACH;
    for(int i=0;i<3&&!cancelled;i++){
        struct dm_time_sample sample;int ret=!strncmp(server,"ntp://",6)?dm_ntp_sample(server,&sample,&cancelled):request_http_sample(server,&sample);
        if(!ret&&sample.rtt<best.rtt)best=sample;
        else if(ret)error=ret;
    }
    if(cancelled){close(lock);return -ECANCELED;}
    if(best.rtt!=INT64_MAX){
        mono=now_ns(CLOCK_BOOTTIME);wall=now_ns(CLOCK_REALTIME);
        int64_t target=best.wall+mono-best.end,offset=target-wall;
        struct timespec desired={.tv_sec=target/NS,.tv_nsec=target%NS};
        if(mono<0||wall<0)error=-EIO;
        else if(llabs(offset)>100000000&&clock_settime(CLOCK_REALTIME,&desired))error=-errno;
        else {
            anchor_wall=now_ns(CLOCK_REALTIME);anchor_mono=now_ns(CLOCK_BOOTTIME);last_sync=anchor_wall/NS;
            persist_rtc();status_file("synchronized",server,offset/1000000,(best.rtt+1999999)/2000000);
            next_attempt=anchor_mono+(int64_t)sync_interval*NS;last_state=2;warned=0;
            char when[48];time_t seconds=(time_t)last_sync;struct tm local;
            localtime_r(&seconds,&local);strftime(when,sizeof(when),"%Y-%m-%dT%H:%M:%S%z",&local);
            fprintf(stderr,"[logctld] [INFO] time synchronized: %s source=%s rtc=%s\n",when,server,last_rtc);
            close(lock);return 0;
        }
    }
    status_file(last_sync?"holdover":"unsynchronized",server,0,0);last_state=3;
    next_attempt=now_ns(CLOCK_BOOTTIME)+30*NS;
    if(!warned++)fprintf(stderr,"[logctld] [WARN] time sync failed: %s; retrying in 30 seconds\n",strerror(-error));
    close(lock);return error;
}
