/* Local collection with an isolated optional network forwarding worker. */
#define _GNU_SOURCE
#include "sol-forward.h"
#include "retention.h"
#include "archive.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHUNK 3072
#define PATH_CAP 1024
#define STREAMS 7
#define LIMIT (2 * 1024 * 1024)
static const char *stages[STREAMS] = {"r5-spl", "a53-spl", "uboot", "initramfs", "kernel", "services", "events"};
static char runtime[PATH_CAP], root[PATH_CAP], current[64], bootpath[PATH_CAP];
static size_t file_limit = LIMIT;
static uint64_t total_limit = 64ULL * 1024 * 1024;
static volatile sig_atomic_t stopping;
static unsigned long long sequence, synced_sequence;
static int write_failure, last_sync_error, collector_stopped;
static struct timespec last_storage_warning;
static int pmsg_fd = -1;
struct packet { char stage[16]; char data[CHUNK]; };
static void stop(int sig) { (void)sig; stopping = 1; }
static void fail(const char *s) { perror(s); exit(1); }
static void join(char *out, const char *a, const char *b)
{
    if (snprintf(out, PATH_CAP, "%s/%s", a, b) >= PATH_CAP) { errno=ENAMETOOLONG; fail("logctl path"); }
}
static int safe_name(const char *s)
{
    if (!*s || strlen(s)>63 || s[0]=='.') return 0;
    for (;*s;s++) if (!((*s>='a' && *s<='z') || (*s>='0' && *s<='9') || *s=='-' || *s=='_')) return 0;
    return 1;
}
static int stage_id(const char *s)
{
    for (int i=0;i<STREAMS;i++) if (!strcmp(s,stages[i])) return i;
    return -1;
}
static int directory(const char *p)
{
    struct stat st;
    if (mkdir(p,0700) && errno!=EEXIST) return -1;
    if (lstat(p,&st) || !S_ISDIR(st.st_mode) || st.st_uid!=geteuid()) { errno=EPERM; return -1; }
    return 0;
}
static int write_all(int fd, const void *data, size_t n)
{
    const char *p=data;
    while (n) { ssize_t k=write(fd,p,n); if(k<0) { if(errno==EINTR)continue;return -1; } if(!k){errno=EIO;return -1;} p+=k;n-=(size_t)k; }
    return 0;
}
static int small_read(const char *path, char *buf, size_t cap)
{
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW); if(fd<0)return -1;
    ssize_t n=read(fd,buf,cap-1); close(fd); if(n<0)return -1;
    buf[n]=0; buf[strcspn(buf,"\r\n")]=0; return 0;
}
static int save_text(const char *path, const char *s)
{
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return -1;
    int ret=write_all(fd,s,strlen(s)); if(!ret)ret=fsync(fd);
    int error=errno; if(close(fd)&&!ret){ret=-1;error=errno;} errno=error; return ret;
}
/* Runtime diagnostics describe completed syncs in this daemon instance. They
 * are not a persistent record index or an acknowledgement from the server. */
static void sync_status(const char *state, int error)
{
    char path[PATH_CAP], temp[PATH_CAP], value[256];
    join(path,runtime,"sync-status"); join(temp,runtime,".sync-status.tmp");
    snprintf(value,sizeof(value),"collector-state=%s\nsync-state=%s\nsynced-sequence=%llu\nobserved-sequence=%llu\nsync-errno=%d\n",collector_stopped?"stopped":"running",state,synced_sequence,sequence,error);
    if(save_text(temp,value)||rename(temp,path)) {
        int saved=errno;unlink(temp);errno=saved;
        perror("[logctld] [ERROR] cannot record sync status");
    }
}
static void storage_error(int error)
{
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    if(!error)error=EIO;
    if(error!=last_sync_error||now.tv_sec-last_storage_warning.tv_sec>=30){
        fprintf(stderr,"[logctld] [ERROR] local persistence unconfirmed: %s; last synced sequence=%llu\n",strerror(error),synced_sequence);
        last_storage_warning=now;
    }
    last_sync_error=error;
    sync_status("error",error);
}
static void setup(void)
{
    const char *r=getenv("LOGCTL_RUN_DIR");
    snprintf(runtime,sizeof(runtime),"%s",r?r:"/run/logctl");
    if(directory(runtime))fail("logctl runtime");
    char p[PATH_CAP]; join(p,runtime,"root");
    if(small_read(p,root,sizeof(root)))join(root,runtime,"boots");
    if(small_read("/proc/sys/kernel/random/boot_id",current,sizeof(current)))fail("boot_id");
    const char *test=getenv("LOGCTL_BOOT_ID");
    if(test)snprintf(current,sizeof(current),"%s",test);
    if(!safe_name(current))fail("invalid boot ID");
    join(bootpath,root,current);
}
/* Datagram send never blocks startup. Before the daemon is ready, keep bounded
 * per-stage spool files on /run; they are imported after the socket is bound. */
static void emit(const char *stage, const void *text, size_t len)
{
    if(stage_id(stage)<0)return;
    int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    struct sockaddr_un addr={.sun_family=AF_UNIX};
    char p[PATH_CAP];join(p,runtime,"socket");
    if(strlen(p)>=sizeof(addr.sun_path)) { if(fd>=0)close(fd);return; }
    memcpy(addr.sun_path,p,strlen(p)+1);
    struct packet packet={0};snprintf(packet.stage,sizeof(packet.stage),"%s",stage);
    if(len>CHUNK)len=CHUNK;
    memcpy(packet.data,text,len);
    int ok=fd>=0 && sendto(fd,&packet,sizeof(packet.stage)+len,MSG_NOSIGNAL,(void*)&addr,sizeof(addr))==(ssize_t)(sizeof(packet.stage)+len);
    int error=errno;if(fd>=0)close(fd);if(ok)return;
    /* A full queue is a real loss, not a reason to grow unbounded memory. */
    if(error==EAGAIN || error==ENOBUFS) {
        join(p,runtime,"dropped");fd=open(p,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW,0600);
        if(fd>=0) { struct stat st;if(!fstat(fd,&st)&&st.st_size<65536)(void)write_all(fd,"1\n",2);close(fd); }return;
    }
    char name[64];snprintf(name,sizeof(name),"early-%s",stage);join(p,runtime,name);
    fd=open(p,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd>=0) { struct stat st; if(!flock(fd,LOCK_EX)&&!fstat(fd,&st)&&st.st_size<256*1024)(void)write_all(fd,text,len);close(fd); }
}
static int sync_directory(const char *path);
static void event(const char *s);
static int append_source_impl(const char *stage, const char *data, size_t n, const char *source)
{
    char p[PATH_CAP],old[PATH_CAP],name[64];struct stat st;
    snprintf(name,sizeof(name),"%s.log",stage);join(p,bootpath,name);
    if(!lstat(p,&st) && (!S_ISREG(st.st_mode)||st.st_nlink!=1)){errno=EINVAL;return -1;}
    if(!lstat(p,&st) && (size_t)st.st_size+n+512>file_limit) {
        snprintf(name,sizeof(name),"%s.log.1",stage);join(old,bootpath,name);
        struct stat previous;
        if (!lstat(old, &previous)) {
            if (!S_ISREG(previous.st_mode) || previous.st_nlink != 1) { errno=EINVAL; return -1; }
            char header[128]; unsigned long long generation=0;
            if (!small_read(old,header,sizeof(header))) (void)sscanf(header,"[seq=%llu",&generation);
            if (log_retention_record(root,current,"stream-limit",stage,generation,(uint64_t)previous.st_size)) return -1;
            if (strcmp(stage,"events")) {
                char notice[256];snprintf(notice,sizeof(notice),"log retention: reason=stream-limit stage=%s generation=%llu bytes=%lld; archive confirmation unavailable\n",stage,generation,(long long)previous.st_size);
                event(notice);
            }
        } else if (errno != ENOENT) return -1;
        if(rename(p,old))return -1;
    }
    int fd=open(p,O_WRONLY|O_APPEND|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return -1;
    struct timespec mono,wall;clock_gettime(CLOCK_MONOTONIC,&mono);clock_gettime(CLOCK_REALTIME,&wall);
    char head[512];int k=snprintf(head,sizeof(head),"[seq=%llu mono=%lld.%09ld wall=%lld bytes=%zu source=%s] ",++sequence,(long long)mono.tv_sec,mono.tv_nsec,(long long)wall.tv_sec,n,source);
    int ret=write_all(fd,head,(size_t)k) || write_all(fd,data,n);
    /* Transport frames never add a newline to the original console bytes. */
    /* Critical events are durable immediately. Routine data is synced in batches. */
    if(!strcmp(stage,"events")){
        if(fdatasync(fd)||sync_directory(bootpath)||sync_directory(root))ret=-1;
    }
    int saved_error=errno;
    if(close(fd)&&!ret){ret=-1;saved_error=errno;}
    if(pmsg_fd>=0 && (!strcmp(stage,"events")||!strcmp(stage,"services"))) {
        if(write_all(pmsg_fd,head,(size_t)k)||write_all(pmsg_fd,data,n)) { close(pmsg_fd);pmsg_fd=-1; }
    }
    errno=saved_error;return ret;
}
static int append_source(const char *stage,const char *data,size_t n,const char *source)
{
    int rc=append_source_impl(stage,data,n,source);
    if(rc){if(!write_failure)write_failure=errno?errno:EIO;storage_error(write_failure);}
    return rc;
}
static int append_record(const char *stage, const char *data, size_t n)
{ return append_source(stage,data,n,"console"); }
static void event(const char *s) { (void)append_record("events",s,strlen(s)); }
static int sync_directory(const char *path)
{
    int fd=open(path,O_DIRECTORY|O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -1;
    int rc=fsync(fd),error=errno;
    if(close(fd)&&!rc){rc=-1;error=errno;}
    errno=error;return rc;
}
static int sync_logs(void)
{
    int error=write_failure;
    DIR *dir=opendir(bootpath);
    if(!dir){if(!error)error=errno;}
    else {
        struct dirent *entry;
        for(;;){
            errno=0;entry=readdir(dir);
            if(!entry){if(errno&&!error)error=errno;break;}
            if(entry->d_name[0]=='.')continue;
            char path[PATH_CAP];join(path,bootpath,entry->d_name);
            int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
            if(fd<0){if(!error)error=errno;continue;}
            struct stat st;int rc=fstat(fd,&st);
            if(!rc&&!S_ISREG(st.st_mode)){errno=EINVAL;rc=-1;}
            if(!rc)rc=fsync(fd);
            if(rc&&!error)error=errno;
            if(close(fd)&&!error)error=errno;
        }
        if(closedir(dir)&&!error)error=errno;
    }
    if(sync_directory(bootpath)&&!error)error=errno;
    if(sync_directory(root)&&!error)error=errno;
    if(error){storage_error(error);errno=error;return -1;}
    synced_sequence=sequence;
    if(last_sync_error)fprintf(stderr,"[logctld] [INFO] local log synchronization recovered; synced sequence=%llu\n",synced_sequence);
    last_sync_error=0;sync_status("ok",0);return 0;
}
struct boot { char name[64]; unsigned long long order; };
static int boot_compare(const void *a,const void *b)
{
    const struct boot *x=a,*y=b;return x->order<y->order?1:x->order>y->order?-1:strcmp(x->name,y->name);
}
static size_t boots(struct boot *list,size_t cap)
{
    DIR *d=opendir(root);if(!d)return 0;struct dirent *de;size_t n=0;
    while((de=readdir(d)) && n<cap) {
        if(!safe_name(de->d_name))continue;
        char p[PATH_CAP],q[PATH_CAP],s[64];struct stat st;join(p,root,de->d_name);
        if(lstat(p,&st)||!S_ISDIR(st.st_mode))continue;
        join(q,p,"order");if(small_read(q,s,sizeof(s)))continue;
        memcpy(list[n].name,de->d_name,strlen(de->d_name)+1);list[n++].order=strtoull(s,NULL,10);
    }
    closedir(d);qsort(list,n,sizeof(*list),boot_compare);return n;
}
static int verify_storage(void)
{
    struct statfs fs;char p[PATH_CAP],value[64];
    if(statfs(root,&fs)||fs.f_type==0x01021994||fs.f_type==0x858458f6){
        puts("[logctl] [WARN] storage=volatile; reboot persistence cannot be verified");return 2;
    }
    struct boot list[256];size_t n=boots(list,256);int previous=0;
    for(size_t i=0;i<n;i++)if(strcmp(list[i].name,current)){
        char dir[PATH_CAP];join(dir,root,list[i].name);join(p,dir,"storage-check");
        if(!small_read(p,value,sizeof(value))&&!strcmp(value,list[i].name)){
            printf("[logctl] [INFO] previous boot marker readable: %s\n",list[i].name);previous=1;break;
        }
    }
    join(p,bootpath,"storage-check");if(save_text(p,current)){perror("[logctl] [ERROR] storage marker");return 1;}
    int fd=open(bootpath,O_DIRECTORY|O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return 1;
    int rc=fsync(fd);close(fd);if(rc)return 1;
    printf("[logctl] [INFO] storage=persistent cross-boot=%s; current marker synced\n",previous?"verified":"pending-next-boot");return 0;
}
static int verify_bootlog(void)
{
    const char *base=getenv("LOGCTL_HANDOFF_DIR");if(!base)base="/sys/firmware/bootlog";
    char state_path[PATH_CAP],state[128];join(state_path,base,"state");
    if(!small_read(state_path,state,sizeof(state))&&!strcmp(state,"startup-state=acknowledged")){
        puts("[logctl] [INFO] startup logs archived; both RAM buffers acknowledged");return 0;
    }
    int missing=0;for(int i=0;i<3;i++){
        char p[PATH_CAP];join(p,base,stages[i]);int fd=open(p,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);char c;
        int ok=fd>=0&&read(fd,&c,1)==1;if(fd>=0)close(fd);
        printf("[logctl] [%s] stage=%s handoff=%s\n",ok?"INFO":"WARN",stages[i],ok?"verified-by-kernel":"unavailable");if(!ok)missing=1;
    }return missing;
}
static void prune(void)
{
    struct boot list[256];size_t n=boots(list,256),remaining=n;
    uint64_t sizes[256]={0},total=0;
    for(size_t i=0;i<n;i++) {
        char dir[PATH_CAP],path[PATH_CAP];join(dir,root,list[i].name);
        DIR *d=opendir(dir);if(!d)continue;
        struct dirent *entry;
        while((entry=readdir(d))) {
            if(entry->d_name[0]=='.')continue;
            join(path,dir,entry->d_name);struct stat st;
            if(!lstat(path,&st)&&S_ISREG(st.st_mode))sizes[i]+=(uint64_t)st.st_size;
        }
        closedir(d);total+=sizes[i];
    }
    for(size_t index=n;index>0;index--) {
        size_t i=index-1;
        if(!strcmp(list[i].name,current))continue;
        if(remaining<=LOG_KEEP_BOOTS&&total<=total_limit)break;
        char dir[PATH_CAP],p[PATH_CAP];join(dir,root,list[i].name);DIR *d=opendir(dir);if(!d)continue;
        const char *reason=remaining>LOG_KEEP_BOOTS?"boot-limit":"capacity-limit";
        if(log_retention_record(root,list[i].name,reason,"all",0,sizes[i])) {
            int error=errno;closedir(d);storage_error(error);continue;
        }
        char notice[256];snprintf(notice,sizeof(notice),"log retention: reason=%s boot=%.63s bytes=%llu; archive confirmation unavailable\n",reason,list[i].name,(unsigned long long)sizes[i]);event(notice);
        struct dirent *de;while((de=readdir(d))) {
            if(de->d_name[0]=='.')continue;
            join(p,dir,de->d_name);struct stat st;
            if(!lstat(p,&st)&&S_ISREG(st.st_mode)&&unlink(p))storage_error(errno);
        }
        closedir(d);
        if(rmdir(dir))storage_error(errno);
        else {remaining--;total-=sizes[i];if(sync_directory(root))storage_error(errno);}
    }
}
static int import_file(const char *path,const char *stage,size_t max)
{
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -1;
    char b[CHUNK];size_t total=0;ssize_t n;int ret=0;
    while(total<max && (n=read(fd,b,sizeof(b)))>0){if(append_record(stage,b,(size_t)n)){ret=-1;break;}total+=(size_t)n;}
    close(fd);return ret;
}
static void import_early(void)
{
    char p[PATH_CAP],name[64];
    for(int i=0;i<STREAMS;i++) {
        snprintf(name,sizeof(name),"early-%s",stages[i]);join(p,runtime,name);
        int fd=open(p,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)continue;
        if(!flock(fd,LOCK_EX)&&!import_file(p,stages[i],256*1024))unlink(p);
        close(fd);
    }
    for(int i=0;i<3;i++) {
        snprintf(p,sizeof(p),"/sys/firmware/bootlog/%s",stages[i]);
        if(import_file(p,stages[i],65536))event("bootloader log unavailable for this stage (requires updated bootloader and kernel handoff)");
    }
    (void)import_file("/run/logctl-initramfs.log","initramfs",256*1024);
}
static void pstore_archive(void)
{
    struct statfs fs;int persistent=!statfs(root,&fs)&&fs.f_type!=0x01021994&&fs.f_type!=0x858458f6;
    DIR *d=opendir("/sys/fs/pstore");if(!d)return;
    struct dirent *de;unsigned count=0;size_t budget=0;
    while((de=readdir(d)) && count<16) {
        if(de->d_name[0]=='.'||strlen(de->d_name)>100)continue;
        char src[PATH_CAP],dst[PATH_CAP],name[128];join(src,"/sys/fs/pstore",de->d_name);
        snprintf(name,sizeof(name),"crash-%s",de->d_name);join(dst,bootpath,name);
        int in=open(src,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(in<0)continue;
        int out=open(dst,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        if(out<0){close(in);continue;}
        char b[CHUNK];ssize_t n;size_t total=0;int ok=1;
        while((n=read(in,b,sizeof(b)))>0){total+=(size_t)n;budget+=(size_t)n;if(budget>1024*1024||write_all(out,b,(size_t)n)){ok=0;break;}}
        if(n<0||fsync(out))ok=0;
        close(out);close(in);
        int dirfd=open(bootpath,O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(dirfd<0||fsync(dirfd))ok=0;if(dirfd>=0)close(dirfd);
        if(ok&&persistent)unlink(src);else if(!ok)unlink(dst);
        count++;
    }
    closedir(d);
}
struct source { char name[256]; ino_t inode; off_t offset; };
static struct source sources[32];static unsigned source_count;
static void scan_services(void)
{
    const char *base=getenv("LOGCTL_SERVICE_DIR");if(!base)base="/var/log";
    DIR *d=opendir(base);if(!d)return;struct dirent *de;
    while((de=readdir(d))) {
        size_t len=strlen(de->d_name);if(len<5||strcmp(de->d_name+len-4,".log"))continue;
        char p[PATH_CAP];join(p,base,de->d_name);struct stat st;
        if(lstat(p,&st)||!S_ISREG(st.st_mode))continue;
        unsigned i;for(i=0;i<source_count;i++)if(!strcmp(sources[i].name,de->d_name))break;
        if(i==source_count){if(i==32)continue;snprintf(sources[i].name,sizeof(sources[i].name),"%s",de->d_name);source_count++;}
        struct source *s=&sources[i];if(st.st_ino!=s->inode||st.st_size<s->offset){s->inode=st.st_ino;s->offset=0;}
        int fd=open(p,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)continue;
        if(st.st_size-s->offset>1024*1024){s->offset=st.st_size-1024*1024;event("service backlog exceeded 1 MiB; oldest bytes omitted");}
        if(lseek(fd,s->offset,SEEK_SET)<0){close(fd);continue;}
        char b[CHUNK];
        for(int batch=0;batch<32;batch++) {
            ssize_t n=read(fd,b,sizeof(b));if(n<=0)break;
            if(append_source("services",b,(size_t)n,"file"))break;
            s->offset+=n;
        }
        close(fd);
    }
    closedir(d);
}
static void recover_sequence(void)
{
    for(int i=0;i<STREAMS;i++) {
        char p[PATH_CAP],name[64],buf[16385];snprintf(name,sizeof(name),"%s.log",stages[i]);join(p,bootpath,name);
        int fd=open(p,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)continue;
        off_t end=lseek(fd,0,SEEK_END);if(end<0){close(fd);continue;}
        (void)lseek(fd,end>16384?end-16384:0,SEEK_SET);ssize_t n=read(fd,buf,sizeof(buf)-1);close(fd);
        if(n<0)continue;
        buf[n]=0;char *next=buf;
        while((next=strstr(next,"[seq="))){unsigned long long value=strtoull(next+5,NULL,10);if(value>sequence)sequence=value;next+=5;}
    }
}
static int daemon_main(int argc,char **argv)
{
    if(argc!=3||strcmp(argv[1],"--root")||argv[2][0]!='/'){fprintf(stderr,"Usage: logctld --root /data/log/boots\n");return 2;}
    snprintf(root,sizeof(root),"%s",argv[2]);join(bootpath,root,current);
    if(directory(root))fail("logctld root");
    char p[PATH_CAP];join(p,runtime,"lock");int lock=open(p,O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
    if(lock<0||flock(lock,LOCK_EX|LOCK_NB))fail("logctld already running");
    if(directory(bootpath))fail("logctld boot directory");
    struct boot list[256];size_t count=boots(list,256);char order[64];
    join(p,bootpath,"order");if(access(p,F_OK)){snprintf(order,sizeof(order),"%llu\n",count?list[0].order+1:1);if(save_text(p,order))fail("boot order");}
    join(p,runtime,"root");if(save_text(p,root))fail("logctld state");
    const char *limit=getenv("LOGCTL_FILE_LIMIT");if(limit){size_t n=strtoul(limit,NULL,10);if(n>=4096&&n<=LIMIT)file_limit=n;}
    const char *quota=getenv("LOGCTL_TOTAL_LIMIT");if(quota){uint64_t n=strtoull(quota,NULL,10);if(n>=4096&&n<=total_limit)total_limit=n;}
    int sock=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(sock<0)fail("socket");
    struct sockaddr_un a={.sun_family=AF_UNIX};join(p,runtime,"socket");if(strlen(p)>=sizeof(a.sun_path))fail("socket path");
    memcpy(a.sun_path,p,strlen(p)+1);unlink(p);if(bind(sock,(void*)&a,sizeof(a)))fail("bind");chmod(p,0600);
    signal(SIGTERM,stop);signal(SIGINT,stop);signal(SIGPIPE,SIG_IGN);
    join(p,bootpath,"cmdline");char cmdline[4096];if(!small_read("/proc/cmdline",cmdline,sizeof(cmdline)))save_text(p,cmdline);
    join(p,bootpath,"identity");
    pid_t child=fork();if(!child){int fd=open(p,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);if(fd>=0){dup2(fd,1);dup2(fd,2);close(fd);}execl("/usr/sbin/fwctl","fwctl","version",NULL);_exit(127);}
    /* No shell and no dependency on the identity command's success. */
    int kmsg=open("/dev/kmsg",O_RDONLY|O_NONBLOCK|O_CLOEXEC);
    pmsg_fd=open("/dev/pmsg0",O_WRONLY|O_CLOEXEC);
    recover_sequence();
    event("log collector started; monotonic/wall timestamps are receive times; kernel records retain native timestamps");
    if(kmsg<0)event("kernel source unavailable: /dev/kmsg");
    join(p,bootpath,"imported");if(access(p,F_OK)){import_early();save_text(p,"1\n");}
    pstore_archive();prune();
    (void)verify_storage();
    pid_t forwarder=-1;
    const char *sol_config=getenv("LOGCTL_SOL_CONFIG");
    if(!sol_config)sol_config="/etc/device-manager/sol.conf";
    if(!access(sol_config,R_OK)) {
        forwarder=fork();
        if(!forwarder){execl("/proc/self/exe","logctl","forward","--root",root,NULL);_exit(127);}
        if(forwarder<0)event("SOL worker could not start; local collection continues");
    }
    (void)sync_logs();
    struct timespec last_sync;clock_gettime(CLOCK_MONOTONIC,&last_sync);
    while(!stopping) {
        struct pollfd fds[2]={{sock,POLLIN,0},{kmsg,POLLIN,0}};
        (void)poll(fds,2,250);
        if(fds[0].revents&POLLIN)for(int j=0;j<64;j++){
            struct packet packet;ssize_t n=recv(sock,&packet,sizeof(packet),0);if(n<0)break;
            if(n<=16||!memchr(packet.stage,0,16)||stage_id(packet.stage)<0)continue;
            (void)append_record(packet.stage,packet.data,(size_t)n-16);
        }
        if(fds[1].revents&POLLIN)for(int j=0;j<128;j++){
            char buf[8192];ssize_t n=read(kmsg,buf,sizeof(buf));if(n<0){if(errno==EPIPE)event("kernel ring overrun: messages lost");break;}
            if(!n)break;
            if(append_record("kernel",buf,(size_t)n))break;
        }
        scan_services();if(child>0&&waitpid(child,NULL,WNOHANG)==child)child=0;
        struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
        if(now.tv_sec-last_sync.tv_sec>5||(now.tv_sec-last_sync.tv_sec==5&&now.tv_nsec>=last_sync.tv_nsec)){
            (void)sync_logs();prune();last_sync=now;
        }
    }
    if(forwarder>0){
        kill(forwarder,SIGTERM);
        for(int i=0;i<20;i++){if(waitpid(forwarder,NULL,WNOHANG)==forwarder){forwarder=-1;break;}usleep(100000);}
        if(forwarder>0){kill(forwarder,SIGKILL);waitpid(forwarder,NULL,0);}
    }
    scan_services();event("log collector stopped");collector_stopped=1;sync_logs();close(sock);if(kmsg>=0)close(kmsg);
    join(p,runtime,"socket");unlink(p);close(lock);return 0;
}
/* exec preserves the command PID and exit status. The relay survives only as
 * long as writers exist, so background services cannot hold rcS completion. */
static int run_command(const char *stage,char **args)
{
    if(stage_id(stage)<0||!*args)return 2;
    int fds[2];if(pipe2(fds,O_CLOEXEC))return 1;
    pid_t relay=fork();if(relay<0)return 1;
    if(!relay){
        close(fds[1]);signal(SIGPIPE,SIG_IGN);char buf[CHUNK];ssize_t n;
        while((n=read(fds[0],buf,sizeof(buf)))!=0){if(n<0){if(errno==EINTR)continue;break;}emit(stage,buf,(size_t)n);(void)write_all(STDOUT_FILENO,buf,(size_t)n);}
        close(fds[0]);_exit(0);
    }
    close(fds[0]);dup2(fds[1],STDOUT_FILENO);dup2(fds[1],STDERR_FILENO);close(fds[1]);execvp(args[0],args);perror("logctl exec");return 127;
}
static int show_file(const char *path)
{
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return 0;
    char buf[8192];ssize_t n;int ret=0;while((n=read(fd,buf,sizeof(buf)))>0)if(write_all(1,buf,(size_t)n)){ret=1;break;}
    if(n<0)ret=1;
    close(fd);return ret;
}
static int cli_main(int argc,char **argv)
{
    if(argc==2&&!strcmp(argv[1],"sync-time"))return sol_sync_time();
    if(argc==4&&!strcmp(argv[1],"forward")&&!strcmp(argv[2],"--root")&&argv[3][0]=='/')return sol_forward(argv[3]);
    if(argc>=4&&!strcmp(argv[1],"run"))return run_command(argv[2],argv+3);
    if(argc==4&&!strcmp(argv[1],"write")){if(stage_id(argv[2])<0)return 2;emit(argv[2],argv[3],strlen(argv[3]));return 0;}
    struct boot list[256];size_t n=boots(list,256);
    if(argc==2&&!strcmp(argv[1],"status")){
        struct statfs fs;char state[64];const char *mode=statfs(root,&fs)?"unavailable":fs.f_type==0x01021994||fs.f_type==0x858458f6?"volatile":"persistent";
        if(small_read("/run/device-manager/storage-state",state,sizeof(state)))snprintf(state,sizeof(state),"unknown");
        printf("boot=%s\nstorage=%s\ndata-state=%s\npath=%s\nretention=%d boots; 2 x 2 MiB per stream; 64 MiB total\narchive-ack=not-tracked\n",current,mode,state,bootpath,LOG_KEEP_BOOTS);
        char status[PATH_CAP];join(status,runtime,"sync-status");
        if(access(status,F_OK))puts("sync-state=unknown");else if(show_file(status))return 1;
        join(status,runtime,"clock-status");if(!access(status,R_OK)&&show_file(status))return 1;
        const char *handoff=getenv("LOGCTL_HANDOFF_DIR");if(!handoff)handoff="/sys/firmware/bootlog";
        join(status,handoff,"state");if(!access(status,R_OK)&&(show_file(status)))return 1;
        return 0;
    }
    if(argc==2&&!strcmp(argv[1],"verify-storage"))return verify_storage();
    if(argc==2&&!strcmp(argv[1],"losses")){
        char p[PATH_CAP];join(p,root,".retention.log.1");if(show_file(p))return 1;
        join(p,root,".retention.log");return show_file(p);
    }
    if(argc==2&&!strcmp(argv[1],"verify-boot"))return verify_bootlog();
    if(argc==2&&!strcmp(argv[1],"boots")){for(size_t i=0;i<n;i++)printf("%llu %s%s\n",list[i].order,list[i].name,!strcmp(list[i].name,current)?" current":"");return 0;}
    if(argc<2)goto usage;
    const char *boot=current,*stage=NULL,*output=NULL;
    for(int i=2;i<argc;i++){
        if(!strcmp(argv[i],"--boot")&&i+1<argc)boot=argv[++i];
        else if(!strcmp(argv[i],"--stage")&&i+1<argc)stage=argv[++i];
        else if(!strcmp(argv[i],"--output")&&i+1<argc)output=argv[++i];else goto usage;
    }
    if(!strcmp(boot,"previous")){boot=NULL;for(size_t i=0;i<n;i++)if(strcmp(list[i].name,current)){boot=list[i].name;break;}if(!boot){fprintf(stderr,"No previous boot available\n");return 1;}}
    if(!safe_name(boot)||(stage&&stage_id(stage)<0))goto usage;
    char path[PATH_CAP];join(path,root,boot);struct stat st;if(lstat(path,&st)||!S_ISDIR(st.st_mode))return 1;
    if(!strcmp(argv[1],"show")){
        for(int i=0;i<STREAMS;i++)if(!stage||!strcmp(stage,stages[i]))for(int j=1;j>=0;j--){
            char p[PATH_CAP],name[64];snprintf(name,sizeof(name),"%s.log%s",stages[i],j?".1":"");join(p,path,name);if(show_file(p))return 1;
        }return 0;
    }
    if(!strcmp(argv[1],"follow")&&!output){
        struct { ino_t inode; off_t offset; } cursors[STREAMS] = {{0}};
        signal(SIGTERM,stop);signal(SIGINT,stop);
        while(!stopping){
            for(int i=0;i<STREAMS;i++)if(!stage||!strcmp(stage,stages[i])){
                char p[PATH_CAP],name[64],buffer[8192];
                snprintf(name,sizeof(name),"%s.log",stages[i]);join(p,path,name);
                int fd=open(p,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)continue;
                struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode)){close(fd);continue;}
                if(cursors[i].inode!=st.st_ino||cursors[i].offset>st.st_size){cursors[i].inode=st.st_ino;cursors[i].offset=0;}
                if(lseek(fd,cursors[i].offset,SEEK_SET)>=0){ssize_t k;while((k=read(fd,buffer,sizeof(buffer)))>0){if(write_all(1,buffer,(size_t)k)){close(fd);return 1;}cursors[i].offset+=k;}}
                close(fd);
            }
            usleep(250000);
        }
        return 0;
    }
    if(!strcmp(argv[1],"export")&&output&&output[0]=='/'&&!stage){
        int rc=log_archive_create(root,boot,output);
        if(rc)fprintf(stderr,"[logctl] [ERROR] export failed: %s\n",strerror(-rc));
        return rc?1:0;
    }
usage:
    fprintf(stderr,"Usage: logctl sync-time | verify-storage | verify-boot\n"
        "       logctl status | boots | losses | show [--boot ID|previous] [--stage STAGE]\n"
        "       logctl follow [--boot ID|previous] [--stage STAGE]\n"
        "       logctl export [--boot ID|previous] --output /path/logs.tar.gz\n"
        "       logctl write STAGE MESSAGE | run STAGE COMMAND [ARGS...]\n");return 2;
}
int main(int argc,char **argv)
{
    umask(077);setup();const char *name=strrchr(argv[0],'/');name=name?name+1:argv[0];
    return !strcmp(name,"logctld")?daemon_main(argc,argv):cli_main(argc,argv);
}
