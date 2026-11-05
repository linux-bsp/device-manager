#define _GNU_SOURCE
#include "archive.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long long monotonic_ms(void)
{
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC,&now))return -1;
    return (long long)now.tv_sec*1000+now.tv_nsec/1000000;
}

int log_archive_create(const char *root,const char *boot,const char *output)
{
    char parent[PATH_MAX],resolved[PATH_MAX],source[PATH_MAX],temp[PATH_MAX];
    if(output[0]!='/'||strlen(output)>=sizeof(parent)||!realpath(root,source))return -EINVAL;
    strcpy(parent,output);char *slash=strrchr(parent,'/');
    if(!slash[1])return -EINVAL;
    if(slash==parent)parent[1]=0;else *slash=0;
    if(!realpath(parent,resolved))return -errno;
    size_t length=strlen(source);
    if(!strcmp(source,"/")||(!strncmp(resolved,source,length)&&(!resolved[length]||resolved[length]=='/')))return -EINVAL;
    if(!*boot||*boot=='.'||*boot=='-'||strchr(boot,'/')||strlen(boot)>63)return -EINVAL;
    if(snprintf(temp,sizeof(temp),"%s.tmp.XXXXXX",output)>=(int)sizeof(temp))return -ENAMETOOLONG;
    int fd=mkostemp(temp,O_CLOEXEC);if(fd<0)return -errno;
    int result=0,status=0;
    pid_t child=fork();
    if(!child){
        if(setpgid(0,0)||dup2(fd,STDOUT_FILENO)<0)_exit(126);
        close(fd);
        execlp("tar","tar","-czf","-","-C",source,boot,NULL);
        _exit(127);
    }
    if(child<0){result=-errno;goto out;}
    (void)setpgid(child,child);
    long long deadline=monotonic_ms()+30000;
    for(;;){
        pid_t waited=waitpid(child,&status,WNOHANG);
        if(waited==child)break;
        if(waited<0&&errno!=EINTR){result=-errno;break;}
        struct stat st;
        if(fstat(fd,&st)){result=-errno;break;}
        if(st.st_size>32*1024*1024){result=-EFBIG;break;}
        if(monotonic_ms()>=deadline){result=-ETIMEDOUT;break;}
        usleep(10000);
    }
    if(result){
        (void)kill(-child,SIGKILL);
        while(waitpid(child,&status,0)<0&&errno==EINTR){}
        goto out;
    }
    if(!WIFEXITED(status)||WEXITSTATUS(status)){result=-EIO;goto out;}
    struct stat st;
    if(fstat(fd,&st)||st.st_size<=0||st.st_size>32*1024*1024){result=-EFBIG;goto out;}
    if(fsync(fd)){result=-errno;goto out;}
    if(rename(temp,output)){result=-errno;goto out;}
    int dir=open(resolved,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(dir<0)result=-errno;
    else {if(fsync(dir))result=-errno;close(dir);}
out:
    close(fd);unlink(temp);return result;
}
