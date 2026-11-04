/* Bind finite early-boot snapshots to the transaction's recovery context.
 * Capture UUIDs are not Linux boot UUIDs. No short firmware hash matching. */
#define _GNU_SOURCE
#include "sol-startup-evidence.h"
#include "ota-device.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static const char *stages[]={"r5-spl","a53-spl","uboot","kernel"};
static const char *terminal="('rolled-back','interrupted','device-error','reboot-failed','verify-failed')";
struct context {unsigned slot,current;unsigned long long generation;char recovery[64];};
static int parse(const char *text,struct context *c)
{
    return !text||sscanf(text,"%u %llu %u %63s",&c->slot,&c->generation,&c->current,c->recovery)!=4||
        c->slot>1||c->current>1||!c->generation||c->generation>LLONG_MAX||!strcmp(c->recovery,"unknown")||
        !*c->recovery||strspn(c->recovery,"abcdefghijklmnopqrstuvwxyz0123456789-_")!=strlen(c->recovery)?-1:0;
}
static sqlite3_stmt *query(sqlite3 *db,const char *sql,const struct ota_report *r)
{
    sqlite3_stmt *s=NULL;if(sqlite3_prepare_v2(db,sql,-1,&s,NULL)!=SQLITE_OK)return NULL;
    sqlite3_bind_text(s,1,r->device,-1,SQLITE_TRANSIENT);sqlite3_bind_int64(s,2,(sqlite3_int64)r->publication);sqlite3_bind_text(s,3,r->boot,-1,SQLITE_TRANSIENT);return s;
}
static int done(sqlite3_stmt *s)
{if(!s)return -1;int bad=sqlite3_step(s)!=SQLITE_DONE;sqlite3_finalize(s);return bad?-1:0;}
static int index_record(sqlite3 *db,const char *device,const char *capture,const char *manifest)
{
    struct context c;if(parse(manifest,&c))return 0;
    sqlite3_stmt *s=NULL;
    if(sqlite3_prepare_v2(db,"INSERT OR IGNORE INTO startup_context VALUES(?,?,?,?,?,?)",-1,&s,NULL)!=SQLITE_OK)return -1;
    sqlite3_bind_text(s,1,device,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,2,capture,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,3,c.recovery,-1,SQLITE_TRANSIENT);
    sqlite3_bind_int(s,4,c.current);sqlite3_bind_int64(s,5,(sqlite3_int64)c.generation);sqlite3_bind_int(s,6,c.slot);return done(s);
}
int sol_startup_evidence_schema(sqlite3 *db)
{
    const char *sql=
        "CREATE TABLE IF NOT EXISTS startup_context(device TEXT,capture TEXT,recovery TEXT,current INTEGER,generation INTEGER,slot INTEGER,PRIMARY KEY(device,capture),FOREIGN KEY(device,capture) REFERENCES startup_boots(device,boot) ON DELETE CASCADE);"
        "CREATE INDEX IF NOT EXISTS startup_recovery ON startup_context(device,recovery);"
        "CREATE INDEX IF NOT EXISTS evidence_recovery ON evidence_groups(device,observed_boot);"
        "CREATE TABLE IF NOT EXISTS evidence_startups(device TEXT,publication INTEGER,source TEXT,capture TEXT,role TEXT,recovery TEXT,received INTEGER,manifest TEXT,meta TEXT,PRIMARY KEY(device,publication,source,capture),FOREIGN KEY(device,publication,source) REFERENCES evidence_groups ON DELETE CASCADE);"
        "CREATE TABLE IF NOT EXISTS evidence_startup_streams(device TEXT,publication INTEGER,source TEXT,capture TEXT,stage TEXT,data BLOB,PRIMARY KEY(device,publication,source,capture,stage),FOREIGN KEY(device,publication,source,capture) REFERENCES evidence_startups ON DELETE CASCADE);"
        "CREATE VIEW IF NOT EXISTS evidence_startup_visible AS SELECT e.* FROM evidence_startups e WHERE NOT EXISTS(SELECT 1 FROM evidence_groups g WHERE g.device=e.device AND g.observed_boot=e.recovery AND g.phase IN('rolled-back','interrupted','device-error','reboot-failed','verify-failed') AND (g.publication!=e.publication OR g.source!=e.source));";
    if(sqlite3_exec(db,sql,NULL,NULL,NULL)!=SQLITE_OK)return -1;
    if(sqlite3_exec(db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK)return -1;
    sqlite3_stmt *s=NULL;
    if(sqlite3_prepare_v2(db,"SELECT device,boot,manifest FROM startup_boots b WHERE committed=1 AND NOT EXISTS(SELECT 1 FROM startup_context c WHERE c.device=b.device AND c.capture=b.boot)",-1,&s,NULL)!=SQLITE_OK){sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);return -1;}
    int step,error=0;
    while((step=sqlite3_step(s))==SQLITE_ROW)if(index_record(db,(const char *)sqlite3_column_text(s,0),(const char *)sqlite3_column_text(s,1),(const char *)sqlite3_column_text(s,2))){error=1;break;}
    sqlite3_finalize(s);
    if(error||step!=SQLITE_DONE)goto rollback;
    /* Backfill before the first SOL deletion can remove an old source archive. */
    if(sqlite3_prepare_v2(db,"SELECT device,publication,source FROM evidence_groups g WHERE phase IN('rolled-back','interrupted','device-error','reboot-failed','verify-failed') AND EXISTS(SELECT 1 FROM startup_context c WHERE c.device=g.device AND c.recovery=g.observed_boot) AND (SELECT count(*) FROM evidence_startups e WHERE e.device=g.device AND e.publication=g.publication AND e.source=g.source)<2",-1,&s,NULL)!=SQLITE_OK)goto rollback;
    while((step=sqlite3_step(s))==SQLITE_ROW){
        struct ota_report r={0};snprintf(r.device,sizeof(r.device),"%s",sqlite3_column_text(s,0));r.publication=(unsigned long long)sqlite3_column_int64(s,1);snprintf(r.boot,sizeof(r.boot),"%s",sqlite3_column_text(s,2));
        if(sol_startup_evidence_link(db,&r)){error=1;break;}
    }
    sqlite3_finalize(s);
    if(!error&&step==SQLITE_DONE&&sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)==SQLITE_OK)return 0;
rollback:sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);return -1;
}
static int validate_archive(sqlite3 *db,const struct ota_report *r,const char *capture)
{
    sqlite3_stmt *s=query(db,"SELECT manifest FROM startup_boots WHERE device=?1 AND boot=?4 AND committed=1",r);
    if(!s)return -1;
    sqlite3_bind_text(s,4,capture,-1,SQLITE_TRANSIENT);int step=sqlite3_step(s);
    char *manifest=step==SQLITE_ROW?strdup((const char *)sqlite3_column_text(s,0)):NULL;
    sqlite3_finalize(s);if(!manifest)return step==SQLITE_DONE?1:-1;
    char *p=strchr(manifest,'\n');int result=1;if(!p)goto finish;p++;
    for(int i=0;i<4;i++){
        unsigned length,crc,dropped,state;int used=0;char name[32];
        if(sscanf(p,"%31s %u %x %u %u\n%n",name,&length,&crc,&dropped,&state,&used)!=5||strcmp(name,stages[i])||state>3)goto finish;
        s=query(db,"SELECT data FROM startup_streams WHERE device=?1 AND boot=?4 AND stage=?5",r);
        if(!s){result=-1;goto finish;}
        sqlite3_bind_text(s,4,capture,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,5,stages[i],-1,SQLITE_STATIC);step=sqlite3_step(s);
        unsigned n=step==SQLITE_ROW?(unsigned)sqlite3_column_bytes(s,0):0;
        const void *data=step==SQLITE_ROW?sqlite3_column_blob(s,0):NULL;
        int bad=(step!=SQLITE_ROW&&step!=SQLITE_DONE)||n!=length||crc32(0,data,n)!=crc;
        sqlite3_finalize(s);if(bad)goto finish;p+=used;
    }
    result=0;
finish:free(manifest);return result;
}
int sol_startup_evidence_link(sqlite3 *db,const struct ota_report *r)
{
    sqlite3_stmt *existing=query(db,"SELECT count(*) FROM evidence_startups WHERE device=?1 AND publication=?2 AND source=?3",r);
    if(!existing)return -1;
    int existing_step=sqlite3_step(existing),saved=existing_step==SQLITE_ROW?sqlite3_column_int(existing,0):-1;
    sqlite3_finalize(existing);if(saved<0)return -1;if(saved>=2)return 0;
    char sql[2048];
    snprintf(sql,sizeof(sql),"SELECT observed_boot FROM evidence_groups g WHERE device=?1 AND publication=?2 AND source=?3 AND phase IN%s AND observed_boot!='unknown' AND observed_boot!=source AND (SELECT count(*) FROM evidence_groups x WHERE x.device=g.device AND x.observed_boot=g.observed_boot AND x.phase IN%s)=1",terminal,terminal);
    sqlite3_stmt *s=query(db,sql,r);if(!s)return -1;
    int step=sqlite3_step(s);char recovery[64]="";
    if(step==SQLITE_ROW)snprintf(recovery,sizeof(recovery),"%s",sqlite3_column_text(s,0));
    sqlite3_finalize(s);if(step==SQLITE_DONE)return 0;
    if(step!=SQLITE_ROW)return -1;
    /* A causal pair must contain exactly the preceding unconfirmed bank and
     * the current healthy bank with the same full recovery ID. */
    s=query(db,"SELECT capture,current,generation,slot FROM startup_context WHERE device=?1 AND recovery=?4 ORDER BY current",r);
    if(!s)return -1;
    sqlite3_bind_text(s,4,recovery,-1,SQLITE_TRANSIENT);
    char captures[2][64];long long generation[2];int slots[2],count=0,bad=0;
    while((step=sqlite3_step(s))==SQLITE_ROW){
        if(count>=2||sqlite3_column_int(s,1)!=count){bad=1;break;}
        snprintf(captures[count],sizeof(captures[count]),"%s",sqlite3_column_text(s,0));generation[count]=sqlite3_column_int64(s,2);slots[count]=sqlite3_column_int(s,3);count++;
    }
    sqlite3_finalize(s);if(step!=SQLITE_DONE&&!bad)return -1;
    if(bad||count!=2||generation[0]==LLONG_MAX||generation[1]!=generation[0]+1||slots[0]==slots[1])return 0;
    for(int i=0;i<2;i++){
        int integrity=validate_archive(db,r,captures[i]);if(integrity)return integrity<0?-1:0;
    }
    for(int i=0;i<2;i++){
        s=query(db,"INSERT OR IGNORE INTO evidence_startups SELECT device,?2,?3,boot,?5,?6,seen,manifest,meta FROM startup_boots WHERE device=?1 AND boot=?4 AND committed=1",r);
        if(!s)return -1;
        sqlite3_bind_text(s,4,captures[i],-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,5,i?"recovery-startup":"failed-startup",-1,SQLITE_STATIC);sqlite3_bind_text(s,6,recovery,-1,SQLITE_TRANSIENT);if(done(s))return -1;
        s=query(db,"INSERT OR IGNORE INTO evidence_startup_streams SELECT device,?2,?3,boot,stage,data FROM startup_streams WHERE device=?1 AND boot=?4",r);
        if(!s)return -1;
    sqlite3_bind_text(s,4,captures[i],-1,SQLITE_TRANSIENT);if(done(s))return -1;
    }
    return 0;
}
int sol_startup_evidence_commit(sqlite3 *db,const char *device,const char *capture,const char *manifest)
{
    if(index_record(db,device,capture,manifest))return -1;
    struct context c;if(parse(manifest,&c))return 0;
    sqlite3_stmt *s=NULL;
    if(sqlite3_prepare_v2(db,"SELECT publication,source FROM evidence_groups WHERE device=? AND observed_boot=?",-1,&s,NULL)!=SQLITE_OK)return -1;
    sqlite3_bind_text(s,1,device,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,2,c.recovery,-1,SQLITE_TRANSIENT);
    int step,error=0;
    while((step=sqlite3_step(s))==SQLITE_ROW){
        struct ota_report r={0};snprintf(r.device,sizeof(r.device),"%s",device);r.publication=(unsigned long long)sqlite3_column_int64(s,0);snprintf(r.boot,sizeof(r.boot),"%s",sqlite3_column_text(s,1));
        if(sol_startup_evidence_link(db,&r)){error=1;break;}
    }
    sqlite3_finalize(s);return error||step!=SQLITE_DONE?-1:0;
}
int sol_startup_evidence_reconcile(sqlite3 *db,const struct ota_report *r)
{
    if(sqlite3_exec(db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK)return -1;
    if(!sol_startup_evidence_link(db,r)&&sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)==SQLITE_OK)return 0;
    sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);return -1;
}
/* Each finite stage retains its original length/CRC/drop state. Completeness
 * concerns captured stages, not ROM/ATF or an inferred wall-clock crash time. */
static void integrity(FILE *out,const char *manifest)
{
    const char *p=strchr(manifest,'\n');int complete=1;char detail[1024]="";size_t used=0;
    if(!p)p="";else p++;
    for(int i=0;i<4;i++){
        unsigned len=0,crc=0,dropped=0,state=0;int n=0;char name[32];
        if(sscanf(p,"%31s %u %x %u %u\n%n",name,&len,&crc,&dropped,&state,&n)!=5||strcmp(name,stages[i])){complete=0;n=0;}
        if(state!=1||dropped||!len)complete=0;
        int k=snprintf(detail+used,sizeof(detail)-used,"%s{\"stage\":\"%s\",\"bytes\":%u,\"crc32\":\"%08x\",\"dropped\":%u,\"status\":%u}",i?",":"",stages[i],len,crc,dropped,state);
        if(k<0||(size_t)k>=sizeof(detail)-used){complete=0;break;}used+=(size_t)k;p+=n;
    }
    fprintf(out,",\"complete\":%s,\"stages\":[%s]",complete?"true":"false",detail);
}
int sol_startup_evidence_summary(sqlite3 *db,const struct ota_report *r,long long *bytes)
{
    sqlite3_stmt *s=query(db,"SELECT count(DISTINCT e.capture),COALESCE(sum(length(v.data)),0) FROM evidence_startup_visible e LEFT JOIN evidence_startup_streams v USING(device,publication,source,capture) WHERE e.device=?1 AND e.publication=?2 AND e.source=?3",r);
    if(!s)return -1;
    int step=sqlite3_step(s),count=-1;
    if(step==SQLITE_ROW){count=sqlite3_column_int(s,0);*bytes=sqlite3_column_int64(s,1);}
    sqlite3_finalize(s);return count;
}
int sol_startup_evidence_metadata(sqlite3 *db,const struct ota_report *r,FILE *out)
{
    sqlite3_stmt *s=query(db,"SELECT e.capture,e.role,e.recovery,e.received,e.manifest,COALESCE(sum(length(v.data)),0) FROM evidence_startup_visible e LEFT JOIN evidence_startup_streams v USING(device,publication,source,capture) WHERE e.device=?1 AND e.publication=?2 AND e.source=?3 GROUP BY e.capture ORDER BY e.role",r);
    if(!s)return -1;
    int step,count=0;
    fputs(",\"startup_archives\":[",out);
    while((step=sqlite3_step(s))==SQLITE_ROW){
        if(count++)fputc(',',out);
        fprintf(out,"{\"capture\":\"%s\",\"role\":\"%s\",\"recovery_boot\":\"%s\",\"received_at\":%lld,\"bytes\":%lld,\"association\":\"recovery-context\"",sqlite3_column_text(s,0),sqlite3_column_text(s,1),sqlite3_column_text(s,2),sqlite3_column_int64(s,3),sqlite3_column_int64(s,5));
        integrity(out,(const char *)sqlite3_column_text(s,4));fputc('}',out);
    }
    fputc(']',out);sqlite3_finalize(s);return step==SQLITE_DONE?count:-1;
}
int sol_startup_evidence_chunks(sqlite3 *db,const struct ota_report *r,const char *capture,unsigned long long cursor,char **out)
{
    sqlite3_stmt *s=query(db,"SELECT 1 FROM evidence_startup_visible WHERE device=?1 AND publication=?2 AND source=?3 AND capture=?4",r);
    if(!s)return -1;
    sqlite3_bind_text(s,4,capture,-1,SQLITE_TRANSIENT);int step=sqlite3_step(s);sqlite3_finalize(s);
    if(step==SQLITE_DONE)return 0;
    if(step!=SQLITE_ROW)return -1;
    s=query(db,"SELECT CASE stage WHEN 'r5-spl' THEN 1 WHEN 'a53-spl' THEN 2 WHEN 'uboot' THEN 3 ELSE 4 END idx,stage,data FROM evidence_startup_streams WHERE device=?1 AND publication=?2 AND source=?3 AND capture=?4 AND CASE stage WHEN 'r5-spl' THEN 1 WHEN 'a53-spl' THEN 2 WHEN 'uboot' THEN 3 ELSE 4 END>?5 ORDER BY idx",r);
    if(!s)return -1;
    sqlite3_bind_text(s,4,capture,-1,SQLITE_TRANSIENT);sqlite3_bind_int64(s,5,(sqlite3_int64)cursor);
    size_t size=0;FILE *f=open_memstream(out,&size);if(!f){sqlite3_finalize(s);return -1;}fputc('[',f);int count=0;
    while((step=sqlite3_step(s))==SQLITE_ROW){
        if(count++)fputc(',',f);
        fprintf(f,"{\"raw\":true,\"id\":%lld,\"stage\":\"%s\",\"text\":\"",sqlite3_column_int64(s,0),sqlite3_column_text(s,1));
        const unsigned char *data=sqlite3_column_blob(s,2);int n=sqlite3_column_bytes(s,2);
        for(int i=0;i<n;i++){unsigned c=data[i];if(c=='"'||c=='\\')fprintf(f,"\\%c",c);else if(c<32||c>=127)fprintf(f,"\\u%04x",c);else fputc((int)c,f);}
        fputs("\"}",f);
    }
    fputc(']',f);sqlite3_finalize(s);int bad=fclose(f)||step!=SQLITE_DONE;
    if(bad){free(*out);*out=NULL;return -1;}return 1;
}
