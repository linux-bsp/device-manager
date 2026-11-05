#define _GNU_SOURCE
#include "sol-retention.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int exec_bound(sqlite3 *db, const char *sql, const char *device,
                      const char *boot, const char *stage, long long gen)
{
    sqlite3_stmt *s=NULL;
    if(sqlite3_prepare_v2(db,sql,-1,&s,NULL)!=SQLITE_OK)return -1;
    if(device)sqlite3_bind_text(s,1,device,-1,SQLITE_TRANSIENT);
    if(boot)sqlite3_bind_text(s,2,boot,-1,SQLITE_TRANSIENT);
    if(stage)sqlite3_bind_text(s,3,stage,-1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(s,4,gen);
    int rc=sqlite3_step(s)==SQLITE_DONE?0:-1;sqlite3_finalize(s);return rc;
}

int sol_retention_schema(sqlite3 *db)
{
    /* A retention cursor always represents a contiguous byte prefix. Older
     * versions trimmed individual chunks but kept pos; reset such streams so
     * the device can resend, and preserve an audit event before deleting. */
    const char *sql=
        "CREATE INDEX IF NOT EXISTS chunks_stream ON chunks(device,boot,stage,gen,off);"
        "CREATE TABLE IF NOT EXISTS retention_events(id INTEGER PRIMARY KEY,device TEXT,boot TEXT,stage TEXT,gen INTEGER,bytes INTEGER,reason TEXT,created INTEGER);"
        "CREATE INDEX IF NOT EXISTS retention_device ON retention_events(device,id);"
        "CREATE TEMP TABLE IF NOT EXISTS broken_streams(device TEXT,boot TEXT,stage TEXT,gen INTEGER,pos INTEGER);"
        "BEGIN IMMEDIATE;DELETE FROM broken_streams;"
        "INSERT INTO broken_streams SELECT s.device,s.boot,s.stage,s.gen,s.pos FROM streams s LEFT JOIN chunks c ON c.device=s.device AND c.boot=s.boot AND c.stage=s.stage AND c.gen=s.gen GROUP BY s.device,s.boot,s.stage,s.gen HAVING s.pos!=coalesce(sum(length(CAST(c.data AS BLOB))),0) OR coalesce(min(c.off),0)!=0;"
        "INSERT INTO retention_events(device,boot,stage,gen,bytes,reason,created) SELECT device,boot,stage,gen,pos,'legacy-incomplete-stream',unixepoch() FROM broken_streams;"
        "DELETE FROM streams WHERE (device,boot,stage,gen) IN (SELECT device,boot,stage,gen FROM broken_streams);"
        "DELETE FROM retention_events WHERE id IN (SELECT id FROM (SELECT id,row_number() OVER(PARTITION BY device ORDER BY id DESC) n FROM retention_events) WHERE n>256);"
        "DELETE FROM broken_streams;COMMIT;";
    if(sqlite3_exec(db,sql,NULL,NULL,NULL)==SQLITE_OK)return 0;
    sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);return -1;
}

/* The caller holds the upload transaction. Whole boots or whole generations
 * leave no acknowledged holes; failures roll back the incoming chunk too. */
int sol_retention_apply(sqlite3 *db,const char *device,const char *boot,
                        const char *stage,long long generation)
{
    const char *sql=
        "INSERT INTO retention_events(device,boot,stage,gen,bytes,reason,created) SELECT device,boot,stage,gen,pos,'generation-limit',unixepoch() FROM streams WHERE device=?1 AND boot=?2 AND stage=?3 AND gen!=?4 AND gen NOT IN (SELECT gen FROM streams WHERE device=?1 AND boot=?2 AND stage=?3 AND gen!=?4 ORDER BY gen DESC LIMIT 3);";
    if(exec_bound(db,sql,device,boot,stage,generation)||exec_bound(db,
        "DELETE FROM streams WHERE device=?1 AND boot=?2 AND stage=?3 AND gen!=?4 AND gen NOT IN (SELECT gen FROM streams WHERE device=?1 AND boot=?2 AND stage=?3 AND gen!=?4 ORDER BY gen DESC LIMIT 3)",device,boot,stage,generation))return -1;
    for(;;) {
        sqlite3_stmt *s=NULL;
        if(sqlite3_prepare_v2(db,"SELECT (SELECT count(*) FROM boots WHERE device=?1), (SELECT count(*) FROM chunks WHERE device=?1)",-1,&s,NULL)!=SQLITE_OK)return -1;
        sqlite3_bind_text(s,1,device,-1,SQLITE_TRANSIENT);
        if(sqlite3_step(s)!=SQLITE_ROW){sqlite3_finalize(s);return -1;}
        int boots=sqlite3_column_int(s,0),chunks=sqlite3_column_int(s,1);sqlite3_finalize(s);
        if(boots<=SOL_KEEP_BOOTS&&chunks<=SOL_KEEP_CHUNKS)break;
        /* Do not discard the boot being uploaded or the newest boot. Backfill
         * arrival order must not replace the actual current session. */
        if(sqlite3_prepare_v2(db,"SELECT boot FROM boots WHERE device=?1 AND boot!=?2 AND boot!=(SELECT boot FROM boots WHERE device=?1 ORDER BY ord DESC,rowid DESC LIMIT 1) ORDER BY ord,rowid LIMIT 1",-1,&s,NULL)!=SQLITE_OK)return -1;
        sqlite3_bind_text(s,1,device,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,2,boot,-1,SQLITE_TRANSIENT);
        int step=sqlite3_step(s);char *victim=step==SQLITE_ROW?strdup((const char *)sqlite3_column_text(s,0)):NULL;sqlite3_finalize(s);
        if(victim) {
            int rc=exec_bound(db,"INSERT INTO retention_events(device,boot,stage,gen,bytes,reason,created) SELECT ?1,?2,'all',0,coalesce(sum(pos),0),'boot-or-capacity-limit',unixepoch() FROM streams WHERE device=?1 AND boot=?2",device,victim,NULL,0)
                ||exec_bound(db,"DELETE FROM boots WHERE device=?1 AND boot=?2",device,victim,NULL,0);
            free(victim);if(rc)return -1;
        } else {
            /* One busy boot may fill the quota. Evict a complete old stream,
             * never the upload stream; if no such stream exists, reject writes. */
            if(exec_bound(db,"INSERT INTO retention_events(device,boot,stage,gen,bytes,reason,created) SELECT device,boot,stage,gen,pos,'capacity-limit',unixepoch() FROM streams WHERE rowid=(SELECT rowid FROM streams WHERE device=?1 AND NOT(boot=?2 AND stage=?3 AND gen=?4) ORDER BY rowid LIMIT 1)",device,boot,stage,generation))return -1;
            if(exec_bound(db,"DELETE FROM streams WHERE rowid=(SELECT rowid FROM streams WHERE device=?1 AND NOT(boot=?2 AND stage=?3 AND gen=?4) ORDER BY rowid LIMIT 1)",device,boot,stage,generation)||!sqlite3_changes(db))return -1;
        }
    }
    return exec_bound(db,"DELETE FROM retention_events WHERE device=?1 AND id NOT IN (SELECT id FROM retention_events WHERE device=?1 ORDER BY id DESC LIMIT 256)",device,NULL,NULL,0);
}

int sol_retention_query(sqlite3 *db,const char *device,char **out)
{
    sqlite3_stmt *s=NULL;
    if(sqlite3_prepare_v2(db,"SELECT json_object('boot',boot,'stage',stage,'generation',gen,'bytes',bytes,'reason',reason,'time',created) FROM retention_events WHERE device=?1 ORDER BY id DESC LIMIT 256",-1,&s,NULL)!=SQLITE_OK)return 503;
    sqlite3_bind_text(s,1,device,-1,SQLITE_TRANSIENT);
    size_t length=0;FILE *f=open_memstream(out,&length);
    if(!f){sqlite3_finalize(s);return 500;}
    fputc('[',f);int count=0,step;
    while((step=sqlite3_step(s))==SQLITE_ROW){if(count++)fputc(',',f);fputs((const char *)sqlite3_column_text(s,0),f);}
    fputc(']',f);int rc=fclose(f);sqlite3_finalize(s);
    if(step!=SQLITE_DONE||rc){free(*out);*out=NULL;return 503;}return 200;
}
