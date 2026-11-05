/* One clock owner. Local CLI requests use bounded, typed Unix packets. */
#define _GNU_SOURCE
#include "config.h"
#include "time-control.h"
#include "time-sync.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static void stop(int signal)
{
    (void)signal;stopping=1;dm_clock_cancel();dm_time_sync_cancel();
}
static long long monotonic_ms(void)
{
    struct timespec now;
    return clock_gettime(CLOCK_BOOTTIME,&now)?-1:(long long)now.tv_sec*1000+now.tv_nsec/1000000;
}
static int wait_fd(int fd,short events,long long deadline)
{
    while(!stopping){
        long long now=monotonic_ms(),left=deadline-now;
        if(now<0||left<=0)return -ETIMEDOUT;
        struct pollfd p={.fd=fd,.events=events};
        int rc=poll(&p,1,left>1000?1000:(int)left);
        if(rc<0){if(errno==EINTR)continue;return -errno;}
        if(rc>0)return p.revents&events?0:-ECONNRESET;
    }
    return -ECANCELED;
}
static int address(struct sockaddr_un *addr)
{
    memset(addr,0,sizeof(*addr));addr->sun_family=AF_UNIX;
    int n=snprintf(addr->sun_path,sizeof(addr->sun_path),"%s/control.sock",time_run_directory());
    return n<0||n>=(int)sizeof(addr->sun_path)?-ENAMETOOLONG:0;
}
static int command(const char *request)
{
    struct sockaddr_un addr;int rc=address(&addr);if(rc)return rc;
    int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if(fd<0)return -errno;
    long long deadline=monotonic_ms()+15000;
    if(connect(fd,(struct sockaddr *)&addr,sizeof(addr))){rc=-errno;goto done;}
    rc=wait_fd(fd,POLLOUT,deadline);if(rc)goto done;
    if(send(fd,request,strlen(request),MSG_NOSIGNAL)!=(ssize_t)strlen(request)){rc=-EIO;goto done;}
    rc=wait_fd(fd,POLLIN,deadline);if(rc)goto done;
    char response[2048];ssize_t n=recv(fd,response,sizeof(response)-1,MSG_TRUNC);
    if(n<=0||n>=(ssize_t)sizeof(response)||memchr(response,0,(size_t)n)){rc=-EPROTO;goto done;}
    response[n]=0;char *end;
    errno=0;long error=strtol(response,&end,10);
    if(errno||end==response||*end!='\n'||error<0||error>4095){rc=-EPROTO;goto done;}
    rc=-(int)error;
    if(end[1])fputs(end+1,rc?stderr:stdout);
done:
    close(fd);return rc;
}
static void serve(int listener,const struct time_config *config)
{
    int fd=accept4(listener,NULL,NULL,SOCK_CLOEXEC|SOCK_NONBLOCK);if(fd<0)return;
    struct ucred peer;socklen_t size=sizeof(peer);
    int rc=getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&size)?-errno:0;
    if(!rc&&peer.uid!=geteuid())rc=-EACCES;
    if(!rc)rc=wait_fd(fd,POLLIN,monotonic_ms()+100);
    char request[640],body[1700]="",response[1800];
    if(!rc){
        ssize_t n=recv(fd,request,sizeof(request)-1,MSG_TRUNC);
        if(n<=0||n>=(ssize_t)sizeof(request)||memchr(request,0,(size_t)n))rc=-EINVAL;
        else {
            request[n]=0;
            if(!strcmp(request,"status"))rc=dm_clock_status(config->server,body,sizeof(body));
            else if(!strcmp(request,"sync")){
                rc=dm_clock_sync_now(config->server,config->interface);
                if(!rc)snprintf(body,sizeof(body),"[timectl] [INFO] time synchronization completed\n");
            }else if(!strncmp(request,"source ",7)){
                rc=dm_clock_set_source(request+7);
                if(!rc)snprintf(body,sizeof(body),"[timectl] [INFO] source configuration saved; automatic sync follows the enabled policy\n");
            }else rc=-EINVAL;
        }
    }
    if(rc)body[0]=0;
    int n=snprintf(response,sizeof(response),"%d\n%s",rc<0?-rc:0,body);
    if(n>0&&n<(int)sizeof(response))(void)send(fd,response,(size_t)n,MSG_NOSIGNAL);
    close(fd);
}
static int daemon_main(void)
{
    struct time_config config;int rc=time_config_load(&config);if(rc)return rc;
    const char *run=time_run_directory();struct stat st;
    if(mkdir(run,0700)&&errno!=EEXIST)return -errno;
    if(lstat(run,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&0077))return -EACCES;
    char path[1024];
    if(snprintf(path,sizeof(path),"%s/daemon.lock",run)>=(int)sizeof(path))return -ENAMETOOLONG;
    int lock=open(path,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);if(lock<0)return -errno;
    if(flock(lock,LOCK_EX|LOCK_NB)){close(lock);return -EBUSY;}
    struct sockaddr_un addr;rc=address(&addr);if(rc){close(lock);return rc;}
    int listener=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if(listener<0){rc=-errno;close(lock);return rc;}
    unlink(addr.sun_path);
    if(bind(listener,(struct sockaddr *)&addr,sizeof(addr))||listen(listener,8)){rc=-errno;goto done;}
    snprintf(path,sizeof(path),"%s/clock-status",run);unlink(path);
    rc=dm_clock_init(config.enabled,config.interval,config.source);
    if(rc)fprintf(stderr,"[timectld] [WARN] saved time configuration unavailable: %s\n",strerror(-rc));
    fprintf(stderr,"[timectld] [INFO] Started; independent time synchronization and reporting\n");
    long long next=0;rc=0;
    while(!stopping){
        long long now=monotonic_ms();if(now<0){rc=-EIO;break;}
        if(now>=next){
            dm_clock_cycle(config.server,config.interface,config.device,config.key);
            next=monotonic_ms()+5000;
        }
        struct pollfd p={.fd=listener,.events=POLLIN};
        int ready=poll(&p,1,200);
        if(ready<0&&errno!=EINTR){rc=-errno;break;}
        if(ready>0&&(p.revents&POLLIN)&&!stopping)serve(listener,&config);
    }
done:
    close(listener);unlink(addr.sun_path);close(lock);return rc;
}
int main(int argc,char **argv)
{
    umask(077);signal(SIGTERM,stop);signal(SIGINT,stop);signal(SIGPIPE,SIG_IGN);
    const char *name=strrchr(argv[0],'/');name=name?name+1:argv[0];
    int rc;
    if(!strcmp(name,"timectld")&&argc==1)rc=daemon_main();
    else if(argc==2&&(!strcmp(argv[1],"status")||!strcmp(argv[1],"sync")))rc=command(argv[1]);
    else if(argc==4&&!strcmp(argv[1],"source")&&!strcmp(argv[2],"set")){
        char request[640];int n=snprintf(request,sizeof(request),"source %s",argv[3]);
        rc=n<0||n>=(int)sizeof(request)?-E2BIG:command(request);
    }else {fprintf(stderr,"Usage: timectl status | sync | source set <ntp://host|http[s]://host|->\n       timectld\n");return 2;}
    if(rc)fprintf(stderr,"[%s] [ERROR] %s\n",name,strerror(-rc));
    return rc?1:0;
}
