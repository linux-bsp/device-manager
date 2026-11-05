/* Independent retrying uploader: local collection never waits for the network. */
#define _GNU_SOURCE
#include "sol-forward.h"
#include "bundle-forward.h"
#include "boot-record.h"
#include "startup-forward.h"
#include "time-sync.h"
#include "time-control.h"
#include "dm/transfer.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <time.h>
#include <unistd.h>

#define P 1024
static volatile sig_atomic_t stopped;
static char server[512], device[64], key[65], interface[32]="eth0";
static int clock_enabled;
static const char *stages[]={"r5-spl","a53-spl","uboot","initramfs","kernel","services","events"};
static void stop_forward(int sig){(void)sig;stopped=1;dm_time_sync_cancel();dm_clock_cancel();}
static int safe(const char *s){if(!*s||strlen(s)>63)return 0;for(;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='0'&&*s<='9')||*s=='-'||*s=='_'))return 0;return 1;}
static int read_text(const char *p,char *buf,size_t cap)
{
    int fd=open(p,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -1;
    ssize_t n=read(fd,buf,cap-1);close(fd);if(n<0)return -1;buf[n]=0;buf[strcspn(buf,"\r\n")]=0;return 0;
}
static int config(void)
{
    const char *path=getenv("LOGCTL_SOL_CONFIG");if(!path)path="/etc/device-manager/sol.conf";
    FILE *f=fopen(path,"r");if(!f)return -1;
    char line[P],keyfile[P]="",auth_mode[8]="key",clock_mode[2]="0";
    server[0]=device[0]=key[0]=0;strcpy(interface,"eth0");
    while(fgets(line,sizeof(line),f)) {
        line[strcspn(line,"\r\n")]=0;char *eq=strchr(line,'=');if(!eq||line[0]=='#')continue;*eq++=0;
        char *dst=NULL;size_t cap=0;
        if(!strcmp(line,"time-sync")){dst=clock_mode;cap=sizeof(clock_mode);}
        else if(!strcmp(line,"auth")){dst=auth_mode;cap=sizeof(auth_mode);}
        else if(!strcmp(line,"server")){dst=server;cap=sizeof(server);}
        else if(!strcmp(line,"device")){dst=device;cap=sizeof(device);}
        else if(!strcmp(line,"key-file")){dst=keyfile;cap=sizeof(keyfile);}
        else if(!strcmp(line,"interface")){dst=interface;cap=sizeof(interface);}
        else {fclose(f);return -1;}
        if(strlen(eq)>=cap){fclose(f);return -1;}strcpy(dst,eq);
    }
    fclose(f);
    if(strcmp(clock_mode,"0")&&strcmp(clock_mode,"1"))return -1;
    clock_enabled=!strcmp(clock_mode,"1");
    size_t n=strlen(server);if(n&&server[n-1]=='/')server[n-1]=0;
    if((strncmp(server,"http://",7)&&strncmp(server,"https://",8))||strpbrk(server,"\r\n @?#")||!safe(interface))return -1;
    if(!*device){char p[P];snprintf(p,sizeof(p),"/sys/class/net/%s/address",interface);if(read_text(p,device,sizeof(device)))return -1;for(char *q=device;*q;q++)if(*q==':')*q='-';}
    struct stat st;
    if(!safe(device)||!strcmp(device,"viewer"))return -1;
    if(!strcmp(auth_mode,"none"))return 0;
    if(strcmp(auth_mode,"key")||keyfile[0]!='/'||lstat(keyfile,&st)||!S_ISREG(st.st_mode)||(st.st_mode&0077)||st.st_uid!=geteuid())return -1;
    if(read_text(keyfile,key,sizeof(key))||strlen(key)!=64)return -1;
    for(int i=0;i<64;i++)if(!((key[i]>='0'&&key[i]<='9')||(key[i]>='a'&&key[i]<='f')))return -1;
    return 0;
}
static int transfer_cancelled(void *context) { (void)context; return stopped != 0; }
static int post_url(const char *url,const char *data,size_t length,long long *next)
{
    char auth[96],body[512],extra; const char *headers[2]; size_t count=0;
    if(*key){snprintf(auth,sizeof(auth),"Authorization: Bearer %s",key);headers[count++]=auth;}
    headers[count++]="Content-Type: application/octet-stream";
    struct dm_transfer_request request; struct dm_transfer_result response;
    dm_transfer_request_init(&request);
    request.url=url;request.method="POST";request.body=data;request.body_size=length;
    request.headers=headers;request.header_count=count;request.response=body;request.response_capacity=sizeof(body);
    request.connect_timeout_ms=2000;request.timeout_ms=5000;request.callbacks.cancelled=transfer_cancelled;
    int rc=dm_transfer_perform(&request,&response);
    if(rc)return rc;
    if(response.http_status==410)return -ESTALE;
    if(response.http_status==409&&strstr(url,"/api/sol/bundles/")&&strstr(body,"Archive identity conflict"))return -ESTALE;
    if((response.http_status!=200&&response.http_status!=409)||
       sscanf(body,"{\"next\":%lld}%c",next,&extra)!=1||*next<0||*next>32*1024*1024)return -EPROTO;
    return 0;
}
static int post_path(const char *path,const char *data,size_t length,long long *next)
{
    char url[P];int k=snprintf(url,sizeof(url),"%s%s",server,path);
    return k<0||k>=(int)sizeof(url)?-1:post_url(url,data,length,next);
}
static int post(const char *boot,long long order,const char *stage,long long gen,long long off,const char *data,size_t length,long long *next)
{
    char path[512];int k=snprintf(path,sizeof(path),"/api/sol/push/%s/%s/%lld/%s/%lld/%lld",device,boot,order,stage,gen,off);
    return k<0||k>=(int)sizeof(path)?-1:post_path(path,data,length,next);
}
struct log_push_context { const char *boot,*stage; long long order,generation; };
static int push_chunk(void *context,uint64_t offset,const void *data,size_t length,uint64_t *next)
{
    struct log_push_context *log=context;long long acknowledged;
    int rc=post(log->boot,log->order,log->stage,log->generation,(long long)offset,data,length,&acknowledged);
    if (!rc) *next = (uint64_t)acknowledged;
    return rc;
}
static int upload_file(const char *path,const char *boot,long long order,const char *stage)
{
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return errno==ENOENT?0:-1;
    struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<0||st.st_size>4*1024*1024){close(fd);return -1;}
    char first[128];ssize_t n=pread(fd,first,sizeof(first)-1,0);
    if(n<=0){close(fd);return n<0?-1:0;}first[n]=0;
    struct log_push_context context={.boot=boot,.stage=stage,.order=order};
    if(sscanf(first,"[seq=%lld",&context.generation)!=1||context.generation<1){close(fd);return -1;}
    struct dm_transfer_push push={.fd=fd,.size=(uint64_t)st.st_size,.chunk_size=4096,.max_chunks=32,
        .exchange=push_chunk,.context=&context,.callbacks={.cancelled=transfer_cancelled}};
    uint64_t next;int rc=dm_transfer_push_file(&push,&next);close(fd);return rc;
}
struct boot {char name[64];long long order;};
static int compare(const void *a,const void *b){const struct boot *x=a,*y=b;return x->order>y->order?-1:x->order<y->order?1:0;}
static int cycle(const char *root)
{
    static size_t history_cursor;
    DIR *d=opendir(root);if(!d)return -1;struct dirent *entry;struct boot boots[256];size_t count=0;
    while((entry=readdir(d))&&count<256){
        char p[P],order[64];if(!safe(entry->d_name))continue;
        snprintf(p,sizeof(p),"%s/%s",root,entry->d_name);struct stat st;if(lstat(p,&st)||!S_ISDIR(st.st_mode))continue;
        snprintf(p,sizeof(p),"%s/%s/order",root,entry->d_name);if(read_text(p,order,sizeof(order)))continue;
        long long n=strtoll(order,NULL,10);if(n<1)continue;
        strcpy(boots[count].name,entry->d_name);boots[count++].order=n;
    }closedir(d);qsort(boots,count,sizeof(*boots),compare);
    /* Live output first, then at most two historical boots per cycle. Rotate
     * the history cursor even on error so one corrupt boot cannot starve all
     * later evidence. Transport yields are not archive acknowledgements. */
    size_t selected[3],visits=0;
    if(count)selected[visits++]=0;
    for(size_t i=0;i<2&&i+1<count;i++) {
        selected[visits++]=1+history_cursor%(count-1);
        history_cursor=(history_cursor+1)%(count-1);
    }
    int pending=0,failed=0;
    for(size_t visit=0;visit<visits&&!stopped;visit++) {
        size_t b=selected[visit];
        char p[P],meta[1024],hostname[64]="unknown",mac[64]="unknown";struct statfs fs;
        gethostname(hostname,sizeof(hostname)-1);hostname[sizeof(hostname)-1]=0;
        snprintf(p,sizeof(p),"/sys/class/net/%s/address",interface);read_text(p,mac,sizeof(mac));
        /* Actual source IP is recorded by the server. No shell or IP command. */
        const char *mode=statfs(root,&fs)?"unavailable":fs.f_type==0x01021994||fs.f_type==0x858458f6?"volatile":"persistent";
        int k=snprintf(meta,sizeof(meta),"storage=%s\npath=%s\nhostname=%s\nmac=%s\ncurrent=%s\n",mode,root,hostname,mac,b==0?"yes":"no");
        if(k<0||k>=(int)sizeof(meta))return -1;
        for(size_t i=0;i<3;i++){
            snprintf(p,sizeof(p),"%.800s/%.63s/%s.log",root,boots[b].name,stages[i]);struct stat st;
            k+=snprintf(meta+k,sizeof(meta)-(size_t)k,"%s=%s\n",stages[i],!lstat(p,&st)&&S_ISREG(st.st_mode)&&st.st_size>0?"captured":"unavailable");
        }
        snprintf(p,sizeof(p),"%.800s/%.63s/storage-check",root,boots[b].name);
        char marker[64];
        k+=snprintf(meta+k,sizeof(meta)-(size_t)k,"durable-marker=%s\n",!read_text(p,marker,sizeof(marker))&&!strcmp(marker,boots[b].name)?"synced":"unavailable");
        char boot_meta[512];
        if(!log_boot_metadata(root,boots[b].name,boot_meta,sizeof(boot_meta))){
            if((size_t)k+strlen(boot_meta)>=sizeof(meta))return -1;
            strcpy(meta+k,boot_meta);
        }
        long long next;
        if(post(boots[b].name,boots[b].order,"meta",0,0,meta,strlen(meta),&next))return -1;
        for(size_t i=0;i<sizeof(stages)/sizeof(*stages);i++)for(int rotated=1;rotated>=0;rotated--){
            if(snprintf(p,sizeof(p),"%s/%s/%s.log%s",root,boots[b].name,stages[i],rotated?".1":"")>=(int)sizeof(p))return -1;
            int rc=upload_file(p,boots[b].name,boots[b].order,stages[i]);
            if(rc<0)failed=1;
            else if(rc>0)pending=1;
        }
    }
    return failed?-1:pending;
}
int sol_forward(const char *root)
{
    if(strlen(root)>800)return 2;
    signal(SIGTERM,stop_forward);signal(SIGINT,stop_forward);signal(SIGPIPE,SIG_IGN);
    int warned=0;
    while(!stopped&&config()){
        if(!warned++)fprintf(stderr,"[logctld] [WARN] SOL waiting for valid configuration and device credential; local collection continues\n");
        for(int i=0;i<20&&!stopped;i++)usleep(250000);
    }
    if(stopped)return 0;
    char path[P];if(snprintf(path,sizeof(path),"%s/.sol-lock",root)>=(int)sizeof(path))return 2;
    int lock=open(path,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);if(lock<0||flock(lock,LOCK_EX|LOCK_NB))return 1;
    signal(SIGTERM,stop_forward);signal(SIGINT,stop_forward);signal(SIGPIPE,SIG_IGN);
    int state=-1;
    while(!stopped){
        if(clock_enabled)dm_clock_cycle(server,interface,device,key);
        if(stopped)break;
        int startup=startup_forward(device,post_path);
        int result=cycle(root)<0?-1:0;
        if(startup)result=-1;
        if(!stopped && bundle_forward(root,server,device,post_path,transfer_cancelled)<0)result=-1;
        if(result!=state){fprintf(stderr,"[logctld] [%s] SOL %s\n",result?"WARN":"INFO",result?"unavailable; local collection continues, retrying":"connected; log forwarding active");state=result;}
        for(int i=0;i<20&&!stopped;i++)usleep(250000);
    }
    close(lock);return 0;
}

int sol_sync_time(void)
{
    if(config()){fprintf(stderr,"[logctl] [ERROR] valid SOL server configuration required for time sync\n");return 1;}
    char source[512]="-";int enabled=1;unsigned interval=3600;
    (void)dm_clock_local(source,&enabled,&interval);dm_time_interval(interval);
    int result=dm_time_sync(strcmp(source,"-")?source:server,interface,1);
    return result?1:0;
}
