/* Startup snapshots are independent of the bounded live SOL chunk retention. */
#define _GNU_SOURCE
#include "sol-startup.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

static const char *stages[] = {"r5-spl", "a53-spl", "uboot", "kernel"};
static const unsigned int limits[] = {32736, 32736, 65504, 327648};
static int reply(char **out, int code, const char *text)
{ *out = strdup(text); return *out ? code : 500; }
static int safe(const char *s)
{
    if (!*s || strlen(s) > 63) return 0;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_')) return 0;
    return 1;
}
static int stage_index(const char *s)
{ for (int i=0;i<4;i++) if (!strcmp(s,stages[i])) return i; return -1; }
static sqlite3_stmt *prepare(sqlite3 *db, const char *sql, const char *device, const char *boot, const char *stage)
{
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,sql,-1,&s,NULL)!=SQLITE_OK) return NULL;
    sqlite3_bind_text(s,1,device,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(s,2,boot,-1,SQLITE_TRANSIENT);
    if (stage) sqlite3_bind_text(s,3,stage,-1,SQLITE_TRANSIENT);
    return s;
}
static int done(sqlite3_stmt *s)
{ if (!s) return -1; int ok=sqlite3_step(s)==SQLITE_DONE;sqlite3_finalize(s);return ok?0:-1; }
int sol_startup_schema(sqlite3 *db)
{
    return sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS startup_boots(device TEXT,boot TEXT,seen INTEGER,peer TEXT,meta TEXT DEFAULT '',committed INTEGER DEFAULT 0,manifest TEXT DEFAULT '',PRIMARY KEY(device,boot));"
        "CREATE TABLE IF NOT EXISTS startup_streams(device TEXT,boot TEXT,stage TEXT,data BLOB NOT NULL DEFAULT X'',PRIMARY KEY(device,boot,stage),FOREIGN KEY(device,boot) REFERENCES startup_boots ON DELETE CASCADE);",
        NULL,NULL,NULL)==SQLITE_OK?0:-1;
}
/* Commit verifies all four finite streams, not merely the last received chunk. */
static int verify(sqlite3 *db, const char *device, const char *boot, const char *body, size_t length)
{
    unsigned int slot,current;
    unsigned long long generation;
    char recovery[64], name[32], extra;
    int consumed=0;
    if (memchr(body,0,length) || sscanf(body,"%u %llu %u %63s\n%n",&slot,&generation,&current,recovery,&consumed)!=4 ||
        slot>1 || !generation || current>1 || !safe(recovery)) return -1;
    const char *p=body+consumed;
    for (int i=0;i<4;i++) {
        unsigned int len,crc,dropped,state;
        consumed=0;
        if (sscanf(p,"%31s %u %x %u %u\n%n",name,&len,&crc,&dropped,&state,&consumed)!=5 ||
            strcmp(name,stages[i]) || len>limits[i] || state>3 || ((state==0||state==2)&&(len||crc))) return -1;
        sqlite3_stmt *s=prepare(db,"SELECT data FROM startup_streams WHERE device=?1 AND boot=?2 AND stage=?3",device,boot,name);
        if (!s) return -1;
        int row=sqlite3_step(s),n=row==SQLITE_ROW?sqlite3_column_bytes(s,0):0;
        const void *data=row==SQLITE_ROW?sqlite3_column_blob(s,0):NULL;
        int bad=(row!=SQLITE_ROW&&row!=SQLITE_DONE) || n!=(int)len || crc32(0,data,(unsigned int)n)!=crc;
        sqlite3_finalize(s);if(bad)return -1;
        p+=consumed;
    }
    return sscanf(p,"%c",&extra)==EOF?0:-1;
}
/* A lost final ACK may be retried from a later successful boot. Recovery
 * context can change; the bank and its finite stage manifest cannot. */
static int same_snapshot(const char *old, const char *body)
{
    unsigned a,b;unsigned long long x,y;
    const char *p=strchr(old,'\n'),*q=strchr(body,'\n');
    return p&&q&&sscanf(old,"%u %llu",&a,&x)==2&&sscanf(body,"%u %llu",&b,&y)==2&&a==b&&x==y&&!strcmp(p,q);
}
int sol_startup_request(sqlite3 *db, const char *method, const char *path,
                        const char *peer, const char *body, size_t length, char **out)
{
    char device[64],boot[64],stage[32],canonical[256],extra;
    long long offset;
    if(strcmp(method,"POST"))return reply(out,405,"{\"error\":\"Use POST\"}");
    if(sscanf(path,"/api/sol/startup/%63[^/]/%63[^/]/%31[^/]/%lld%c",device,boot,stage,&offset,&extra)!=4 ||
        !safe(device)||!safe(boot)||offset<0||offset>327648||length>4096)
        return reply(out,400,"{\"error\":\"Invalid startup archive path\"}");
    snprintf(canonical,sizeof(canonical),"/api/sol/startup/%s/%s/%s/%lld",device,boot,stage,offset);
    int commit=!strcmp(stage,"commit"),index=stage_index(stage);
    if(strcmp(path,canonical)||(!commit&&index<0)||(commit&&offset))
        return reply(out,400,"{\"error\":\"Invalid startup archive stream\"}");
    if(sqlite3_exec(db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK)goto failure;
    sqlite3_stmt *s=prepare(db,"INSERT OR IGNORE INTO startup_boots(device,boot,seen,peer) VALUES(?1,?2,?3,?4)",device,boot,NULL);
    if(!s)goto rollback;
    sqlite3_bind_int64(s,3,time(NULL));sqlite3_bind_text(s,4,peer,-1,SQLITE_TRANSIENT);
    if(done(s))goto rollback;
    s=prepare(db,"SELECT committed,manifest FROM startup_boots WHERE device=?1 AND boot=?2",device,boot,NULL);
    if(!s)goto rollback;
    if(sqlite3_step(s)!=SQLITE_ROW){sqlite3_finalize(s);goto rollback;}
    int committed=sqlite3_column_int(s,0);
    int identical=committed&&!memchr(body,0,length)&&same_snapshot((const char *)sqlite3_column_text(s,1),body);
    sqlite3_finalize(s);
    if(commit) {
        if((committed&&!identical)||(!committed&&verify(db,device,boot,body,length)))goto conflict;
        if(committed){
            if(sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)!=SQLITE_OK)goto rollback;
            return reply(out,200,"{\"next\":1}");
        }
        char recovery[64];unsigned slot,current;unsigned long long generation;
        if(sscanf(body,"%u %llu %u %63s",&slot,&generation,&current,recovery)!=4)goto conflict;
        char meta[1024];int meta_size=snprintf(meta,sizeof(meta),"startup=%s\ncurrent=no\nrecovery-boot=%s\nbuffer=%u\ngeneration=%llu\narchive=complete\n",current?"healthy-current":"previous-unconfirmed",recovery,slot,generation);
        const char *details=strchr(body,'\n');
        if(details&&meta_size>0&&(size_t)meta_size<sizeof(meta))snprintf(meta+meta_size,sizeof(meta)-(size_t)meta_size,"stage-fields=bytes crc32 dropped status(0=absent,1=valid,2=corrupt,3=recovered-prefix)\n%s",details+1);
        s=prepare(db,"UPDATE startup_boots SET committed=1,manifest=?3,meta=?4 WHERE device=?1 AND boot=?2",device,boot,NULL);
        if(!s)goto rollback;
        sqlite3_bind_text(s,3,body,(int)length,SQLITE_TRANSIENT);sqlite3_bind_text(s,4,meta,-1,SQLITE_TRANSIENT);
        if(done(s))goto rollback;
        if(sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)!=SQLITE_OK)goto rollback;
        return reply(out,200,"{\"next\":1}");
    }
    s=prepare(db,"SELECT length(data) FROM startup_streams WHERE device=?1 AND boot=?2 AND stage=?3",device,boot,stage);
    if(!s)goto rollback;
    int rc=sqlite3_step(s);long long next=rc==SQLITE_ROW?sqlite3_column_int64(s,0):0;
    sqlite3_finalize(s);if(rc!=SQLITE_ROW&&rc!=SQLITE_DONE)goto rollback;
    if(length&&offset==next&&!committed) {
        if(next+(long long)length>limits[index])goto conflict;
        s=prepare(db,"INSERT INTO startup_streams(device,boot,stage,data) VALUES(?1,?2,?3,?4) ON CONFLICT(device,boot,stage) DO UPDATE SET data=CAST(data || excluded.data AS BLOB)",device,boot,stage);
        if(!s)goto rollback;
        sqlite3_bind_blob(s,4,body,(int)length,SQLITE_TRANSIENT);
        if(done(s))goto rollback;
        next+=(long long)length;
    }
    if(sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)!=SQLITE_OK)goto rollback;
    char response[64];snprintf(response,sizeof(response),"{\"next\":%lld}",next);
    return reply(out,200,response);
conflict:
    sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);
    return reply(out,409,"{\"error\":\"Startup archive incomplete or immutable\"}");
rollback:
    sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);
failure:
    return reply(out,503,"{\"error\":\"Startup archive not committed\"}");
}
