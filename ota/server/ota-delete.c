/* Erase all payload copies of a firmware identity; retain only audit metadata. */
#define _XOPEN_SOURCE 700
#include "ota-admin.h"
#include "ota-device.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int valid_id(const char *id)
{
    if (!id || strlen(id)!=12 || strncmp(id,"draft-",6)) return 0;
    for (size_t i=6;i<12;i++) if (!isalnum((unsigned char)id[i])) return 0;
    return 1;
}
static int directory(const char *path)
{
    struct stat st;
    return !lstat(path,&st) && S_ISDIR(st.st_mode);
}
static int active_identity(const char *root,const struct ota_catalog_identity *identity)
{
    for (size_t pos=0;pos<strlen(identity->hwids);pos+=17) {
        char hwid[17],relative[64],path[OTA_PATH];struct release channel;
        struct ota_catalog_identity current;
        memcpy(hwid,identity->hwids+pos,16);hwid[16]=0;
        char channel_path[OTA_PATH];
        struct stat st;
        if (ota_channel_path(root, hwid, channel_path)) return -1;
        if (lstat(channel_path, &st)) {
            if (errno == ENOENT) continue;
            return -1;
        }
        if (!S_ISREG(st.st_mode) || ota_channel_read(root, hwid, &channel)) return -1;
        snprintf(relative,sizeof(relative),"releases/%llu",channel.version);
        if (ota_path(path,root,relative)||ota_catalog_get(root,path,&current)) return -1;
        if (!strcmp(current.hash,identity->hash)) return 1;
    }
    return 0;
}
static int in_progress(const char *root,const struct ota_catalog_identity *identity)
{
    char path[OTA_PATH];if(ota_path(path,root,"devices"))return -1;
    DIR *dir=opendir(path);if(!dir)return errno==ENOENT?0:-1;
    int result=0;struct dirent *entry;
    while((entry=readdir(dir))) {
        if(entry->d_name[0]=='.')continue;
        char text[OTA_REPORT_SIZE],relative[64];struct ota_report report;
        if(ota_device_read(root,entry->d_name,text)||ota_report_parse(text,&report)){result=-1;break;}
        if(strcmp(report.phase,"installing")&&strcmp(report.phase,"rebooting")&&strcmp(report.phase,"boot-testing"))continue;
        struct ota_catalog_identity target;
        snprintf(relative,sizeof(relative),"releases/%llu",report.publication);
        if(ota_path(path,root,relative)||ota_catalog_get(root,path,&target)){result=-1;break;}
        if(!strcmp(target.hash,identity->hash)){result=1;break;}
    }
    closedir(dir);return result;
}

static int release_id(const char *id)
{
    if (!id || !*id || *id=='0' || strlen(id)>10 || strspn(id,"0123456789")!=strlen(id)) return 0;
    return strtoull(id,NULL,10)<=2147483647;
}
static int erase_payload(const char *root,const char *directory)
{
    /* No recursive traversal and no backup payloads. The manifest is audit-only
     * and reserves publication numbers permanently against reuse. */
    const char *files[]={"package.tar.gz","release.itb","kernel.itb","rootfs.itb","SHA256SUMS","published"};
    for(size_t i=0;i<sizeof(files)/sizeof(files[0]);i++) {
        char path[OTA_PATH];if(ota_path(path,directory,files[i]))return -1;
        if(unlink(path)&&errno!=ENOENT)return -1;
    }
    return ota_sync_dir(directory)||ota_catalog_remove_done(root,directory)?-1:0;
}
/* Resume durable deletion intent after a crash. The publisher lock is also used
 * by manual deletion, fallback and publication; a second server may be running. */
int ota_admin_cleanup(const char *root)
{
    int lock=ota_lock(root);if(lock<0)return -1;
    int rc=0;
    for(int draft=0;draft<2 && !rc;draft++) {
        char base[OTA_PATH],path[OTA_PATH];
        if(ota_path(base,root,draft?"drafts":"releases")){rc=-1;break;}
        DIR *dir=opendir(base);if(!dir){if(errno!=ENOENT)rc=-1;continue;}
        struct dirent *entry;
        while((entry=readdir(dir))) {
            if(draft?!valid_id(entry->d_name):!release_id(entry->d_name))continue;
            if(ota_path(path,base,entry->d_name)||!directory(path)){rc=-1;break;}
            int removed=ota_catalog_removed(root,path,NULL);
            if(removed<0 || (removed && erase_payload(root,path))){rc=-1;break;}
        }
        closedir(dir);
    }
    close(lock);return rc;
}
int ota_admin_delete(const char *root,const char *id,const char *peer)
{
    int draft=valid_id(id);
    if(!draft && (strncmp(id,"release-",8)||!release_id(id+8)))return 400;
    int lock=ota_lock(root);if(lock<0)return 409;
    int result=503;size_t count=0;
    struct ota_removal *items=calloc(1024,sizeof(*items));
    char base[OTA_PATH],source[OTA_PATH];struct ota_catalog_identity identity;
    if(!items || ota_path(base,root,draft?"drafts":"releases")||ota_path(source,base,draft?id:id+8))goto done;
    if(!directory(source)){result=404;goto done;}
    if(ota_catalog_recorded(root,source,&identity))goto done;
    int active=active_identity(root,&identity);
    if(active){result=active>0?409:503;goto done;}
    active=in_progress(root,&identity);
    if(active){result=active>0?409:503;goto done;}
    /* Snapshot every matching draft and publication before changing any file.
     * This includes duplicate uploads, multi-HWID copies and fallback copies. */
    for(int kind=0;kind<2;kind++) {
        if(ota_path(base,root,kind?"releases":"drafts"))goto done;
        DIR *dir=opendir(base);if(!dir){if(errno==ENOENT)continue;goto done;}
        struct dirent *entry;int failed=0;
        while((entry=readdir(dir))) {
            if(kind?!release_id(entry->d_name):!valid_id(entry->d_name))continue;
            struct ota_catalog_identity candidate;struct stat st;
            if(ota_path(source,base,entry->d_name)||lstat(source,&st)||!S_ISDIR(st.st_mode)){failed=1;break;}
            if(ota_catalog_recorded(root,source,&candidate)){failed=1;break;}
            if(strcmp(identity.hash,candidate.hash)||strcmp(identity.tag,candidate.tag))continue;
            if(count==1024){failed=1;break;}
            strcpy(items[count].path,source);items[count].created=st.st_mtime;
            if(ota_catalog_removed(root,source,&items[count].created)<0){failed=1;break;}
            snprintf(items[count].id,sizeof(items[count].id),"%s%.12s",kind?"release-":"",entry->d_name);count++;
        }
        closedir(dir);if(failed)goto done;
    }
    if(!count || ota_catalog_remove_begin(root,items,count,&identity,peer))goto done;
    for(size_t i=0;i<count;i++)if(erase_payload(root,items[i].path))goto done;
    result=200;
done:
    free(items);close(lock);return result;
}
