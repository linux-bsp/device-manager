// SPDX-License-Identifier: GPL-2.0+
#define _POSIX_C_SOURCE 200809L
#include "private.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint32_t crc32(const void *data, size_t length)
{
    const unsigned char *p = data; uint32_t crc = ~0U;
    while (length--) {
        crc ^= *p++;
        for (unsigned bit=0; bit<8; bit++) crc = (crc>>1) ^ (0xedb88320U & (0U-(crc&1U)));
    }
    return ~crc;
}
static int encode(char out[256], const struct dm_transfer_checkpoint *c)
{
    if (strlen(c->sha256)!=64 || strspn(c->sha256,"0123456789abcdef")!=64 ||
        strlen(c->prefix)!=64 || strspn(c->prefix,"0123456789abcdef")!=64 ||
        !c->size || c->offset>c->size) return -EINVAL;
    int n = snprintf(out,256,"DMTRANSFER1 %s %" PRIu64 " %" PRIu64 " %s\n",c->sha256,c->size,c->offset,c->prefix);
    if (n<0 || n>230) return -EOVERFLOW;
    snprintf(out+n,256-(size_t)n,"crc32 %08x\n",crc32(out,(size_t)n));
    return 0;
}
int dm_transfer_checkpoint_read(const char *path, struct dm_transfer_checkpoint *c)
{
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)return errno==ENOENT?1:-errno;
    struct stat st;char text[256],canonical[256],extra;int rc=-EBADMSG;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<=0||st.st_size>=256)goto done;
    size_t used=0;
    while(used<(size_t)st.st_size){
        ssize_t n=read(fd,text+used,(size_t)st.st_size-used);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)goto done;
        used+=(size_t)n;
    }
    text[used]=0;if(memchr(text,0,used))goto done;
    char *line=strchr(text,'\n');if(!line)goto done;*line=0;
    memset(c,0,sizeof(*c));
    int count=sscanf(text,"DMTRANSFER1 %64s %" SCNu64 " %" SCNu64 " %64s %c",c->sha256,&c->size,&c->offset,c->prefix,&extra);
    *line='\n';
    if(count!=4||encode(canonical,c)||strcmp(text,canonical))goto done;
    rc=0;
done:
    if(close(fd)&&!rc)rc=-errno;
    return rc;
}
int dm_transfer_checkpoint_write(const char *path, const struct dm_transfer_checkpoint *c)
{
    char text[256],temp[DM_TRANSFER_PATH];int rc=encode(text,c);if(rc)return rc;
    if(snprintf(temp,sizeof(temp),"%s.XXXXXX",path)>=(int)sizeof(temp))return -ENAMETOOLONG;
    int fd=mkstemp(temp);if(fd<0)return -errno;
    (void)fcntl(fd,F_SETFD,FD_CLOEXEC);
    rc=dm_transfer_write_all(fd,text,strlen(text));
    if(!rc&&fsync(fd))rc=-errno;
    if(close(fd)&&!rc)rc=-errno;
    if(!rc&&rename(temp,path))rc=-errno;
    if(!rc)rc=dm_transfer_sync_parent(path);
    if(rc)unlink(temp);
    return rc;
}
