/* Send finite startup snapshots; RAM ACK follows server archive commit only. */
#define _GNU_SOURCE
#include "startup-forward.h"
#include "dm/transfer.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *stages[]={"r5-spl","a53-spl","uboot","kernel"};
static const unsigned limits[]={32736,32736,65504,327648};
struct bank {
    unsigned slot,current,length[4],crc[4],dropped[4],state[4];
    unsigned long long generation;
    char id[40];
};
static uint32_t checksum(uint32_t crc,const void *data,size_t length)
{
    const unsigned char *p=data;crc=~crc;
    while(length--){crc^=*p++;for(int i=0;i<8;i++)crc=(crc>>1)^((0U-(crc&1U))&0xedb88320U);}
    return ~crc;
}
static int uuid(const char *s)
{
    if(strlen(s)!=36)return 0;
    for(int i=0;i<36;i++)if(i==8||i==13||i==18||i==23){if(s[i]!='-')return 0;}
    else if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return 0;
    return 1;
}
static int control(const char *base,const char *name,const char *value)
{
    char path[1024];if(snprintf(path,sizeof(path),"%s/%s",base,name)>=(int)sizeof(path))return -1;
    int fd=open(path,O_WRONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -1;
    size_t n=strlen(value);ssize_t written=write(fd,value,n);int error=errno;
    if(close(fd)&&written==(ssize_t)n)return -1;
    errno=error;return written==(ssize_t)n?0:-1;
}
static int read_banks(const char *base,struct bank banks[2])
{
    char path[1024],line[256],name[32],extra;unsigned count=0;
    if(snprintf(path,sizeof(path),"%s/banks",base)>=(int)sizeof(path))return -1;
    FILE *f=fopen(path,"re");if(!f)return -1;
    while(fgets(line,sizeof(line),f)){
        if(count>=2)goto fail;
        struct bank *b=&banks[count];
        if(sscanf(line,"%u %39s %llu %u %c",&b->slot,b->id,&b->generation,&b->current,&extra)!=4||
           b->slot>1||!uuid(b->id)||!b->generation||b->current>1||
           (count&&banks[0].slot==b->slot))goto fail;
        for(int i=0;i<4;i++){
            if(!fgets(line,sizeof(line),f)||sscanf(line,"%31s %u %x %u %u %c",name,&b->length[i],&b->crc[i],&b->dropped[i],&b->state[i],&extra)!=5||
               strcmp(name,stages[i])||b->length[i]>limits[i]||b->state[i]>3||
               ((b->state[i]==0||b->state[i]==2)&&(b->length[i]||b->crc[i])))goto fail;
        }
        count++;
    }
    if(ferror(f))goto fail;
    fclose(f);return (int)count;
fail:
    fclose(f);return -1;
}
struct startup_push { const char *device;const struct bank *bank;int stage;startup_post_fn post; };
static int startup_chunk(void *context,uint64_t offset,const void *data,size_t length,uint64_t *next)
{
    struct startup_push *s=context;char endpoint[256];long long acknowledged;
    snprintf(endpoint,sizeof(endpoint),"/api/sol/startup/%s/%s/%s/%llu",s->device,s->bank->id,
             stages[s->stage],(unsigned long long)offset);
    int rc=s->post(endpoint,data,length,&acknowledged);
    if(!rc&&acknowledged<0)return -EPROTO;
    if(!rc)*next=(uint64_t)acknowledged;
    return rc;
}
static int stream(const char *base,const char *device,const struct bank *b,int stage,startup_post_fn post)
{
    char path[1024],buffer[4096];
    snprintf(path,sizeof(path),"%s/bank%u-%s",base,b->slot,stages[stage]);
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -1;
    uint32_t crc=0;unsigned total=0;
    for(;;){
        ssize_t n=read(fd,buffer,sizeof(buffer));if(n<0&&errno==EINTR)continue;
        if(n<0){close(fd);return -1;}if(!n)break;
        if((size_t)n>b->length[stage]-total){close(fd);return -1;}
        crc=checksum(crc,buffer,(size_t)n);total+=(unsigned)n;
    }
    if(total!=b->length[stage]||crc!=b->crc[stage]){close(fd);return -1;}
    struct startup_push context={device,b,stage,post};
    struct dm_transfer_push push={.fd=fd,.size=total,.chunk_size=4096,.exchange=startup_chunk,.context=&context};
    uint64_t next;int rc=dm_transfer_push_file(&push,&next);close(fd);return rc;
}
int startup_forward(const char *device,startup_post_fn post)
{
    const char *base=getenv("LOGCTL_HANDOFF_DIR");if(!base)base="/sys/firmware/bootlog";
    const char *health=getenv("LOGCTL_BOOT_HEALTH");if(!health)health="/run/fwctl-boot-confirmed";
    char path[1024];snprintf(path,sizeof(path),"%s/banks",base);
    if(access(path,F_OK))return errno==ENOENT?0:-1; /* Older board ABI. */
    if(access(health,R_OK))return 0;
    if(control(base,"seal","freeze\n"))return -1;
    struct bank banks[2]={0};int count=read_banks(base,banks);
    if(count<=0)return count;
    char recovery[64];int fd=open("/proc/sys/kernel/random/boot_id",O_RDONLY|O_CLOEXEC);
    if(fd<0)return -1;
    ssize_t n=read(fd,recovery,sizeof(recovery)-1);close(fd);if(n<0)return -1;
    recovery[n]=0;recovery[strcspn(recovery,"\r\n")]=0;if(!uuid(recovery))return -1;
    char ack[96],ids[2][40]={"-","-"};
    for(int i=0;i<count;i++){
        struct bank *b=&banks[i];
        for(int stage=0;stage<4;stage++)if(stream(base,device,b,stage,post))return -1;
        char manifest[768],endpoint[256];
        int length=snprintf(manifest,sizeof(manifest),"%u %llu %u %s\n",b->slot,b->generation,b->current,recovery);
        for(int stage=0;stage<4;stage++)length+=snprintf(manifest+length,sizeof(manifest)-(size_t)length,"%s %u %08x %u %u\n",stages[stage],b->length[stage],b->crc[stage],b->dropped[stage],b->state[stage]);
        if(length<0||length>=(int)sizeof(manifest))return -1;
        snprintf(endpoint,sizeof(endpoint),"/api/sol/startup/%s/%s/commit/0",device,b->id);
        long long next;if(post(endpoint,manifest,(size_t)length,&next)||next!=1)return -1;
        strcpy(ids[b->slot],b->id);
    }
    snprintf(ack,sizeof(ack),"%s %s\n",ids[0],ids[1]);
    if(control(base,"ack",ack))return -1;
    fprintf(stderr,"[logctld] [INFO] %d startup buffer%s archived by server and cleared; next boot starts in buffer 1\n",count,count==1?"":"s");
    return 0;
}
