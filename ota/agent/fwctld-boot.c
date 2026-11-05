#define _POSIX_C_SOURCE 200809L
#include "fwctld-boot.h"
#include "ota-common.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static pid_t unreaped = -1;
static long long now_ms(void)
{
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC,&now)?-1:(long long)now.tv_sec*1000+now.tv_nsec/1000000;
}

int fwctld_boot_event(const char *event)
{
    int status;
    if(unreaped>0){
        pid_t rc=waitpid(unreaped,&status,WNOHANG);
        if(rc==0)return -EBUSY;
        if(rc<0&&errno!=ECHILD)return -errno;
        unreaped=-1;
    }
    long long started=now_ms();
    if(started<0)return -EIO;
    if(ota_stop)return -EINTR;
    pid_t child=fork();
    if(child<0)return -errno;
    if(!child){
        if(setpgid(0,0))_exit(126);
        int fd=open("/dev/null",O_RDWR);
        if(fd<0||dup2(fd,0)<0||dup2(fd,1)<0||dup2(fd,2)<0)_exit(126);
        if(fd>2)close(fd);
        execlp("logctl","logctl","boot-event",event,NULL);
        _exit(errno==ENOENT?127:126);
    }
    (void)setpgid(child,child);
    for(;;){
        pid_t rc=waitpid(child,&status,WNOHANG);
        if(rc==child)return WIFEXITED(status)&&(WEXITSTATUS(status)==0||WEXITSTATUS(status)==127)?0:-EIO;
        if(rc<0&&errno!=EINTR)return -errno;
        long long now=now_ms();
        if(ota_stop||now<0||now-started>=3000)break;
        struct timespec pause={.tv_nsec=10000000};nanosleep(&pause,NULL);
    }
    (void)kill(-child,SIGKILL);
    /* A task stuck in uninterruptible kernel I/O cannot be waited on forever.
     * Keep at most one unreaped child; a later call reaps it before spawning. */
    for(int attempt=0;attempt<10;attempt++){
        pid_t rc=waitpid(child,&status,WNOHANG);
        if(rc==child||(rc<0&&errno==ECHILD))return ota_stop?-EINTR:-ETIMEDOUT;
        struct timespec pause={.tv_nsec=10000000};nanosleep(&pause,NULL);
    }
    unreaped=child;
    return ota_stop?-EINTR:-ETIMEDOUT;
}
