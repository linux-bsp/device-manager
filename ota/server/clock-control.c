/* Durable clock configuration requests, device acknowledgements and status. */
#define _GNU_SOURCE
#include "clock-control.h"
#include "sol.h"
#include "../shared/time-source.h"
#include <errno.h>
#include <sqlite3.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int identifier(const char *p)
{
    if(!*p||strlen(p)>63||!strcmp(p,"viewer"))return 0;
    for(;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='-'||*p=='_'))return 0;
    return 1;
}
static sqlite3 *database(const char *root)
{
    char path[4096];sqlite3 *db=NULL;struct stat st;
    if(snprintf(path,sizeof(path),"%s/clock.db",root)>=(int)sizeof(path))return NULL;
    if(!lstat(path,&st)&&!S_ISREG(st.st_mode))return NULL;
    if(sqlite3_open(path,&db)!=SQLITE_OK)goto fail;
    chmod(path,0600);sqlite3_busy_timeout(db,1000);
    if(sqlite3_exec(db,"PRAGMA journal_mode=WAL;PRAGMA synchronous=FULL;"
        "CREATE TABLE IF NOT EXISTS requests(id INTEGER PRIMARY KEY AUTOINCREMENT,device TEXT NOT NULL,enabled INTEGER,interval INTEGER,source TEXT,created INTEGER,completed INTEGER DEFAULT 0,error INTEGER DEFAULT -1);"
        "CREATE INDEX IF NOT EXISTS clock_device ON requests(device,id);"
        "CREATE TABLE IF NOT EXISTS status(device TEXT PRIMARY KEY,seen INTEGER,peer TEXT,report TEXT);",NULL,NULL,NULL)!=SQLITE_OK)goto fail;
    return db;
fail:if(db)sqlite3_close(db);return NULL;
}
static void json_string(FILE *f,const char *s)
{
    fputc('"',f);for(const unsigned char *p=(const unsigned char *)s;*p;p++){
        if(*p=='"'||*p=='\\')fprintf(f,"\\%c",*p);
        else if(*p<32)fprintf(f,"\\u%04x",*p);else fputc(*p,f);
    }fputc('"',f);
}
static void list(int client,sqlite3 *db,bool head)
{
    sqlite3_stmt *s=NULL;char *json=NULL;size_t length=0;FILE *f=open_memstream(&json,&length);
    if(!f){http_text(client,503,"Unavailable","Out of memory\n",head);return;}
    fprintf(f,"{\"server_time\":%lld,\"devices\":[",(long long)time(NULL));
    const char *sql="SELECT d.device,COALESCE(s.seen,0),COALESCE(s.peer,''),COALESCE(s.report,''),COALESCE(r.id,0),COALESCE(r.enabled,1),COALESCE(r.interval,3600),COALESCE(r.source,'-'),COALESCE(r.created,0),COALESCE(r.completed,0),COALESCE(r.error,-1) FROM (SELECT device FROM status UNION SELECT device FROM requests) d LEFT JOIN status s ON d.device=s.device LEFT JOIN requests r ON r.id=(SELECT MAX(id) FROM requests WHERE device=d.device) ORDER BY d.device";
    int error=sqlite3_prepare_v2(db,sql,-1,&s,NULL)!=SQLITE_OK,first=1,step=SQLITE_DONE;
    while(!error&&(step=sqlite3_step(s))==SQLITE_ROW){
        if(!first)fputc(',',f);
        first=0;
        fputs("{\"device\":",f);json_string(f,(const char *)sqlite3_column_text(s,0));
        fprintf(f,",\"seen\":%lld,\"peer\":",sqlite3_column_int64(s,1));json_string(f,(const char *)sqlite3_column_text(s,2));
        fputs(",\"report\":",f);json_string(f,(const char *)sqlite3_column_text(s,3));
        fprintf(f,",\"revision\":%lld,\"enabled\":%d,\"interval\":%d,\"source\":",sqlite3_column_int64(s,4),sqlite3_column_int(s,5),sqlite3_column_int(s,6));
        json_string(f,(const char *)sqlite3_column_text(s,7));
        fprintf(f,",\"created\":%lld,\"completed\":%lld,\"error\":%d}",sqlite3_column_int64(s,8),sqlite3_column_int64(s,9),sqlite3_column_int(s,10));
    }
    if(step!=SQLITE_DONE)error=1;
    sqlite3_finalize(s);fputs("]}",f);fclose(f);
    if(error)http_text(client,503,"Unavailable","Clock database unavailable\n",head);
    else http_json(client,200,"OK",json,head);
    free(json);
}
static int read_body(int client,const struct http_request *r,char body[2048])
{
    if(!r->has_length||r->length==0||r->length>=2048||r->expect)return -1;
    struct timeval timeout={.tv_sec=2};setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    size_t got=0;struct timespec start,now;clock_gettime(CLOCK_MONOTONIC,&start);
    while(got<r->length){
        clock_gettime(CLOCK_MONOTONIC,&now);if(now.tv_sec-start.tv_sec>5)return -1;
        ssize_t n=recv(client,body+got,(size_t)r->length-got,0);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)return -1;
        got+=(size_t)n;
    }
    if(memchr(body,0,got))return -1;
    body[got]=0;return 0;
}
static int report(sqlite3 *db,const char *device,const char *peer,const char *body)
{
    long long applied,attempted,seconds,uptime;int error,n=0;char boot[64];
    if(sscanf(body,"DMCLOCKREPORT1 %lld %lld %d %lld %lld %63s%n",&applied,&attempted,&error,&seconds,&uptime,boot,&n)!=6||body[n]!='\n'||
       applied<0||attempted<0||(attempted&&!error&&applied!=attempted)||error<0||error>4095||seconds<0||seconds>=4102444800LL||uptime<0||!identifier(boot))return 400;
    /* Status values are bounded printable text, never interpreted as code/JSON. */
    const char *p=body+n+1;size_t fields=0;
    for(;*p;p++){if((*p<32&&*p!='\n')||*p>126)return 400;if(*p=='\n')fields++;}
    if(fields>20||strlen(body+n+1)>1400)return 400;
    sqlite3_stmt *s=NULL;int rc=503;
    if(sqlite3_exec(db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK)return rc;
    long long ids[2]={applied,attempted};
    for(int i=0;i<2;i++)if(ids[i]){
        if(sqlite3_prepare_v2(db,"SELECT id FROM requests WHERE id=? AND device=?",-1,&s,NULL)!=SQLITE_OK)goto out;
        sqlite3_bind_int64(s,1,ids[i]);sqlite3_bind_text(s,2,device,-1,SQLITE_TRANSIENT);
        int found=sqlite3_step(s)==SQLITE_ROW;sqlite3_finalize(s);s=NULL;
        if(!found){rc=409;goto out;}
    }
    if(attempted){
        if(sqlite3_prepare_v2(db,"UPDATE requests SET completed=?,error=? WHERE id=? AND device=? AND completed=0",-1,&s,NULL)!=SQLITE_OK)goto out;
        sqlite3_bind_int64(s,1,time(NULL));sqlite3_bind_int(s,2,error);sqlite3_bind_int64(s,3,attempted);sqlite3_bind_text(s,4,device,-1,SQLITE_TRANSIENT);
        if(sqlite3_step(s)!=SQLITE_DONE)goto out;
        sqlite3_finalize(s);s=NULL;
    }
    if(sqlite3_prepare_v2(db,"INSERT INTO status VALUES(?,?,?,?) ON CONFLICT(device) DO UPDATE SET seen=excluded.seen,peer=excluded.peer,report=excluded.report",-1,&s,NULL)!=SQLITE_OK)goto out;
    sqlite3_bind_text(s,1,device,-1,SQLITE_TRANSIENT);sqlite3_bind_int64(s,2,time(NULL));sqlite3_bind_text(s,3,peer,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,4,body,-1,SQLITE_TRANSIENT);
    if(sqlite3_step(s)!=SQLITE_DONE)goto out;
    sqlite3_finalize(s);s=NULL;
    if(sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)==SQLITE_OK)return 200;
out:sqlite3_finalize(s);sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);return rc;
}
bool clock_control_route(int client,const struct server_context *context,const struct http_request *r)
{
    if(strncmp(r->path,"/api/clock",10))return false;
    if(!strcmp(r->path,"/api/clock")&&(!strcmp(r->method,"GET")||r->head)){
        if(!sol_access(context->directory,"viewer",r->authorization)){http_text(client,403,"Forbidden","Access denied\n",r->head);return true;}
        sqlite3 *db=database(context->directory);if(db){list(client,db,r->head);sqlite3_close(db);}else http_text(client,503,"Unavailable","Clock database unavailable\n",r->head);
        return true;
    }
    const char *id=NULL;int admin=0;
    if(!strncmp(r->path,"/api/clock/config/",18)){id=r->path+18;admin=1;}
    else if(!strncmp(r->path,"/api/clock/device/",18))id=r->path+18;
    if(!id||!identifier(id)){http_text(client,400,"Bad Request","Invalid clock path\n",r->head);return true;}
    if((admin&&strcmp(r->method,"POST"))||(!admin&&strcmp(r->method,"GET")&&strcmp(r->method,"POST"))){http_text(client,405,"Method Not Allowed","Invalid clock method\n",r->head);return true;}
    char expected[512];snprintf(expected,sizeof(expected),"http://%s",r->host?r->host:"");
    if((admin&&(!r->admin||!r->host||(r->origin&&strcmp(r->origin,expected))||!sol_access(context->directory,"viewer",r->authorization)))||
       (!admin&&(r->origin||!sol_access(context->directory,id,r->authorization)))){
        http_text(client,403,"Forbidden","Clock access denied\n",r->head);return true;
    }
    char body[2048];if(!strcmp(r->method,"POST")&&read_body(client,r,body)){
        http_text(client,400,"Bad Request","Invalid clock body\n",false);return true;
    }
    sqlite3 *db=database(context->directory);if(!db){http_text(client,503,"Unavailable","Clock database unavailable\n",r->head);return true;}
    sqlite3_stmt *s=NULL;int status=503;
    if(admin){
        int enabled,interval,n=0;char source[512];
        if(sscanf(body,"DMCLOCK1 %d %d %511s%n",&enabled,&interval,source,&n)!=3||strcmp(body+n,"\n")||
           enabled<0||enabled>1||interval<60||interval>86400||!dm_source_valid(source))status=400;
        else if(sqlite3_prepare_v2(db,"INSERT INTO requests(device,enabled,interval,source,created) VALUES(?,?,?,?,?)",-1,&s,NULL)==SQLITE_OK){
            sqlite3_bind_text(s,1,id,-1,SQLITE_TRANSIENT);sqlite3_bind_int(s,2,enabled);sqlite3_bind_int(s,3,interval);sqlite3_bind_text(s,4,source,-1,SQLITE_TRANSIENT);sqlite3_bind_int64(s,5,time(NULL));
            if(sqlite3_step(s)==SQLITE_DONE){char json[128];snprintf(json,sizeof(json),"{\"revision\":%lld,\"status\":\"pending\"}",sqlite3_last_insert_rowid(db));http_json(client,202,"Accepted",json,false);status=0;}
        }
    }else if(!strcmp(r->method,"GET")){
        if(r->length)status=400;
        else if(sqlite3_prepare_v2(db,"SELECT id,enabled,interval,source FROM requests WHERE device=? ORDER BY id DESC LIMIT 1",-1,&s,NULL)==SQLITE_OK){
            sqlite3_bind_text(s,1,id,-1,SQLITE_TRANSIENT);int step=sqlite3_step(s);
            if(step==SQLITE_ROW){char value[640];snprintf(value,sizeof(value),"DMCLOCK1 %lld %d %d %s\n",sqlite3_column_int64(s,0),sqlite3_column_int(s,1),sqlite3_column_int(s,2),sqlite3_column_text(s,3));http_text(client,200,"OK",value,false);status=0;}
            else if(step==SQLITE_DONE){http_text(client,200,"OK","DMCLOCK1 0 1 3600 -\n",false);status=0;}
        }
    }else status=report(db,id,r->peer_ip,body);
    sqlite3_finalize(s);sqlite3_close(db);
    if(status)http_text(client,status,status==200?"OK":"Clock Request Failed",status==200?"ok\n":"Clock request rejected\n",r->head);
    return true;
}
