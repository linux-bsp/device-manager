/* Deletion tombstones prevent an offline uploader from recreating removed logs. */
#define _GNU_SOURCE
#include "sol-delete.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int reply(char **out,int code,const char *text)
{*out=strdup(text);return *out?code:500;}
static sqlite3_stmt *prepare(sqlite3 *db,const char *sql,const char *device,const char *boot)
{
    sqlite3_stmt *s=NULL;if(sqlite3_prepare_v2(db,sql,-1,&s,NULL)!=SQLITE_OK)return NULL;
    sqlite3_bind_text(s,1,device,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,2,boot,-1,SQLITE_TRANSIENT);return s;
}
static int done(sqlite3_stmt *s)
{if(!s)return -1;int error=sqlite3_step(s)!=SQLITE_DONE;sqlite3_finalize(s);return error?-1:0;}
int sol_delete_schema(sqlite3 *db)
{
    return sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS sol_deleted(device TEXT,boot TEXT,deleted_at INTEGER,peer TEXT,bytes INTEGER,PRIMARY KEY(device,boot));"
        "CREATE TABLE IF NOT EXISTS sol_deleted_streams(device TEXT,boot TEXT,stage TEXT,gen INTEGER,pos INTEGER,PRIMARY KEY(device,boot,stage,gen),FOREIGN KEY(device,boot) REFERENCES sol_deleted ON DELETE CASCADE);",NULL,NULL,NULL)==SQLITE_OK?0:-1;
}
int sol_deleted(sqlite3 *db,const char *device,const char *boot)
{
    sqlite3_stmt *s=prepare(db,"SELECT 1 FROM sol_deleted WHERE device=?1 AND boot=?2",device,boot);if(!s)return -1;
    int step=sqlite3_step(s);sqlite3_finalize(s);return step==SQLITE_ROW?1:step==SQLITE_DONE?0:-1;
}
int sol_delete_record(sqlite3 *db,const char *device,const char *boot,const char *peer,char **out)
{
    if(sqlite3_exec(db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK)return reply(out,503,"{\"error\":\"日志数据库忙，请重试。\"}");
    sqlite3_stmt *s=prepare(db,"SELECT CASE WHEN meta='' AND ord=(SELECT MAX(ord) FROM boots WHERE device=?1) THEN 'uploading' ELSE meta END FROM boots WHERE device=?1 AND boot=?2 UNION ALL SELECT CASE WHEN committed=0 THEN 'uploading' ELSE 'archive' END FROM startup_boots WHERE device=?1 AND boot=?2 UNION ALL SELECT CASE WHEN committed=0 THEN 'uploading' ELSE 'archive' END FROM bundles WHERE device=?1 AND boot=?2 AND committed>=0",device,boot);
    if(!s)goto fail;
    int step,found=0,protected=0;
    while((step=sqlite3_step(s))==SQLITE_ROW){
        found=1;const char *meta=(const char *)sqlite3_column_text(s,0);
        if(!meta||!strcmp(meta,"uploading")||!strncmp(meta,"current=yes\n",12)||strstr(meta,"\ncurrent=yes\n")||!strcmp(meta,"current=yes")||(strlen(meta)>=12&&!strcmp(meta+strlen(meta)-12,"\ncurrent=yes")))protected=1;
    }
    sqlite3_finalize(s);if(step!=SQLITE_DONE)goto fail;
    if(protected){sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);return reply(out,409,"{\"error\":\"正在采集或上传的启动记录不能删除。\"}");}
    int deleted=sol_deleted(db,device,boot);if(deleted<0)goto fail;
    if(!found&&!deleted){sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);return reply(out,404,"{\"error\":\"启动记录不存在。\"}");}
    s=prepare(db,"INSERT OR IGNORE INTO sol_deleted VALUES(?1,?2,?3,?4,(SELECT COALESCE(SUM(length(CAST(data AS BLOB))),0) FROM chunks WHERE device=?1 AND boot=?2)+(SELECT COALESCE(SUM(length(data)),0) FROM startup_streams WHERE device=?1 AND boot=?2)+(SELECT COALESCE(SUM(size),0) FROM bundles WHERE device=?1 AND boot=?2 AND committed>=0))",device,boot);
    if(!s)goto fail;
    sqlite3_bind_int64(s,3,time(NULL));sqlite3_bind_text(s,4,peer,-1,SQLITE_TRANSIENT);if(done(s))goto fail;
    if(done(prepare(db,"INSERT OR IGNORE INTO sol_deleted_streams SELECT device,boot,stage,gen,pos FROM streams WHERE device=?1 AND boot=?2",device,boot))||
       done(prepare(db,"INSERT OR IGNORE INTO sol_deleted_streams SELECT device,boot,stage,0,length(data) FROM startup_streams WHERE device=?1 AND boot=?2",device,boot))||
       done(prepare(db,"DELETE FROM boots WHERE device=?1 AND boot=?2",device,boot))||
       done(prepare(db,"DELETE FROM startup_boots WHERE device=?1 AND boot=?2",device,boot))||
       done(prepare(db,"UPDATE bundles SET committed=-1 WHERE device=?1 AND boot=?2",device,boot)))goto fail;
    if(sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)!=SQLITE_OK)goto fail;
    return reply(out,200,"{\"deleted\":true}");
fail:sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);return reply(out,503,"{\"error\":\"删除未完成，请重试。\"}");
}
int sol_deleted_upload(sqlite3 *db,const char *device,const char *boot,const char *stage,long long gen,long long offset,size_t length,char **out)
{
    int deleted=sol_deleted(db,device,boot);if(!deleted)return 0;
    if(deleted<0)return reply(out,503,"{\"error\":\"Deletion state unavailable\"}");
    long long next=0;
    if(strcmp(stage,"meta")&&strcmp(stage,"commit")){
        if(offset+(long long)length>4*1024*1024)return reply(out,400,"{\"error\":\"Invalid offset\"}");
        sqlite3_stmt *s=prepare(db,"INSERT INTO sol_deleted_streams VALUES(?1,?2,?3,?4,?5) ON CONFLICT(device,boot,stage,gen) DO UPDATE SET pos=MAX(pos,excluded.pos)",device,boot);
        if(!s)return reply(out,503,"{\"error\":\"Cursor unavailable\"}");
        sqlite3_bind_text(s,3,stage,-1,SQLITE_TRANSIENT);sqlite3_bind_int64(s,4,gen);sqlite3_bind_int64(s,5,offset+(long long)length);
        if(done(s))return reply(out,503,"{\"error\":\"Cursor unavailable\"}");
        s=prepare(db,"SELECT pos FROM sol_deleted_streams WHERE device=?1 AND boot=?2 AND stage=?3 AND gen=?4",device,boot);
        if(!s)return reply(out,503,"{\"error\":\"Cursor unavailable\"}");
        sqlite3_bind_text(s,3,stage,-1,SQLITE_TRANSIENT);sqlite3_bind_int64(s,4,gen);
        int step=sqlite3_step(s);if(step==SQLITE_ROW)next=sqlite3_column_int64(s,0);sqlite3_finalize(s);
        if(step!=SQLITE_ROW)return reply(out,503,"{\"error\":\"Cursor unavailable\"}");
    }
    sqlite3_stmt *trim=prepare(db,"DELETE FROM sol_deleted_streams WHERE device=?1 AND boot=?2 AND stage=?3 AND gen NOT IN(SELECT gen FROM sol_deleted_streams WHERE device=?1 AND boot=?2 AND stage=?3 ORDER BY gen DESC LIMIT 4)",device,boot);
    if(!trim)return reply(out,503,"{\"error\":\"Cursor unavailable\"}");
    sqlite3_bind_text(trim,3,stage,-1,SQLITE_TRANSIENT);if(done(trim))return reply(out,503,"{\"error\":\"Cursor unavailable\"}");
    char json[96];snprintf(json,sizeof(json),"{\"next\":%lld}",next);return reply(out,200,json);
}
