/* Delete uploaded artifacts; immutable publication snapshots remain available
 * for rollback, in-flight devices and historical audit downloads. */
#define _XOPEN_SOURCE 700
#include "ota-admin.h"
#include "ota-device.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
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
int ota_admin_delete(const char *root,const char *id)
{
    if (!valid_id(id)) return 400;
    int lock=ota_lock(root);if(lock<0)return 409;
    int result=503;
    char base[OTA_PATH],source[OTA_PATH];struct ota_catalog_identity identity;
    if(ota_path(base,root,"drafts")||ota_path(source,base,id))goto done;
    if(!directory(source)){result=404;goto done;}
    if(ota_catalog_get(root,source,&identity))goto done;
    int active=active_identity(root,&identity);
    if(active){result=active>0?409:503;goto done;}
    active=in_progress(root,&identity);
    if(active){result=active>0?409:503;goto done;}
    /* The UI groups duplicate uploads by content. Remove every matching upload
     * so deleting one row cannot expose an older duplicate of the same row. */
    DIR *dir=opendir(base);if(!dir)goto done;
    char names[256][13];size_t count=0;struct dirent *entry;
    while((entry=readdir(dir))) {
        if(!valid_id(entry->d_name))continue;
        struct ota_catalog_identity candidate;
        if(ota_path(source,base,entry->d_name)||!directory(source))continue;
        if(ota_catalog_get(root,source,&candidate)){closedir(dir);goto done;}
        if(strcmp(identity.hash,candidate.hash)||strcmp(identity.tag,candidate.tag))continue;
        if(count==256){closedir(dir);goto done;}
        memcpy(names[count++],entry->d_name,13);
    }
    closedir(dir);
    for(size_t i=0;i<count;i++) {
        char tombname[32],tomb[OTA_PATH],path[OTA_PATH];
        snprintf(tombname,sizeof(tombname),".deleted-%.12s",names[i]);
        if(ota_path(source,base,names[i])||ota_path(tomb,base,tombname)||
           ota_catalog_deletion(root,names[i],&identity,"requested"))goto done;
        /* Atomic visibility change; the retained manifest makes an interrupted
         * cleanup auditable. No recursive traversal of user-controlled paths. */
        if(rename(source,tomb)||ota_sync_dir(base))goto done;
        const char *files[]={"package.tar.gz","release.itb","kernel.itb","rootfs.itb","SHA256SUMS","published"};
        for(size_t n=0;n<sizeof(files)/sizeof(files[0]);n++) {
            if(ota_path(path,tomb,files[n]))goto done;
            if(unlink(path)&&errno!=ENOENT)goto done;
        }
        if(ota_sync_dir(tomb)||ota_catalog_deletion(root,names[i],&identity,"deleted"))goto done;
    }
    result=200;
done:
    close(lock);return result;
}
