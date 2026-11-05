#define _GNU_SOURCE
#include "config.h"
#include "../ota/shared/time-source.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

const char *time_run_directory(void)
{
    const char *path=getenv("TIMECTL_RUN_DIR");
    return path?path:"/run/timectl";
}
static int identifier(const char *value)
{
    return *value&&strlen(value)<64&&strspn(value,"abcdefghijklmnopqrstuvwxyz0123456789-_")==strlen(value);
}
static int read_line(const char *path,char *value,size_t capacity)
{
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return -errno;
    char buffer[256];ssize_t n=read(fd,buffer,sizeof(buffer)-1);int saved=errno;close(fd);
    if(n<0)return -saved;
    if((size_t)n==sizeof(buffer)-1)return -EOVERFLOW;
    buffer[n]=0;buffer[strcspn(buffer,"\r\n")]=0;
    if(strlen(buffer)>=capacity)return -EOVERFLOW;
    strcpy(value,buffer);return 0;
}
int time_config_load(struct time_config *config)
{
    memset(config,0,sizeof(*config));
    strcpy(config->interface,"eth0");strcpy(config->source,"-");
    config->enabled=1;config->interval=3600;
    const char *path=getenv("TIMECTL_CONFIG");if(!path)path="/etc/device-manager/time.conf";
    FILE *f=fopen(path,"r");if(!f)return -errno;
    char line[1024],auth[8]="key",keyfile[1024]="";
    int rc=0;
    while(fgets(line,sizeof(line),f)){
        if(!strchr(line,'\n')&&!feof(f)){rc=-EOVERFLOW;break;}
        line[strcspn(line,"\r\n")]=0;
        if(!*line||*line=='#')continue;
        char *value=strchr(line,'=');if(!value){rc=-EINVAL;break;}*value++=0;
        char *dst=NULL;size_t capacity=0;
#define FIELD(name, target) if(!strcmp(line,name)){dst=(target);capacity=sizeof(target);}
        FIELD("server",config->server)
        else FIELD("source",config->source)
        else FIELD("interface",config->interface)
        else FIELD("device",config->device)
        else FIELD("auth",auth)
        else FIELD("key-file",keyfile)
        else if(!strcmp(line,"enabled")){
            if(strcmp(value,"0")&&strcmp(value,"1")){rc=-EINVAL;break;}
            config->enabled=*value=='1';continue;
        }else if(!strcmp(line,"interval")){
            char *end;errno=0;unsigned long v=strtoul(value,&end,10);
            if(errno||!*value||*end||v<60||v>86400){rc=-EINVAL;break;}
            config->interval=(unsigned)v;continue;
        }else {rc=-EINVAL;break;}
#undef FIELD
        if(strlen(value)>=capacity){rc=-EOVERFLOW;break;}strcpy(dst,value);
    }
    if(ferror(f))rc=-EIO;
    fclose(f);if(rc)return rc;
    size_t n=strlen(config->server);if(n&&config->server[n-1]=='/')config->server[n-1]=0;
    if((strncmp(config->server,"http://",7)&&strncmp(config->server,"https://",8))||
       !dm_source_valid(config->server)||!dm_source_valid(config->source)||!identifier(config->interface))return -EINVAL;
    if(!*config->device){
        char mac[160];snprintf(mac,sizeof(mac),"/sys/class/net/%s/address",config->interface);
        if((rc=read_line(mac,config->device,sizeof(config->device))))return rc;
        for(char *p=config->device;*p;p++)if(*p==':')*p='-';
    }
    if(!identifier(config->device)||!strcmp(config->device,"viewer"))return -EINVAL;
    if(!strcmp(auth,"none"))return 0;
    struct stat st;
    if(strcmp(auth,"key")||*keyfile!='/'||lstat(keyfile,&st)||!S_ISREG(st.st_mode)||
       (st.st_mode&0077)||st.st_uid!=geteuid())return -EACCES;
    if((rc=read_line(keyfile,config->key,sizeof(config->key))))return rc;
    return strlen(config->key)==64&&strspn(config->key,"0123456789abcdef")==64?0:-EINVAL;
}
