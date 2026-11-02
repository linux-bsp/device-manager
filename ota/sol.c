#define _GNU_SOURCE
#include "sol.h"
#include <sqlite3.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define CAP 4096
static sqlite3 *db;
static bool open_view;
void sol_set_open_view(bool enabled){open_view=enabled;}
static char dbroot[CAP];
static int identifier(const char *s)
{
    if (!*s || strlen(s)>63) return 0;
    for (;*s;s++) if (!((*s>='a'&&*s<='z')||(*s>='0'&&*s<='9')||*s=='-'||*s=='_')) return 0;
    return 1;
}
static int stage_valid(const char *s)
{
    const char *names[]={"r5-spl","a53-spl","uboot","initramfs","kernel","services","events"};
    for (size_t i=0;i<sizeof(names)/sizeof(*names);i++) if (!strcmp(s,names[i])) return 1;
    return 0;
}
static int key_path(char *p,const char *root,const char *device)
{
    return snprintf(p,CAP,"%s/sol-keys/%s",root,device)>=CAP?-1:0;
}
static int load_key(const char *path,char key[65])
{
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW); if(fd<0)return -1;
    struct stat st; char b[66];ssize_t n=read(fd,b,sizeof(b));
    int bad=fstat(fd,&st)||!S_ISREG(st.st_mode)||(st.st_mode&0077)||st.st_uid!=geteuid();close(fd);
    if(bad||n!=65||b[64]!='\n')return -1;
    for(int i=0;i<64;i++)if(!((b[i]>='0'&&b[i]<='9')||(b[i]>='a'&&b[i]<='f')))return -1;
    memcpy(key,b,64);key[64]=0;return 0;
}
static int create_key(const char *path)
{
    char key[65];if(!load_key(path,key))return 0;
    unsigned char random[32];int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);
    if(fd<0)return -1;
    ssize_t n=read(fd,random,sizeof(random));close(fd);if(n!=32)return -1;
    for(int i=0;i<32;i++)snprintf(key+2*i,3,"%02x",random[i]);
    key[64]='\n';fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return -1;
    int result=write(fd,key,65)==65&&fsync(fd)==0?0:-1;close(fd);return result;
}
static int setup(const char *root)
{
    char p[CAP];struct stat st;
    if(key_path(p,root,""))return -1;
    if(mkdir(p,0700)&&errno!=EEXIST)return -1;
    if(lstat(p,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&0077))return -1;
    if(key_path(p,root,"viewer")||create_key(p))return -1;
    if(db&&!strcmp(root,dbroot))return 0;
    if(db)sqlite3_close(db);
    db=NULL;
    if(snprintf(p,sizeof(p),"%s/sol.db",root)>=(int)sizeof(p))return -1;
    if(!lstat(p,&st)&&!S_ISREG(st.st_mode))return -1;
    if(sqlite3_open(p,&db)!=SQLITE_OK)return -1;
    chmod(p,0600);sqlite3_busy_timeout(db,1000);
    const char *sql="PRAGMA journal_mode=WAL;PRAGMA synchronous=FULL;PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS boots(device TEXT,boot TEXT,ord INTEGER,seen INTEGER,peer TEXT,meta TEXT,PRIMARY KEY(device,boot));"
        "CREATE TABLE IF NOT EXISTS streams(device TEXT,boot TEXT,stage TEXT,gen INTEGER,pos INTEGER,PRIMARY KEY(device,boot,stage,gen),FOREIGN KEY(device,boot) REFERENCES boots ON DELETE CASCADE);"
        "CREATE TABLE IF NOT EXISTS chunks(id INTEGER PRIMARY KEY AUTOINCREMENT,device TEXT,boot TEXT,stage TEXT,gen INTEGER,off INTEGER,data TEXT,FOREIGN KEY(device,boot,stage,gen) REFERENCES streams ON DELETE CASCADE);"
        "CREATE INDEX IF NOT EXISTS chunks_device ON chunks(device,id);";
    if(sqlite3_exec(db,sql,NULL,NULL,NULL)!=SQLITE_OK)return -1;
    snprintf(dbroot,sizeof(dbroot),"%s",root);return 0;
}
static int authorized(const char *root,const char *device,const char *auth)
{
    char p[CAP],key[65];
    if(!auth||strncmp(auth,"Bearer ",7)||strlen(auth+7)!=64||key_path(p,root,device)||load_key(p,key))return 0;
    unsigned diff=0;for(int i=0;i<64;i++)diff|=(unsigned char)key[i]^(unsigned char)auth[7+i];
    return !diff;
}
static sqlite3_stmt *statement(const char *sql,const char *device,const char *boot,const char *stage,long long gen)
{
    sqlite3_stmt *s=NULL;if(sqlite3_prepare_v2(db,sql,-1,&s,NULL)!=SQLITE_OK)return NULL;
    if(device)sqlite3_bind_text(s,1,device,-1,SQLITE_TRANSIENT);
    if(boot)sqlite3_bind_text(s,2,boot,-1,SQLITE_TRANSIENT);
    if(stage)sqlite3_bind_text(s,3,stage,-1,SQLITE_TRANSIENT);
    if(gen>=0)sqlite3_bind_int64(s,4,gen);
    return s;
}
static int finish(sqlite3_stmt *s)
{
    if(!s)return -1;
    int ret=sqlite3_step(s)==SQLITE_DONE?0:-1;sqlite3_finalize(s);return ret;
}
static void json_bytes(FILE *f,const char *s,size_t length)
{
    const unsigned char *end=(const unsigned char *)s+length;
    fputc('"',f);for(const unsigned char *p=(const unsigned char *)s;p<end;p++) {
        if(*p=='"'||*p=='\\')fprintf(f,"\\%c",*p);
        else if(*p<32||*p>=127)fprintf(f,"\\u%04x",*p);else fputc(*p,f);
    }fputc('"',f);
}
static void json_string(FILE *f,const char *s){json_bytes(f,s?s:"",s?strlen(s):0);}
static int reply(char **out,int status,const char *message)
{
    *out=strdup(message);return *out?status:500;
}
/* Strict canonical paths prevent alternate numeric encodings and query tricks. */
int sol_request(const char *root,const char *method,const char *path,const char *auth,
                const char *peer,const char *body,size_t length,char **out)
{
    *out=NULL;
    if(setup(root))return reply(out,503,"{\"error\":\"SOL storage unavailable\"}");
    char device[64],boot[64],stage[64],canonical[512],extra;
    long long order,gen,offset;
    if(!strncmp(path,"/api/sol/push/",14)) {
        if(strcmp(method,"POST")||sscanf(path,"/api/sol/push/%63[^/]/%63[^/]/%lld/%63[^/]/%lld/%lld%c",device,boot,&order,stage,&gen,&offset,&extra)!=6||
           !identifier(device)||!strcmp(device,"viewer")||!identifier(boot)||order<1||gen<0||offset<0||offset>4*1024*1024||length>4096)
            return reply(out,400,"{\"error\":\"Invalid log batch\"}");
        snprintf(canonical,sizeof(canonical),"/api/sol/push/%s/%s/%lld/%s/%lld/%lld",device,boot,order,stage,gen,offset);
        if(strcmp(path,canonical)||(!stage_valid(stage)&&strcmp(stage,"meta")))return reply(out,400,"{\"error\":\"Invalid stream\"}");
        if(!authorized(root,device,auth))return reply(out,403,"{\"error\":\"Device credential rejected\"}");
        if(sqlite3_exec(db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK)return reply(out,503,"{\"error\":\"SOL busy\"}");
        sqlite3_stmt *s=statement("INSERT INTO boots VALUES(?1,?2,?3,?4,?5,?6) ON CONFLICT(device,boot) DO UPDATE SET seen=excluded.seen,peer=excluded.peer,meta=CASE WHEN excluded.meta!='' THEN excluded.meta ELSE boots.meta END",device,boot,NULL,-1);
        if(!s)goto fail;
        sqlite3_bind_int64(s,3,order);sqlite3_bind_int64(s,4,time(NULL));sqlite3_bind_text(s,5,peer,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(s,6,!strcmp(stage,"meta")?body:"",!strcmp(stage,"meta")?(int)length:0,SQLITE_TRANSIENT);
        if(finish(s))goto fail;
        long long next=0;int status=200;
        if(strcmp(stage,"meta")) {
            s=statement("INSERT OR IGNORE INTO streams VALUES(?1,?2,?3,?4,0)",device,boot,stage,gen);
            if(finish(s))goto fail;
            s=statement("SELECT pos FROM streams WHERE device=?1 AND boot=?2 AND stage=?3 AND gen=?4",device,boot,stage,gen);
            if(!s)goto fail;
            if(sqlite3_step(s)!=SQLITE_ROW){sqlite3_finalize(s);goto fail;}
            next=sqlite3_column_int64(s,0);sqlite3_finalize(s);
            if(length&&offset==next) {
                s=statement("INSERT INTO chunks(device,boot,stage,gen,off,data) VALUES(?1,?2,?3,?4,?5,?6)",device,boot,stage,gen);
                if(!s)goto fail;
                sqlite3_bind_int64(s,5,offset);sqlite3_bind_text(s,6,body,(int)length,SQLITE_TRANSIENT);if(finish(s))goto fail;
                next+=length;
                s=statement("UPDATE streams SET pos=?5 WHERE device=?1 AND boot=?2 AND stage=?3 AND gen=?4",device,boot,stage,gen);
                if(!s)goto fail;
                sqlite3_bind_int64(s,5,next);if(finish(s))goto fail;
            }else if(length)status=409; /* Lost ACK or stale client: return durable cursor. */
        }
        s=statement("DELETE FROM boots WHERE device=?1 AND boot NOT IN(SELECT boot FROM boots WHERE device=?1 ORDER BY rowid DESC LIMIT 4)",device,NULL,NULL,-1);
        if(finish(s))goto fail;
        s=statement("DELETE FROM streams WHERE device=?1 AND boot=?2 AND stage=?3 AND gen NOT IN(SELECT gen FROM streams WHERE device=?1 AND boot=?2 AND stage=?3 ORDER BY gen DESC LIMIT 4)",device,boot,stage,-1);
        if(finish(s))goto fail;
        s=statement("DELETE FROM chunks WHERE device=?1 AND id NOT IN(SELECT id FROM chunks WHERE device=?1 ORDER BY id DESC LIMIT 2048)",device,NULL,NULL,-1);
        if(finish(s)||sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)!=SQLITE_OK)goto fail;
        char response[96];snprintf(response,sizeof(response),"{\"next\":%lld}",next);return reply(out,status,response);
fail:
        sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);return reply(out,503,"{\"error\":\"Log batch not committed\"}");
    }
    if(strcmp(method,"GET"))return reply(out,405,"{\"error\":\"Use GET\"}");
    if(!strcmp(path,"/api/sol/config"))return reply(out,200,open_view?"{\"open_view\":true}":"{\"open_view\":false}");
    if(!open_view&&!authorized(root,"viewer",auth))return reply(out,403,"{\"error\":\"Enter the SOL viewing credential\"}");
    sqlite3_stmt *s=NULL;
    if(!strcmp(path,"/api/sol/devices"))s=statement("SELECT device,boot,ord,seen,peer,meta FROM boots ORDER BY device,rowid DESC",NULL,NULL,NULL,-1);
    else {
        if(sscanf(path,"/api/sol/logs/%63[^/]/%63[^/]/%63[^/]/%lld%c",device,boot,stage,&offset,&extra)!=4||!identifier(device)||!identifier(boot)||(!stage_valid(stage)&&strcmp(stage,"all"))||offset<0)
            return reply(out,400,"{\"error\":\"Invalid log query\"}");
        snprintf(canonical,sizeof(canonical),"/api/sol/logs/%s/%s/%s/%lld",device,boot,stage,offset);
        if(strcmp(path,canonical))return reply(out,400,"{\"error\":\"Invalid log query\"}");
        s=statement("SELECT id,stage,data FROM chunks WHERE device=?1 AND boot=?2 AND (?3='all' OR stage=?3) AND id>?4 ORDER BY id LIMIT 128",device,boot,stage,offset);
    }
    if(!s)return reply(out,503,"{\"error\":\"Query unavailable\"}");
    size_t size=0;FILE *f=open_memstream(out,&size);if(!f){sqlite3_finalize(s);return 500;}
    fputs("[",f);int n=0,rc;
    while((rc=sqlite3_step(s))==SQLITE_ROW){
        if(n++)fputs(",",f);
        if(!strcmp(path,"/api/sol/devices")){
            fputs("{\"device\":",f);json_string(f,(const char*)sqlite3_column_text(s,0));
            fputs(",\"boot\":",f);json_string(f,(const char*)sqlite3_column_text(s,1));
            fprintf(f,",\"order\":%lld,\"seen\":%lld,\"peer\":",sqlite3_column_int64(s,2),sqlite3_column_int64(s,3));
            json_string(f,(const char*)sqlite3_column_text(s,4));fputs(",\"meta\":",f);json_string(f,(const char*)sqlite3_column_text(s,5));fputs("}",f);
        }else{
            fprintf(f,"{\"id\":%lld,\"stage\":",sqlite3_column_int64(s,0));json_string(f,(const char*)sqlite3_column_text(s,1));
            fputs(",\"text\":",f);json_bytes(f,(const char*)sqlite3_column_text(s,2),(size_t)sqlite3_column_bytes(s,2));fputs("}",f);
        }
    }
    sqlite3_finalize(s);fputs("]",f);fclose(f);if(rc!=SQLITE_DONE){free(*out);return reply(out,503,"{\"error\":\"Query failed\"}");}return 200;
}
int sol_enroll(int argc,char **argv)
{
    if(argc!=6||strcmp(argv[2],"--root")||strcmp(argv[4],"--device")||!identifier(argv[5])||!strcmp(argv[5],"viewer")) {
        fprintf(stderr,"Usage: device-server sol-enroll --root DIR --device ID\n");return 2;
    }
    char p[CAP];if(setup(argv[3])||key_path(p,argv[3],argv[5])||create_key(p)){perror("SOL credential");return 1;}
    printf("Device credential: %s\nViewing credential: %s/sol-keys/viewer\n",p,argv[3]);return 0;
}
