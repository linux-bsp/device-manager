/* Bounded unicast SNTP sampling; no daemon, shell, broadcast or fallback host. */
#define _GNU_SOURCE
#include "time-sync.h"
#include "../ota/shared/time-source.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdint.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#define NS 1000000000LL
static int64_t mono(void)
{struct timespec t;if(clock_gettime(CLOCK_BOOTTIME,&t))return -1;return (int64_t)t.tv_sec*NS+t.tv_nsec;}
static uint32_t get32(const unsigned char *p)
{return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static int64_t timestamp(const unsigned char *p)
{
    if(!get32(p)&&!get32(p+4))return -1;
    int64_t sec=(int64_t)get32(p)-2208988800LL;
    /* Select the only NTP era in the supported 2020..2099 UTC range. */
    if(sec<1577836800LL)sec+=4294967296LL;
    if(sec<1577836800LL||sec>=4102444800LL)return -1;
    return sec*NS+((uint64_t)get32(p+4)*NS>>32);
}
int dm_ntp_sample(const char *source,struct dm_time_sample *sample,const volatile sig_atomic_t *cancel)
{
    char host[256],port[6];if(dm_source_parse(source,host,port)!=1)return -EINVAL;
    struct addrinfo hints={.ai_socktype=SOCK_DGRAM,.ai_family=AF_UNSPEC,.ai_flags=AI_ADDRCONFIG},*addresses=NULL;
    if(getaddrinfo(host,port,&hints,&addresses))return -EHOSTUNREACH;
    int fd=-1;
    for(struct addrinfo *a=addresses;a&&!*cancel;a=a->ai_next){
        fd=socket(a->ai_family,SOCK_DGRAM|SOCK_CLOEXEC,a->ai_protocol);
        if(fd<0)continue;
        if(!connect(fd,a->ai_addr,a->ai_addrlen))break;
        close(fd);fd=-1;
    }
    freeaddrinfo(addresses);if(fd<0)return *cancel?-ECANCELED:-EHOSTUNREACH;
    unsigned char request[48]={0},response[512];request[0]=0x23;
    int random=open("/dev/urandom",O_RDONLY|O_CLOEXEC);
    if(random<0){close(fd);return -errno;}
    ssize_t got=read(random,request+40,8);close(random);
    if(got!=8){close(fd);return -EIO;}
    int64_t start=mono();int error=-ETIMEDOUT;
    if(start<0||send(fd,request,sizeof(request),0)!=sizeof(request)){close(fd);return -EIO;}
    while(!*cancel){
        int64_t elapsed=mono()-start;if(elapsed<0||elapsed>=2*NS)break;
        struct pollfd p={.fd=fd,.events=POLLIN};int ready=poll(&p,1,100);
        if(ready<0){if(errno==EINTR)continue;error=-errno;break;}
        if(!ready)continue;
        got=recv(fd,response,sizeof(response),MSG_DONTWAIT);int64_t end=mono();
        if(got<0){error=-errno;break;}
        error=-EBADMSG;
        if(got<48||(response[0]&7)!=4||((response[0]>>3)&7)<3||((response[0]>>3)&7)>4||
           (response[0]>>6)==3||response[1]<1||response[1]>15||memcmp(response+24,request+40,8))break;
        int64_t receive=timestamp(response+32),transmit=timestamp(response+40),rtt=end-start;
        if(receive<0||transmit<receive||rtt<0||rtt>2*NS||transmit-receive>rtt+1000000)break;
        /* Remove server processing time from the path-delay estimate. */
        int64_t delay=rtt-(transmit-receive);if(delay<0)delay=0;
        sample->rtt=rtt;sample->end=end;sample->wall=transmit+delay/2;error=0;break;
    }
    close(fd);return *cancel?-ECANCELED:error;
}
