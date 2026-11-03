/* Independent retrying uploader: local collection never waits for the network. */
#define _GNU_SOURCE
#include "sol-forward.h"
#include "startup-forward.h"
#include <curl/curl.h>
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
static const char *stages[]={"r5-spl","a53-spl","uboot","initramfs","kernel","services","events"};
static void stop_forward(int sig){(void)sig;stopped=1;}
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
    char line[P],keyfile[P]="",auth_mode[8]="key";
    server[0]=device[0]=key[0]=0;strcpy(interface,"eth0");
    while(fgets(line,sizeof(line),f)) {
        line[strcspn(line,"\r\n")]=0;char *eq=strchr(line,'=');if(!eq||line[0]=='#')continue;*eq++=0;
        char *dst=NULL;size_t cap=0;
        if(!strcmp(line,"auth")){dst=auth_mode;cap=sizeof(auth_mode);}
        else if(!strcmp(line,"server")){dst=server;cap=sizeof(server);}
        else if(!strcmp(line,"device")){dst=device;cap=sizeof(device);}
        else if(!strcmp(line,"key-file")){dst=keyfile;cap=sizeof(keyfile);}
        else if(!strcmp(line,"interface")){dst=interface;cap=sizeof(interface);}
        else {fclose(f);return -1;}
        if(strlen(eq)>=cap){fclose(f);return -1;}strcpy(dst,eq);
    }
    fclose(f);
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
struct response{char b[512];size_t n;};
static size_t receive(char *p,size_t a,size_t b,void *opaque)
{
    struct response *r=opaque;size_t n=a*b;if(n>=sizeof(r->b)-r->n)return 0;
    memcpy(r->b+r->n,p,n);r->n+=n;r->b[r->n]=0;return n;
}
static int progress(void *p,curl_off_t a,curl_off_t b,curl_off_t c,curl_off_t d){(void)p;(void)a;(void)b;(void)c;(void)d;return stopped?1:0;}
static int post_url(const char *url,const char *data,size_t length,long long *next)
{
    char auth[96];
    CURL *c=curl_easy_init();if(!c)return -1;
    snprintf(auth,sizeof(auth),"Authorization: Bearer %s",key);
    struct curl_slist *h=NULL;if(*key)h=curl_slist_append(h,auth);h=curl_slist_append(h,"Content-Type: application/octet-stream");h=curl_slist_append(h,"Expect:");
    struct response r={0};
    curl_easy_setopt(c,CURLOPT_URL,url);curl_easy_setopt(c,CURLOPT_HTTPHEADER,h);
    curl_easy_setopt(c,CURLOPT_POSTFIELDS,data);curl_easy_setopt(c,CURLOPT_POSTFIELDSIZE,(long)length);
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,receive);curl_easy_setopt(c,CURLOPT_WRITEDATA,&r);
    curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,2L);curl_easy_setopt(c,CURLOPT_TIMEOUT,5L);
    curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);curl_easy_setopt(c,CURLOPT_NOPROXY,"*");
    curl_easy_setopt(c,CURLOPT_XFERINFOFUNCTION,progress);curl_easy_setopt(c,CURLOPT_NOPROGRESS,0L);
    curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"http,https");
    CURLcode result=curl_easy_perform(c);long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);
    curl_slist_free_all(h);curl_easy_cleanup(c);
    char extra;
    if(result!=CURLE_OK||(status!=200&&status!=409)||sscanf(r.b,"{\"next\":%lld}%c",next,&extra)!=1||*next<0||*next>4*1024*1024)return -1;
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
static int upload_file(const char *path,const char *boot,long long order,const char *stage)
{
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return errno==ENOENT?0:-1;
    struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size>4*1024*1024){close(fd);return -1;}
    char buf[4097];ssize_t n=pread(fd,buf,127,0);if(n<=0){close(fd);return 0;}buf[n]=0;
    long long gen=0,off=0;if(sscanf(buf,"[seq=%lld",&gen)!=1||gen<1){close(fd);return -1;}
    /* Durable server cursor is authoritative even after either side restarts. */
    if(post(boot,order,stage,gen,0,"",0,&off)){close(fd);return -1;}
    if(off>st.st_size){close(fd);return -1;}
    for(int batch=0;batch<32&&!stopped;batch++) {
        if(off>=st.st_size)break;
        size_t wanted=(size_t)(st.st_size-off);if(wanted>4096)wanted=4096;
        n=pread(fd,buf,wanted,off);if(n<0){close(fd);return -1;}if(!n)break;
        long long next;
        if(post(boot,order,stage,gen,off,buf,(size_t)n,&next)){close(fd);return -1;}
        if(next<=off||next>st.st_size){close(fd);return -1;}off=next;
    }
    close(fd);return 0;
}
struct boot {char name[64];long long order;};
static int compare(const void *a,const void *b){const struct boot *x=a,*y=b;return x->order>y->order?-1:x->order<y->order?1:0;}
static int cycle(const char *root)
{
    DIR *d=opendir(root);if(!d)return -1;struct dirent *entry;struct boot boots[256];size_t count=0;
    while((entry=readdir(d))&&count<256){
        char p[P],order[64];if(!safe(entry->d_name))continue;
        snprintf(p,sizeof(p),"%s/%s",root,entry->d_name);struct stat st;if(lstat(p,&st)||!S_ISDIR(st.st_mode))continue;
        snprintf(p,sizeof(p),"%s/%s/order",root,entry->d_name);if(read_text(p,order,sizeof(order)))continue;
        long long n=strtoll(order,NULL,10);if(n<1)continue;
        strcpy(boots[count].name,entry->d_name);boots[count++].order=n;
    }closedir(d);qsort(boots,count,sizeof(*boots),compare);
    size_t selected=count<4?count:4;
    for(size_t visit=selected;visit>0&&!stopped;visit--) {
        size_t b=visit-1;
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
        long long next;
        if(post(boots[b].name,boots[b].order,"meta",0,0,meta,strlen(meta),&next))return -1;
        for(size_t i=0;i<sizeof(stages)/sizeof(*stages);i++)for(int rotated=1;rotated>=0;rotated--){
            if(snprintf(p,sizeof(p),"%s/%s/%s.log%s",root,boots[b].name,stages[i],rotated?".1":"")>=(int)sizeof(p))return -1;
            if(upload_file(p,boots[b].name,boots[b].order,stages[i]))return -1;
        }
    }
    return 0;
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
    curl_global_init(CURL_GLOBAL_DEFAULT);int state=-1;
    while(!stopped){
        int startup=startup_forward(device,post_path);
        int result=cycle(root);
        if(startup)result=-1;
        if(result!=state){fprintf(stderr,"[logctld] [%s] SOL %s\n",result?"WARN":"INFO",result?"unavailable; local collection continues, retrying":"connected; log forwarding active");state=result;}
        for(int i=0;i<20&&!stopped;i++)usleep(250000);
    }
    curl_global_cleanup();close(lock);return 0;
}
