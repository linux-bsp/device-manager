#define _GNU_SOURCE
#include "fwctld-diagnostics.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

static int read_text(const char *path,char *text,size_t size)
{
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -1;
    ssize_t n=read(fd,text,size-1);int error=errno;close(fd);errno=error;
    if(n<=0)return -1;
    text[n]=0;text[strcspn(text,"\r\n")]=0;return 0;
}
int fwctld_diagnostic_reason(const char *storage,const char *directory,char reason[64])
{
    char state[64];
    if(!read_text(storage,state,sizeof(state))){
        const char *value=NULL;
        if(!strcmp(state,"unavailable"))value="data-unavailable-observed";
        if(!strcmp(state,"read-only"))value="data-read-only-observed";
        if(!strcmp(state,"low-space"))value="data-low-space-observed";
        if(value){snprintf(reason,64,"%s",value);return 1;}
    }
    if(!directory)return 0;
    DIR *dir=opendir(directory);if(!dir)return 0;
    struct dirent *entry;size_t budget=1024*1024;int found=0;
    while(!found&&budget&&(entry=readdir(dir))){
        const char *name=entry->d_name;
        if(!strncmp(name,"crash-",6))name+=6;
        if(strncmp(name,"dmesg-ramoops",13)&&strncmp(name,"console-ramoops",14))continue;
        int fd=openat(dirfd(dir),entry->d_name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
        struct stat st;if(fd<0)continue;
        if(fstat(fd,&st)||!S_ISREG(st.st_mode)){close(fd);continue;}
        char text[4160];size_t carry=0;
        while(budget){
            size_t want=budget<4096?budget:4096;
            ssize_t n=read(fd,text+carry,want);if(n<=0)break;
            budget-=(size_t)n;size_t count=carry+(size_t)n;
            if(memmem(text,count,"Kernel panic - not syncing:",26)){
                strcpy(reason,"linux-panic-observed");found=1;break;
            }
            if(memmem(text,count,"Oops:",5)){
                strcpy(reason,"linux-oops-observed");found=1;break;
            }
            carry=count<64?count:64;memmove(text,text+count-carry,carry);
        }
        close(fd);
    }
    closedir(dir);return found;
}
void fwctld_observe_failure(char reason[64],int after_reboot)
{
    const char *state="/run/device-manager/storage-state";
    if(fwctld_diagnostic_reason(state,NULL,reason))return;
    char root[1024],boot[64],directory[1152];
    if(!read_text("/run/logctl/root",root,sizeof(root))&&root[0]=='/'&&
       !read_text("/proc/sys/kernel/random/boot_id",boot,sizeof(boot))&&
       strlen(boot)==36&&strspn(boot,"0123456789abcdef-")==36){
        struct statvfs fs;
        if(!strncmp(root,"/data/",6)){
            if(statvfs(root,&fs)){strcpy(reason,"data-unreadable-observed");return;}
            if(fs.f_flag&ST_RDONLY){strcpy(reason,"data-read-only-observed");return;}
        }
        if(snprintf(directory,sizeof(directory),"%s/%s",root,boot)<(int)sizeof(directory)&&
           after_reboot&&fwctld_diagnostic_reason(state,directory,reason))return;
    }
    if(after_reboot)(void)fwctld_diagnostic_reason(state,"/sys/fs/pstore",reason);
}
