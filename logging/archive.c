#define _GNU_SOURCE
#include "archive.h"
#include "dm/transfer.h"
#include <dirent.h>
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

int log_archive_create_cancel(const char *root,const char *boot,const char *output,int (*cancelled)(void *))
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
        if(cancelled&&cancelled(NULL)){result=-ECANCELED;break;}
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

static int entry_compare(const void *left,const void *right)
{return strcmp(left,right);}

/* Closed boot snapshot identity uses file identity/size/change times. Atime is
 * excluded because archive reads update it. No symlinks or subtrees are read. */
int log_archive_index(const char *root,const char *boot,char *index,unsigned capacity)
{
    char path[PATH_MAX],names[64][129];size_t count=0,used=0;uint64_t total=0;
    if(!capacity||!*boot||strchr(boot,'/')||*boot=='.'||*boot=='-')return -EINVAL;
    if(snprintf(path,sizeof(path),"%s/%s",root,boot)>=(int)sizeof(path))return -ENAMETOOLONG;
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -errno;
    DIR *dir=fdopendir(fd);if(!dir){int error=errno;close(fd);return -error;}
    struct dirent *entry;int result=0;
    for(;;){
        errno=0;entry=readdir(dir);if(!entry){if(errno)result=-errno;break;}
        if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")||!strcmp(entry->d_name,".archive-receipt")||!strcmp(entry->d_name,".archive-receipt.tmp")||!strcmp(entry->d_name,".archive-unavailable")||!strcmp(entry->d_name,".archive-unavailable.tmp"))continue;
        if(count==64||strlen(entry->d_name)>128||entry->d_name[0]=='.'){result=-EINVAL;break;}
        for(const char *p=entry->d_name;*p;p++)
            if(!((*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='.'||*p=='-'||*p=='_')){result=-EINVAL;break;}
        if(result)break;
        strcpy(names[count++],entry->d_name);
    }
    if(!result&&!count)result=-ENODATA;
    qsort(names,count,sizeof(names[0]),entry_compare);
    for(size_t i=0;!result&&i<count;i++){
        struct stat st;
        if(fstatat(fd,names[i],&st,AT_SYMLINK_NOFOLLOW)){result=-errno;break;}
        if(!S_ISREG(st.st_mode)||st.st_nlink!=1||st.st_size<0||st.st_size>4*1024*1024){result=-EINVAL;break;}
        total+=(uint64_t)st.st_size;if(total>32*1024*1024){result=-EFBIG;break;}
        int n=snprintf(index+used,capacity-used,"%s %llu %llu %llu %lld.%09ld %lld.%09ld %o\n",names[i],
            (unsigned long long)st.st_dev,(unsigned long long)st.st_ino,(unsigned long long)st.st_size,
            (long long)st.st_mtim.tv_sec,st.st_mtim.tv_nsec,(long long)st.st_ctim.tv_sec,st.st_ctim.tv_nsec,(unsigned)st.st_mode);
        if(n<0||(size_t)n>=capacity-used){result=-EOVERFLOW;break;}
        used+=(size_t)n;
    }
    closedir(dir);return result;
}

int log_archive_manifest_read(const char *path,struct log_archive_manifest *manifest)
{
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return -errno;
    char text[LOG_ARCHIVE_INDEX_CAP+1024];struct stat st;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<=0||st.st_size>=(off_t)sizeof(text)){close(fd);return -EINVAL;}
    size_t used=0;
    while(used<(size_t)st.st_size){ssize_t n=read(fd,text+used,(size_t)st.st_size-used);if(n<0&&errno==EINTR)continue;if(n<=0){close(fd);return -EIO;}used+=(size_t)n;}
    close(fd);text[used]=0;if(memchr(text,0,used))return -EINVAL;
    char *newline=strchr(text,'\n'),extra,tag[16];unsigned long long size;
    if(!newline)return -EINVAL;
    *newline++=0;
    if(sscanf(text,"%15s %511s %63s %63s %64s %llu%c",tag,manifest->server,manifest->device,manifest->boot,manifest->sha,&size,&extra)!=6||
       strcmp(tag,"DMBUNDLE1")||!size||size>32*1024*1024||strlen(manifest->sha)!=64||strlen(newline)>=sizeof(manifest->index)||!*newline)return -EINVAL;
    for(const char *p=manifest->sha;*p;p++)if(!((*p>='a'&&*p<='f')||(*p>='0'&&*p<='9')))return -EINVAL;
    manifest->size=size;strcpy(manifest->index,newline);return 0;
}

int log_archive_manifest_write(const char *path,const struct log_archive_manifest *manifest)
{
    char temp[PATH_MAX],text[LOG_ARCHIVE_INDEX_CAP+1024];
    if(snprintf(temp,sizeof(temp),"%s.tmp",path)>=(int)sizeof(temp))return -ENAMETOOLONG;
    int n=snprintf(text,sizeof(text),"DMBUNDLE1 %s %s %s %s %llu\n%s",manifest->server,manifest->device,manifest->boot,manifest->sha,(unsigned long long)manifest->size,manifest->index);
    if(n<0||n>=(int)sizeof(text))return -EOVERFLOW;
    int fd=open(temp,O_WRONLY|O_CREAT|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);
    if(fd<0)return -errno;
    struct stat st;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_nlink!=1){close(fd);return -EINVAL;}
    if(ftruncate(fd,0)){int error=errno;close(fd);return -error;}
    int rc=0;
    for(int written=0;written<n;){ssize_t k=write(fd,text+written,(size_t)(n-written));if(k<0&&errno==EINTR)continue;if(k<=0){rc=errno?-errno:-EIO;break;}written+=(int)k;}
    if(!rc&&fsync(fd))rc=-errno;
    if(close(fd)&&!rc)rc=-errno;
    if(!rc&&rename(temp,path))rc=-errno;
    if(!rc)rc=dm_transfer_sync_parent(path);
    unlink(temp);return rc;
}

int log_archive_receipted(const char *root,const char *boot)
{
    char path[PATH_MAX],index[LOG_ARCHIVE_INDEX_CAP];struct log_archive_manifest manifest;
    if(snprintf(path,sizeof(path),"%s/%s/.archive-receipt",root,boot)>=(int)sizeof(path))return 0;
    if(log_archive_manifest_read(path,&manifest)||strcmp(manifest.boot,boot)||log_archive_index(root,boot,index,sizeof(index)))return 0;
    return !strcmp(index,manifest.index);
}

int log_archive_create(const char *root,const char *boot,const char *output)
{return log_archive_create_cancel(root,boot,output,NULL);}
