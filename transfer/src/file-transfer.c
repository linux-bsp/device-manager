// SPDX-License-Identifier: GPL-2.0+
#define _POSIX_C_SOURCE 200809L
#include "private.h"
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct download_context {
    const struct dm_transfer_download *options;
    struct dm_transfer_result *result;
    struct dm_transfer_checkpoint checkpoint;
    EVP_MD_CTX *hash;
    const char *state_path;
    int fd;
    uint64_t offset,base;
    struct timespec last_sync;
};
static int save(struct download_context *c)
{
    if(fdatasync(c->fd))return -errno;
    if(c->options->resume){
        c->checkpoint.offset=c->offset;
        int rc=dm_transfer_hash_finish(c->hash,c->checkpoint.prefix);if(rc)return rc;
        rc=dm_transfer_checkpoint_write(c->state_path,&c->checkpoint);if(rc)return rc;
    }
    c->result->durable_bytes=c->offset;
    clock_gettime(CLOCK_MONOTONIC,&c->last_sync);return 0;
}
static int sink(void *context,const void *data,size_t length)
{
    struct download_context *c=context;
    int rc=dm_transfer_write_all(c->fd,data,length);if(rc)return rc;
    if(!EVP_DigestUpdate(c->hash,data,length))return -EIO;
    c->offset+=length;
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    if(c->options->resume&&(c->offset-c->result->durable_bytes>=1024*1024||now.tv_sec-c->last_sync.tv_sec>=1))return save(c);
    return 0;
}
static int cancelled(void *context)
{
    struct download_context *c=context;return dm_transfer_cancelled(&c->options->request.callbacks);
}
static void progress(void *context,uint64_t done,uint64_t total)
{
    struct download_context *c=context;
    if(c->options->request.callbacks.progress)c->options->request.callbacks.progress(
        c->options->request.callbacks.context,c->base+done,c->options->size?c->options->size:c->base+total);
}
static int cached(const struct dm_transfer_download *d)
{
    if(!d->sha256||!d->size)return 0;
    int fd=open(d->path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return errno==ENOENT?0:-EIO;
    struct stat st;char hex[65];int rc=0;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode))rc=-EIO;
    else if(st.st_size>=0&&(uint64_t)st.st_size==d->size){
        rc=dm_transfer_sha256_fd(fd,d->size,hex,&d->request.callbacks);
        if(!rc)rc=!strcmp(hex,d->sha256)?1:0;
    }
    if(close(fd))rc=-EIO;
    return rc;
}
static int download_once(const struct dm_transfer_download *d,struct dm_transfer_result *result)
{
    char partial[DM_TRANSFER_PATH],state[DM_TRANSFER_PATH];
    if(snprintf(partial,sizeof(partial),"%s.part",d->path)>=(int)sizeof(partial)||
       snprintf(state,sizeof(state),"%s.part.state",d->path)>=(int)sizeof(state))return -ENAMETOOLONG;
    memset(result,0,sizeof(*result));
    int hit=cached(d);
    if(hit<0){result->kind=hit==-ECANCELED?DM_TRANSFER_CANCELLED:DM_TRANSFER_STORAGE;return hit;}
    if(hit){
        int rc=dm_transfer_sync_file(d->path);
        if(rc){result->kind=DM_TRANSFER_STORAGE;return -EIO;}
        if(access(partial,F_OK)&&errno==ENOENT){
            struct dm_transfer_checkpoint old;
            if(!dm_transfer_checkpoint_read(state,&old)&&!strcmp(old.sha256,d->sha256)&&old.size==d->size){
                if(unlink(state)||dm_transfer_sync_parent(state)){result->kind=DM_TRANSFER_STORAGE;return -EIO;}
            }
        }
        result->cached=1;result->durable_bytes=d->size;result->http_status=200;return 0;
    }
    struct download_context c={.options=d,.result=result,.state_path=state,.fd=-1};
    int rc=0,promoted=0;
    c.fd=open(partial,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);
    if(c.fd<0){result->kind=DM_TRANSFER_STORAGE;return -EIO;}
    struct stat st;
    if(fstat(c.fd,&st)||!S_ISREG(st.st_mode)||st.st_size<0||st.st_nlink!=1){rc=-EIO;goto done;}
    if(flock(c.fd,LOCK_EX|LOCK_NB)){rc=-errno;goto done;}
    c.hash=EVP_MD_CTX_new();if(!c.hash){rc=-ENOMEM;goto done;}
    if(d->resume){
        int loaded=dm_transfer_checkpoint_read(state,&c.checkpoint);
        if(loaded<0){rc=-EIO;goto done;}
        if(!loaded&&!strcmp(c.checkpoint.sha256,d->sha256)&&c.checkpoint.size==d->size){
            c.offset=c.checkpoint.offset;
            if((uint64_t)st.st_size<c.offset){rc=-EIO;goto done;}
            char hex[65];rc=dm_transfer_hash_prefix(c.fd,c.offset,c.hash,&d->request.callbacks);if(rc)goto done;
            rc=dm_transfer_hash_finish(c.hash,hex);if(rc)goto done;
            if(strcmp(hex,c.checkpoint.prefix)){rc=-EIO;goto done;}
        }else{
            memset(&c.checkpoint,0,sizeof(c.checkpoint));
            strcpy(c.checkpoint.sha256,d->sha256);c.checkpoint.size=d->size;
            rc=dm_transfer_hash_prefix(c.fd,0,c.hash,&d->request.callbacks);if(rc)goto done;
        }
    }else{rc=dm_transfer_hash_prefix(c.fd,0,c.hash,&d->request.callbacks);if(rc)goto done;}
    /* Discard only bytes after the last data+checkpoint durability boundary. */
    if(ftruncate(c.fd,(off_t)c.offset)||lseek(c.fd,(off_t)c.offset,SEEK_SET)<0){rc=-errno;goto done;}
    rc=save(&c);if(rc)goto done;
    result->resume_from=c.offset;
    if(d->request.callbacks.progress)d->request.callbacks.progress(d->request.callbacks.context,c.offset,d->size);
    int restarted=0;
again:
    c.base=c.offset;
    if(!d->size||c.offset<d->size){
        struct dm_transfer_request request=d->request;
        request.method="GET";request.output_fd=-1;request.sink=sink;request.sink_context=&c;
        request.response=NULL;request.response_capacity=0;request.output_limit=d->maximum-c.offset;
        request.ranged=c.offset>0;request.range_offset=c.offset;request.range_total=d->size;
        request.callbacks=(struct dm_transfer_callbacks){.cancelled=cancelled,.progress=progress,.context=&c};
        struct dm_transfer_result response;
        rc=dm_transfer_perform(&request,&response);
        result->kind=response.kind;result->http_status=response.http_status;result->backend_code=response.backend_code;
        result->received+=response.received;result->system_error=response.system_error;
        if(rc==-EPROTO&&request.ranged&&response.http_status==200&&!restarted){
            restarted=1;c.offset=0;result->resume_from=0;
            if(ftruncate(c.fd,0)||lseek(c.fd,0,SEEK_SET)<0){rc=-errno;goto done;}
            rc=dm_transfer_hash_prefix(c.fd,0,c.hash,&d->request.callbacks);if(!rc)rc=save(&c);if(rc)goto done;
            goto again;
        }
        if(!rc&&response.http_status!=200&&response.http_status!=206&&
           !(d->request.allow_tftp&&!strncmp(d->request.url,"tftp://",7)&&!response.http_status)){
            result->kind=DM_TRANSFER_HTTP;
            rc=response.http_status==404||response.http_status==204?-ENOENT:
               response.http_status==410?-ESTALE:-EAGAIN;
        }
        int sync_error=save(&c);
        if(sync_error){result->kind=DM_TRANSFER_STORAGE;rc=-EIO;goto done;}
        if(rc)goto done;
    }
    if(!c.offset||(d->size&&c.offset!=d->size)){result->kind=DM_TRANSFER_NETWORK;rc=-EAGAIN;goto done;}
    char hex[65];rc=dm_transfer_hash_finish(c.hash,hex);if(rc)goto done;
    if(d->sha256&&strcmp(hex,d->sha256)){
        result->kind=DM_TRANSFER_INTEGRITY;rc=-EBADMSG;
        if(unlink(partial)||(unlink(state)&&errno!=ENOENT)||dm_transfer_sync_parent(partial)){rc=-EIO;result->kind=DM_TRANSFER_STORAGE;}
        goto done;
    }
    rc=save(&c);if(rc)goto done;
    if(rename(partial,d->path)){rc=-errno;goto done;}
    promoted=1;
    rc=dm_transfer_sync_parent(d->path);if(rc)goto done;
    if(d->resume&&unlink(state)&&errno!=ENOENT){rc=-errno;goto done;}
    rc=dm_transfer_sync_parent(d->path);
    if(!rc)result->kind=DM_TRANSFER_OK;
done:
    if(rc&&result->kind==DM_TRANSFER_OK)result->kind=rc==-ECANCELED?DM_TRANSFER_CANCELLED:DM_TRANSFER_STORAGE;
    if(rc&&result->kind==DM_TRANSFER_STORAGE)rc=-EIO;
    if(c.hash)EVP_MD_CTX_free(c.hash);
    if(close(c.fd)&&!rc){rc=-EIO;result->kind=DM_TRANSFER_STORAGE;}
    if(rc&&!d->resume&&!promoted)unlink(partial);
    return rc;
}
int dm_transfer_download_file(const struct dm_transfer_download *d,struct dm_transfer_result *result)
{
    if(!result)return -EINVAL;
    memset(result,0,sizeof(*result));
    if(!d||!d->path||!d->request.url||!d->maximum||d->maximum>INT64_MAX||d->size>d->maximum||
       (d->sha256&&(strlen(d->sha256)!=64||strspn(d->sha256,"0123456789abcdef")!=64))||
       (d->resume&&(!d->sha256||!d->size))||d->attempts>16||d->retry_delay_ms>30000){
        result->kind=DM_TRANSFER_PROTOCOL;result->system_error=EINVAL;return -EINVAL;
    }
    unsigned attempts=d->attempts?d->attempts:1;
    int rc=-EAGAIN;
    for(unsigned i=0;i<attempts;i++){
        rc=download_once(d,result);
        if(!rc||i+1==attempts||dm_transfer_cancelled(&d->request.callbacks)||
           (result->kind!=DM_TRANSFER_NETWORK&&result->kind!=DM_TRANSFER_TIMEOUT&&
            !(result->kind==DM_TRANSFER_HTTP&&(result->http_status==429||result->http_status>=500))))break;
        unsigned left=d->retry_delay_ms?d->retry_delay_ms:250;
        while(left&&!dm_transfer_cancelled(&d->request.callbacks)){
            unsigned slice=left>100?100:left;struct timespec delay={.tv_nsec=(long)slice*1000000L};
            while(nanosleep(&delay,&delay)&&errno==EINTR){}
            left-=slice;
        }
    }
    return rc;
}
