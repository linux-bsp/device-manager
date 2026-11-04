/* Incident copies are independent of the bounded live SOL window. */
#define _GNU_SOURCE
#include "sol-evidence.h"
#include "sol-startup-evidence.h"
#include "ota-device.h"
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

int sol_evidence_schema(sqlite3 *db)
{
    const char *sql =
        "CREATE TABLE IF NOT EXISTS evidence_groups(device TEXT,publication INTEGER,source TEXT,"
        "mode TEXT NOT NULL,phase TEXT NOT NULL,reason TEXT NOT NULL,observed_boot TEXT NOT NULL,"
        "bytes INTEGER NOT NULL DEFAULT 0,chunks INTEGER NOT NULL DEFAULT 0,truncated INTEGER NOT NULL DEFAULT 0,"
        "PRIMARY KEY(device,publication,source));"
        "CREATE TABLE IF NOT EXISTS evidence_boots(device TEXT,publication INTEGER,source TEXT,boot TEXT,role TEXT,"
        "PRIMARY KEY(device,publication,source,boot),"
        "FOREIGN KEY(device,publication,source) REFERENCES evidence_groups ON DELETE CASCADE);"
        "CREATE INDEX IF NOT EXISTS evidence_boot_lookup ON evidence_boots(device,boot);"
        "CREATE TABLE IF NOT EXISTS evidence_chunks(id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "device TEXT,publication INTEGER,source TEXT,boot TEXT,source_id INTEGER,stage TEXT,gen INTEGER,off INTEGER,data TEXT,"
        "UNIQUE(device,publication,source,source_id),UNIQUE(device,publication,source,boot,stage,gen,off),"
        "FOREIGN KEY(device,publication,source,boot) REFERENCES evidence_boots ON DELETE CASCADE);"
        "CREATE INDEX IF NOT EXISTS evidence_progress ON evidence_chunks(device,publication,source,boot,source_id);"
        "CREATE TRIGGER IF NOT EXISTS evidence_budget BEFORE INSERT ON evidence_chunks "
        "WHEN NOT EXISTS(SELECT 1 FROM evidence_chunks WHERE device=NEW.device AND publication=NEW.publication AND source=NEW.source AND (source_id=NEW.source_id OR (boot=NEW.boot AND stage=NEW.stage AND gen=NEW.gen AND off=NEW.off))) "
        "AND ((SELECT bytes FROM evidence_groups WHERE device=NEW.device AND publication=NEW.publication AND source=NEW.source)+length(CAST(NEW.data AS BLOB))>33554432 OR (SELECT chunks FROM evidence_groups WHERE device=NEW.device AND publication=NEW.publication AND source=NEW.source)>=16384) "
        "BEGIN UPDATE evidence_groups SET truncated=1 WHERE device=NEW.device AND publication=NEW.publication AND source=NEW.source;SELECT RAISE(IGNORE);END;"
        "CREATE TRIGGER IF NOT EXISTS evidence_added AFTER INSERT ON evidence_chunks BEGIN "
        "UPDATE evidence_groups SET chunks=chunks+1,bytes=bytes+length(CAST(NEW.data AS BLOB)) WHERE device=NEW.device AND publication=NEW.publication AND source=NEW.source;END;"
        "CREATE TRIGGER IF NOT EXISTS evidence_removed AFTER DELETE ON evidence_chunks BEGIN "
        "UPDATE evidence_groups SET chunks=chunks-1,bytes=bytes-length(CAST(OLD.data AS BLOB)) WHERE device=OLD.device AND publication=OLD.publication AND source=OLD.source;END;";
    return sqlite3_exec(db,sql,NULL,NULL,NULL)==SQLITE_OK?0:-1;
}

static sqlite3_stmt *query(sqlite3 *db,const char *sql,const struct ota_report *r)
{
    sqlite3_stmt *s=NULL;
    if(sqlite3_prepare_v2(db,sql,-1,&s,NULL)!=SQLITE_OK)return NULL;
    sqlite3_bind_text(s,1,r->device,-1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(s,2,(sqlite3_int64)r->publication);
    sqlite3_bind_text(s,3,r->boot,-1,SQLITE_TRANSIENT);
    return s;
}
static int done(sqlite3_stmt *s)
{
    if(!s)return -1;
    int rc=sqlite3_step(s);sqlite3_finalize(s);return rc==SQLITE_DONE?0:-1;
}
static int boot_safe(const char *s)
{
    return s&&*s&&strlen(s)<=63&&strspn(s,"abcdefghijklmnopqrstuvwxyz0123456789-_")==strlen(s)&&strcmp(s,"unknown");
}
int sol_evidence_capture(sqlite3 *db,const char *device,const char *boot)
{
    sqlite3_stmt *s=NULL;
    const char *sql="INSERT OR IGNORE INTO evidence_chunks(device,publication,source,boot,source_id,stage,gen,off,data) "
        "SELECT b.device,b.publication,b.source,b.boot,c.id,c.stage,c.gen,c.off,c.data FROM evidence_boots b "
        "JOIN evidence_groups g USING(device,publication,source) JOIN chunks c ON c.device=b.device AND c.boot=b.boot "
        "WHERE b.device=?1 AND b.boot=?2 AND g.mode IN('candidate','logs') AND g.truncated=0 "
        "AND c.id>COALESCE((SELECT max(e.source_id) FROM evidence_chunks e WHERE e.device=b.device AND e.publication=b.publication AND e.source=b.source AND e.boot=b.boot),0) ORDER BY c.id";
    if(sqlite3_prepare_v2(db,sql,-1,&s,NULL)!=SQLITE_OK)return -1;
    sqlite3_bind_text(s,1,device,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,2,boot,-1,SQLITE_TRANSIENT);
    return done(s);
}

int sol_evidence_event(sqlite3 *db,const struct ota_report *r,const char *observed)
{
    int incident=ota_report_incident(r);
    int candidate=!strcmp(r->phase,"installing")||!strcmp(r->phase,"boot-testing");
    int explicit_reason_only=!strncmp(r->reason,"linux-",6)||!strncmp(r->reason,"data-",5);
    char saved_reason[64]="",saved_observer[64]="";
    if(!incident&&!candidate&&strcmp(r->phase,"success")&&strcmp(r->phase,"up-to-date"))return 0;
    if(sqlite3_exec(db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK)return -1;
    sqlite3_stmt *s=NULL;
    if(!incident&&!candidate){
        if(done(query(db,"DELETE FROM evidence_groups WHERE device=?1 AND publication=?2 AND source=?3 AND mode='candidate'",r)))goto fail;
        goto commit;
    }
    s=query(db,"INSERT OR IGNORE INTO evidence_groups(device,publication,source,mode,phase,reason,observed_boot) VALUES(?1,?2,?3,'candidate',?4,?5,?6)",r);
    if(!s)goto fail;
    sqlite3_bind_text(s,4,r->phase,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,5,r->reason,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(s,6,observed?observed:"unknown",-1,SQLITE_TRANSIENT);if(done(s))goto fail;
    s=query(db,"SELECT mode,reason,observed_boot FROM evidence_groups WHERE device=?1 AND publication=?2 AND source=?3",r);
    if(!s)goto fail;
    if(sqlite3_step(s)!=SQLITE_ROW){sqlite3_finalize(s);goto fail;}
    int accept=!strcmp((const char*)sqlite3_column_text(s,0),"candidate");
    const char *prior=(const char*)sqlite3_column_text(s,1);
    int saved_reason_only=!strncmp(prior,"data-",5)||!strncmp(prior,"linux-",6);
    if(saved_reason_only){snprintf(saved_reason,sizeof(saved_reason),"%s",prior);snprintf(saved_observer,sizeof(saved_observer),"%s",sqlite3_column_text(s,2));}
    sqlite3_finalize(s);
    if(candidate&&!accept)goto commit;
    if(candidate&&(explicit_reason_only||saved_reason_only)){
        if(done(query(db,"DELETE FROM evidence_boots WHERE device=?1 AND publication=?2 AND source=?3",r)))goto fail;
        if(explicit_reason_only){
            s=query(db,"UPDATE evidence_groups SET reason=?4,observed_boot=?5 WHERE device=?1 AND publication=?2 AND source=?3",r);
            if(!s)goto fail;
            sqlite3_bind_text(s,4,r->reason,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,5,observed?observed:"unknown",-1,SQLITE_TRANSIENT);
            if(done(s))goto fail;
        }
        goto commit;
    }
    if(incident&&saved_reason_only)explicit_reason_only=1;
    const char *role=!strcmp(r->phase,"boot-testing")?"target":"source";
    if(!explicit_reason_only&&boot_safe(observed)&&((!strcmp(role,"target")&&strcmp(observed,r->boot))||(!strcmp(role,"source")&&!strcmp(observed,r->boot)))){
        s=query(db,"INSERT OR IGNORE INTO evidence_boots VALUES(?1,?2,?3,?4,?5)",r);
        if(!s)goto fail;
        sqlite3_bind_text(s,4,observed,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,5,role,-1,SQLITE_TRANSIENT);
        if(done(s))goto fail;
    }
    if(!explicit_reason_only){
        s=query(db,"SELECT boot FROM evidence_boots WHERE device=?1 AND publication=?2 AND source=?3",r);
        if(!s)goto fail;
        int step;
        while((step=sqlite3_step(s))==SQLITE_ROW){
            if(sol_evidence_capture(db,r->device,(const char*)sqlite3_column_text(s,0))){sqlite3_finalize(s);goto fail;}
        }
        sqlite3_finalize(s);if(step!=SQLITE_DONE)goto fail;
    }
    if(incident){
        int reason_only=explicit_reason_only;
        s=query(db,"SELECT COALESCE(sum(role='target'),0),count(*) FROM evidence_boots WHERE device=?1 AND publication=?2 AND source=?3",r);
        if(!s)goto fail;
        int result=sqlite3_step(s),target=result==SQLITE_ROW?sqlite3_column_int(s,0):-1;
        int total=result==SQLITE_ROW?sqlite3_column_int(s,1):0;
        sqlite3_finalize(s);if(target<0)goto fail;
        if(!total||(!strcmp(r->phase,"rolled-back")&&!target))reason_only=1;
        s=query(db,"DELETE FROM evidence_boots WHERE device=?1 AND publication=?2 AND source=?3 AND (?4=1 OR role!=?5)",r);
        if(!s)goto fail;
        sqlite3_bind_int(s,4,reason_only);sqlite3_bind_text(s,5,target?"target":"source",-1,SQLITE_STATIC);if(done(s))goto fail;
        s=query(db,"UPDATE evidence_groups SET mode=?4,phase=?5,reason=?6,observed_boot=CASE WHEN ?7='unknown' THEN observed_boot ELSE ?7 END WHERE device=?1 AND publication=?2 AND source=?3",r);
        if(!s)goto fail;
        sqlite3_bind_text(s,4,reason_only?"reason-only":"logs",-1,SQLITE_STATIC);sqlite3_bind_text(s,5,r->phase,-1,SQLITE_TRANSIENT);
        const char *reason=saved_reason_only?saved_reason:(r->reason[0]?r->reason:"not-reported");
        const char *observer=saved_reason_only?saved_observer:(observed?observed:"unknown");
        sqlite3_bind_text(s,6,reason,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(s,7,observer,-1,SQLITE_TRANSIENT);if(done(s))goto fail;
    }
commit:
    if(incident&&sol_startup_evidence_link(db,r))goto fail;
    if(sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)==SQLITE_OK)return 0;
fail:
    sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);return -1;
}

static void bytes(FILE *out,const unsigned char *data,int n)
{
    fputc('"',out);
    for(int i=0;i<n;i++){
        unsigned c=data[i];
        if(c=='"'||c=='\\')fprintf(out,"\\%c",c);
        else if(c>=32&&c<127)fputc((int)c,out);
        else fprintf(out,"\\u%04x",c);
    }
    fputc('"',out);
}
int sol_evidence_query(sqlite3 *db,const char *path,char **out)
{
    struct ota_report r={0};char boot[64],extra,canonical[512];unsigned long long cursor=0;
    int chunks=sscanf(path,"/api/sol/incidents/%63[^/]/%llu/%63[^/]/%63[^/]/%llu%c",r.device,&r.publication,r.boot,boot,&cursor,&extra)==5;
    if(!chunks&&sscanf(path,"/api/sol/incidents/%63[^/]/%llu/%63s%c",r.device,&r.publication,r.boot,&extra)!=3)return 400;
    if(!ota_identifier(r.device)||!ota_identifier(r.boot)||!r.publication||r.publication>2147483647||(chunks&&!boot_safe(boot)))return 400;
    if(chunks&&cursor>LLONG_MAX)return 400;
    if(chunks)snprintf(canonical,sizeof(canonical),"/api/sol/incidents/%s/%llu/%s/%s/%llu",r.device,r.publication,r.boot,boot,cursor);
    else snprintf(canonical,sizeof(canonical),"/api/sol/incidents/%s/%llu/%s",r.device,r.publication,r.boot);
    if(strcmp(path,canonical))return 400;
    if(!chunks&&sol_startup_evidence_reconcile(db,&r))return 503;
    if(chunks){
        int archived=sol_startup_evidence_chunks(db,&r,boot,cursor,out);
        if(archived)return archived>0?200:503;
    }
    long long archive_bytes=0;
    int archived=sol_startup_evidence_summary(db,&r,&archive_bytes);if(archived<0)return 503;
    sqlite3_stmt *s=query(db,"SELECT mode,reason,phase,observed_boot,bytes,truncated FROM evidence_groups WHERE device=?1 AND publication=?2 AND source=?3",&r);
    if(!s)return 503;
    if(sqlite3_step(s)!=SQLITE_ROW){sqlite3_finalize(s);return 404;}
    const char *mode=(const char*)sqlite3_column_text(s,0);
    if(chunks&&strcmp(mode,"logs")){sqlite3_finalize(s);return 404;}
    size_t length;FILE *f=open_memstream(out,&length);if(!f){sqlite3_finalize(s);return 500;}
    if(!chunks){
        fprintf(f,"{\"mode\":\"%s\",\"reason\":\"%s\",\"phase\":\"%s\",\"observed_boot\":\"%s\",\"bytes\":%lld,\"truncated\":%s,\"complete\":false,\"boots\":[",archived?"logs":mode,sqlite3_column_text(s,1),sqlite3_column_text(s,2),sqlite3_column_text(s,3),(long long)sqlite3_column_int64(s,4)+archive_bytes,sqlite3_column_int(s,5)?"true":"false");
        sqlite3_finalize(s);
        s=query(db,"SELECT b.boot,b.role,COALESCE(SUM(length(CAST(c.data AS BLOB))),0) FROM evidence_boots b LEFT JOIN evidence_chunks c USING(device,publication,source,boot) WHERE b.device=?1 AND b.publication=?2 AND b.source=?3 GROUP BY b.boot,b.role ORDER BY b.boot",&r);
    }else{
        sqlite3_finalize(s);fputc('[',f);
        s=query(db,"SELECT id,stage,gen,off,data FROM evidence_chunks WHERE device=?1 AND publication=?2 AND source=?3 AND boot=?4 AND id>?5 ORDER BY id LIMIT 128",&r);
        if(s){sqlite3_bind_text(s,4,boot,-1,SQLITE_TRANSIENT);sqlite3_bind_int64(s,5,(sqlite3_int64)cursor);}
    }
    int n=0,rc=SQLITE_ERROR;
    if(s)while((rc=sqlite3_step(s))==SQLITE_ROW){
        if(n++)fputc(',',f);
        if(!chunks)fprintf(f,"{\"boot\":\"%s\",\"role\":\"%s\",\"bytes\":%lld}",sqlite3_column_text(s,0),sqlite3_column_text(s,1),(long long)sqlite3_column_int64(s,2));
        else{
            fprintf(f,"{\"id\":%lld,\"stage\":\"%s\",\"gen\":%lld,\"off\":%lld,\"text\":",(long long)sqlite3_column_int64(s,0),sqlite3_column_text(s,1),(long long)sqlite3_column_int64(s,2),(long long)sqlite3_column_int64(s,3));
            bytes(f,sqlite3_column_blob(s,4),sqlite3_column_bytes(s,4));fputc('}',f);
        }
    }
    sqlite3_finalize(s);fputc(']',f);
    int attachment_error=!chunks&&sol_startup_evidence_metadata(db,&r,f)<0;
    if(!chunks)fputc('}',f);
    int failed=fclose(f)||rc!=SQLITE_DONE||attachment_error;
    if(failed){free(*out);*out=NULL;return 503;}return 200;
}
