/* Finite opaque boot archives. Transfer cursors and archive receipts differ. */
#define _GNU_SOURCE
#include "sol-bundle.h"
#include "sol.h"
#include "sol-delete.h"
#include "downloads.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#define BUNDLE_MAX (32ULL * 1024 * 1024)
#define BUNDLE_QUOTA (256ULL * 1024 * 1024)
static int safe(const char *s)
{
    if(!*s||strlen(s)>63)return 0;
    for(;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='0'&&*s<='9')||*s=='-'||*s=='_'))return 0;
    return 1;
}
static int digest_valid(const char *s)
{
    if(strlen(s)!=64)return 0;
    for(;*s;s++)if(!((*s>='a'&&*s<='f')||(*s>='0'&&*s<='9')))return 0;
    return 1;
}
static sqlite3_stmt *statement(sqlite3 *db,const char *sql,const char *device,const char *boot)
{
    sqlite3_stmt *s=NULL;if(sqlite3_prepare_v2(db,sql,-1,&s,NULL)!=SQLITE_OK)return NULL;
    if(device)sqlite3_bind_text(s,1,device,-1,SQLITE_TRANSIENT);
    if(boot)sqlite3_bind_text(s,2,boot,-1,SQLITE_TRANSIENT);
    return s;
}
static int done(sqlite3_stmt *s)
{if(!s)return -1;int rc=sqlite3_step(s)==SQLITE_DONE?0:-1;sqlite3_finalize(s);return rc;}
int sol_bundle_schema(sqlite3 *db)
{
    return sqlite3_exec(db,"CREATE TABLE IF NOT EXISTS bundles(device TEXT,boot TEXT,sha TEXT,size INTEGER,pos INTEGER DEFAULT 0,committed INTEGER DEFAULT 0,received INTEGER DEFAULT 0,ord INTEGER,peer TEXT,PRIMARY KEY(device,boot));",NULL,NULL,NULL)==SQLITE_OK?0:-1;
}
static int subdir(int root,const char *name)
{
    if(mkdirat(root,name,0700)&&errno!=EEXIST)return -1;
    int fd=openat(root,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd>=0){struct stat st;if(fstat(fd,&st)||st.st_uid!=geteuid()||(st.st_mode&0077)){close(fd);return -1;}}
    return fd;
}
static int directory(const struct server_context *context,const char *device)
{
    int root=subdir(context->root,"sol-bundles");if(root<0)return -1;
    int dir=subdir(root,device);if(dir>=0&&(fsync(root)||fsync(context->root))){close(dir);dir=-1;}
    close(root);return dir;
}
static int cleanup(sqlite3 *db,int dir,const char *device)
{
    sqlite3_stmt *s=statement(db,"SELECT boot FROM bundles WHERE device=?1 AND committed=-1",device,NULL);
    if(!s)return -1;
    int step,rc=0;
    while((step=sqlite3_step(s))==SQLITE_ROW){
        const char *boot=(const char *)sqlite3_column_text(s,0);char path[96];
        if(!safe(boot)){rc=-1;break;}
        for(int i=0;i<2;i++){
            snprintf(path,sizeof(path),"%s.%s",boot,i?"tar.gz":"part");
            if(unlinkat(dir,path,0)&&errno!=ENOENT)rc=-1;
        }
    }
    sqlite3_finalize(s);if(step!=SQLITE_DONE)rc=-1;
    if(fsync(dir))rc=-1;
    return rc;
}
/* Audit logical eviction before unlink. A failed unlink is retried and blocks
 * further ingestion rather than growing unbounded orphan archive files. */
static int reserve(sqlite3 *db,int dir,const char *device,unsigned long long incoming)
{
    /* A lost client cache cannot reserve the device indefinitely. A day without
     * progress expires only the partial package, with a durable loss record. */
    if(sqlite3_exec(db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK)return -1;
    int expired=done(statement(db,"INSERT INTO retention_events(device,boot,stage,gen,bytes,reason,created) SELECT device,boot,'bundle',0,pos,'bundle-timeout',unixepoch() FROM bundles WHERE device=?1 AND committed=0 AND received<unixepoch()-86400",device,NULL)) ||
        done(statement(db,"UPDATE bundles SET committed=-1 WHERE device=?1 AND committed=0 AND received<unixepoch()-86400",device,NULL));
    if(expired||sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)!=SQLITE_OK){sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);return -1;}

    if(cleanup(db,dir,device))return -1;
    for(;;){
        sqlite3_stmt *s=statement(db,"SELECT count(*),coalesce(sum(size),0),sum(committed=0) FROM bundles WHERE device=?1 AND committed>=0",device,NULL);
        if(!s)return -1;
        if(sqlite3_step(s)!=SQLITE_ROW){sqlite3_finalize(s);return -1;}
        int count=sqlite3_column_int(s,0),pending=sqlite3_column_int(s,2);
        unsigned long long bytes=(unsigned long long)sqlite3_column_int64(s,1);sqlite3_finalize(s);
        if(pending)return -1;
        if(count<16&&bytes+incoming<=BUNDLE_QUOTA)return 0;
        s=statement(db,"SELECT boot FROM bundles WHERE device=?1 AND committed=1 ORDER BY ord,received LIMIT 1",device,NULL);
        if(!s)return -1;
        if(sqlite3_step(s)!=SQLITE_ROW){sqlite3_finalize(s);return -1;}
        char boot[64];snprintf(boot,sizeof(boot),"%s",sqlite3_column_text(s,0));sqlite3_finalize(s);
        if(sqlite3_exec(db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK)return -1;
        int rc=done(statement(db,"INSERT INTO retention_events(device,boot,stage,gen,bytes,reason,created) SELECT device,boot,'bundle',0,size,'bundle-quota',unixepoch() FROM bundles WHERE device=?1 AND boot=?2",device,boot)) ||
            done(statement(db,"UPDATE bundles SET committed=-1 WHERE device=?1 AND boot=?2",device,boot));
        if(rc||sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)!=SQLITE_OK){sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);return -1;}
        if(cleanup(db,dir,device))return -1;
        if(done(statement(db,"DELETE FROM retention_events WHERE device=?1 AND id NOT IN(SELECT id FROM retention_events WHERE device=?1 ORDER BY id DESC LIMIT 256)",device,NULL)))return -1;
    }
}
static void reply(int client,int status,const char *text)
{http_json(client,status,status==200?"OK":"Archive Request Failed",text,false);}
static void cursor(int client,unsigned long long next)
{char text[96];snprintf(text,sizeof(text),"{\"next\":%llu}",next);reply(client,200,text);}

bool sol_bundle_route(int client,const struct server_context *context,const struct http_request *request)
{
    if(strncmp(request->path,"/api/sol/bundles",16))return false;
    sqlite3 *db=sol_storage(context->directory);
    if(!db){reply(client,503,"{\"error\":\"Archive catalog unavailable\"}");return true;}
    char device[64],boot[64],sha[65],action[32],extra,canonical[512];
    unsigned long long size,order;
    int writing=!strcmp(request->method,"POST");
    if(!writing&&!strcmp(request->path,"/api/sol/bundles")){
        if(strcmp(request->method,"GET")||request->length||!sol_view_access(context->directory,request->authorization)){reply(client,403,"{\"error\":\"Archive view rejected\"}");return true;}
        sqlite3_stmt *s=statement(db,"SELECT json_object('device',device,'boot',boot,'hash',sha,'size',size,'received',received,'first_connected_at',coalesce((SELECT first_seen FROM sol_connections c WHERE c.device=bundles.device AND c.boot=bundles.boot),received),'order',ord,'peer',peer) FROM bundles WHERE committed=1 ORDER BY device,ord DESC",NULL,NULL);
        if(!s){reply(client,503,"{}");return true;}
        char *text=NULL;size_t n=0;FILE *f=open_memstream(&text,&n);int step=SQLITE_ERROR,count=0;
        if(f){fputc('[',f);while((step=sqlite3_step(s))==SQLITE_ROW){if(count++)fputc(',',f);fputs((const char *)sqlite3_column_text(s,0),f);}fputc(']',f);if(fclose(f))step=SQLITE_ERROR;}
        sqlite3_finalize(s);reply(client,step==SQLITE_DONE?200:503,text?text:"{}");free(text);return true;
    }
    if(!writing){
        if(sscanf(request->path,"/api/sol/bundles/%63[^/]/%63[^/]%c",device,boot,&extra)!=2||!safe(device)||!safe(boot))goto invalid;
        snprintf(canonical,sizeof(canonical),"/api/sol/bundles/%s/%s",device,boot);
        if(strcmp(canonical,request->path)||strcmp(request->method,"GET")||request->length)goto invalid;
        if(!sol_view_access(context->directory,request->authorization)){reply(client,403,"{}");return true;}
        sqlite3_stmt *s=statement(db,"SELECT size,received FROM bundles WHERE device=?1 AND boot=?2 AND committed=1",device,boot);
        if(!s){reply(client,503,"{}");return true;}
        if(sqlite3_step(s)!=SQLITE_ROW){sqlite3_finalize(s);reply(client,404,"{\"error\":\"Archive unavailable\"}");return true;}
        size=(unsigned long long)sqlite3_column_int64(s,0);time_t stamp=(time_t)sqlite3_column_int64(s,1)+8*3600;sqlite3_finalize(s);
        int dir=directory(context,device);char path[96];snprintf(path,sizeof(path),"%s.tar.gz",boot);
        int fd=dir<0?-1:openat(dir,path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);if(dir>=0)close(dir);
        struct stat st;if(fd<0||fstat(fd,&st)||!S_ISREG(st.st_mode)||(unsigned long long)st.st_size!=size){if(fd>=0)close(fd);reply(client,503,"{\"error\":\"Archive data unavailable\"}");return true;}
        struct tm tm;gmtime_r(&stamp,&tm);char date[40],filename[128];strftime(date,sizeof(date),"%Y-%m-%d_%H-%M-%S+0800",&tm);
        snprintf(filename,sizeof(filename),"%s_boot-%.8s.tar.gz",date,boot);
        struct dm_transfer_serve serve={.socket=client,.fd=fd,.size=size,.range=request->range,.content_type="application/gzip",.filename=filename};
        server_download(&serve,NULL,0,NULL,0,context->response_deferred);close(fd);return true;
    }
    if(sscanf(request->path,"/api/sol/bundles/%63[^/]/%63[^/]/%64[^/]/%llu/%llu/%31[^/]%c",device,boot,sha,&size,&order,action,&extra)!=6||!safe(device)||!safe(boot)||!strcmp(device,"viewer")||!digest_valid(sha)||!size||size>BUNDLE_MAX||!order||order>0x7fffffffffffffffULL)goto invalid;
    snprintf(canonical,sizeof(canonical),"/api/sol/bundles/%s/%s/%s/%llu/%llu/%s",device,boot,sha,size,order,action);
    if(strcmp(canonical,request->path)||request->origin||request->expect||!request->has_length||request->length>4096)goto invalid;
    if(!sol_access(context->directory,device,request->authorization)){reply(client,403,"{\"error\":\"Device credential rejected\"}");return true;}
    int commit=!strcmp(action,"commit");unsigned long long offset=0;
    if(!commit){char number[32];if(sscanf(action,"%llu%c",&offset,&extra)!=1||offset>size)goto invalid;snprintf(number,sizeof(number),"%llu",offset);if(strcmp(number,action)||request->length>size-offset)goto invalid;}
    else if(request->length)goto invalid;
    char body[4096];if(dm_transfer_receive_exact(client,body,(size_t)request->length,5000,NULL)){reply(client,400,"{\"error\":\"Incomplete archive chunk\"}");return true;}
    int dir=directory(context,device);if(dir<0){reply(client,503,"{\"error\":\"Archive storage unavailable\"}");return true;}
    if(flock(dir,LOCK_EX|LOCK_NB))goto unavailable;
    int deleted=sol_deleted(db,device,boot);
    if(deleted<0)goto unavailable;
    if(deleted){close(dir);reply(client,410,"{\"error\":\"Archive deleted\"}");return true;}
    sqlite3_stmt *s=statement(db,"SELECT sha,size,pos,committed,ord FROM bundles WHERE device=?1 AND boot=?2",device,boot);
    if(!s)goto unavailable;
    int step=sqlite3_step(s),state=0;unsigned long long pos=0;
    if(step==SQLITE_ROW){
        int mismatch=strcmp(sha,(const char *)sqlite3_column_text(s,0))||size!=(unsigned long long)sqlite3_column_int64(s,1)||order!=(unsigned long long)sqlite3_column_int64(s,4);
        pos=(unsigned long long)sqlite3_column_int64(s,2);state=sqlite3_column_int(s,3);sqlite3_finalize(s);
        if(state<0){close(dir);reply(client,410,"{\"error\":\"Archive expired\"}");return true;}
        if(mismatch&&state==0&&!commit&&!offset&&!request->length){
            /* A client rebuilding a lost/corrupt cache may replace only an
             * unfinished package. Completed evidence is immutable. */
            s=statement(db,"SELECT coalesce(sum(size),0)+?3<=268435456 FROM bundles WHERE device=?1 AND boot!=?2 AND committed>=0",device,boot);
            if(!s)goto unavailable;
            sqlite3_bind_int64(s,3,(sqlite3_int64)size);
            int fits=sqlite3_step(s)==SQLITE_ROW&&sqlite3_column_int(s,0);sqlite3_finalize(s);
            if(!fits||sqlite3_exec(db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK)goto unavailable;
            int rc=done(statement(db,"INSERT INTO retention_events(device,boot,stage,gen,bytes,reason,created) SELECT device,boot,'bundle',0,pos,'bundle-restarted',unixepoch() FROM bundles WHERE device=?1 AND boot=?2",device,boot));
            s=statement(db,"UPDATE bundles SET sha=?3,size=?4,pos=0,ord=?5,received=unixepoch() WHERE device=?1 AND boot=?2",device,boot);
            if(s){sqlite3_bind_text(s,3,sha,-1,SQLITE_TRANSIENT);sqlite3_bind_int64(s,4,(sqlite3_int64)size);sqlite3_bind_int64(s,5,(sqlite3_int64)order);}
            if(done(s))rc=1;
            if(rc||sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)!=SQLITE_OK){sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);goto unavailable;}
            pos=0;
        }else if(mismatch){close(dir);reply(client,409,"{\"error\":\"Archive identity conflict\"}");return true;}
    } else {
        sqlite3_finalize(s);if(step!=SQLITE_DONE||commit||reserve(db,dir,device,size))goto unavailable;
        s=statement(db,"INSERT INTO bundles(device,boot,sha,size,ord,peer,received) SELECT ?1,?2,?3,?4,?5,?6,unixepoch() WHERE NOT EXISTS(SELECT 1 FROM sol_deleted WHERE device=?1 AND boot=?2)",device,boot);
        if(!s)goto unavailable;
        sqlite3_bind_text(s,3,sha,-1,SQLITE_TRANSIENT);sqlite3_bind_int64(s,4,(sqlite3_int64)size);sqlite3_bind_int64(s,5,(sqlite3_int64)order);sqlite3_bind_text(s,6,request->peer_ip,-1,SQLITE_TRANSIENT);
        if(done(s)||sqlite3_changes(db)!=1)goto unavailable;
    }
    char partial[96],final[96];snprintf(partial,sizeof(partial),"%s.part",boot);snprintf(final,sizeof(final),"%s.tar.gz",boot);
    int fd=openat(dir,state?final:partial,(state?O_RDONLY:O_RDWR|O_CREAT)|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);
    if(fd<0)goto unavailable;
    struct stat st;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_nlink!=1){close(fd);goto unavailable;}
    if(state){
        if((unsigned long long)st.st_size!=size){close(fd);goto unavailable;}
        if(commit){char actual[65];if(dm_transfer_sha256_fd(fd,size,actual,NULL)||strcmp(actual,sha)){close(fd);goto unavailable;}}
        close(fd);close(dir);cursor(client,commit?1:size);return true;
    }
    if((unsigned long long)st.st_size<pos)pos=0;
    if(ftruncate(fd,(off_t)pos)){close(fd);goto unavailable;}
    if(!commit&&request->length&&offset==pos){
        size_t n=0;while(n<request->length){ssize_t k=pwrite(fd,body+n,(size_t)request->length-n,(off_t)(pos+n));if(k<0&&errno==EINTR)continue;if(k<=0){close(fd);goto unavailable;}n+=(size_t)k;}pos+=n;
    }
    if(fsync(fd)||fsync(dir)){close(fd);goto unavailable;}
    if(commit){
        char actual[65];
        if(pos!=size){close(fd);close(dir);cursor(client,0);return true;}
        if(dm_transfer_sha256_fd(fd,size,actual,NULL)||strcmp(actual,sha)){
            if(ftruncate(fd,0)||fsync(fd)){close(fd);goto unavailable;}
            close(fd);
            if(done(statement(db,"UPDATE bundles SET pos=0 WHERE device=?1 AND boot=?2",device,boot)))goto unavailable;
            close(dir);cursor(client,0);return true;
        }
        if(renameat(dir,partial,dir,final)||fsync(dir)){close(fd);goto unavailable;}
    }
    close(fd);
    s=statement(db,"UPDATE bundles SET pos=?3,committed=?4,received=unixepoch() WHERE device=?1 AND boot=?2 AND committed>=0 AND NOT EXISTS(SELECT 1 FROM sol_deleted WHERE device=?1 AND boot=?2)",device,boot);
    if(!s)goto unavailable;
    sqlite3_bind_int64(s,3,(sqlite3_int64)pos);sqlite3_bind_int(s,4,commit?1:0);
    if(done(s)||sqlite3_changes(db)!=1)goto unavailable;
    close(dir);cursor(client,commit?1:pos);return true;
unavailable:
    close(dir);reply(client,503,"{\"error\":\"Archive not committed\"}");return true;
invalid:
    reply(client,400,"{\"error\":\"Invalid archive request\"}");return true;
}

int sol_bundle_cleanup(const struct server_context *context,const char *path)
{
    char device[64],boot[64],extra;
    if(sscanf(path,"/api/sol/records/%63[^/]/%63[^/]%c",device,boot,&extra)!=2||!safe(device)||!safe(boot))return -1;
    sqlite3 *db=sol_storage(context->directory);
    int dir=directory(context,device);
    if(!db||dir<0){if(dir>=0)close(dir);return -1;}
    int rc=flock(dir,LOCK_EX|LOCK_NB)?-1:cleanup(db,dir,device);
    close(dir);return rc;
}
