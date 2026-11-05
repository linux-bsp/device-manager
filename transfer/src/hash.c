// SPDX-License-Identifier: GPL-2.0+
#define _POSIX_C_SOURCE 200809L
#include "private.h"
#include <errno.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <unistd.h>

int dm_transfer_hash_finish(EVP_MD_CTX *ctx, char hex[65])
{
    EVP_MD_CTX *copy=EVP_MD_CTX_new();unsigned char bytes[32];unsigned size;
    if(!copy)return -ENOMEM;
    int ok=EVP_MD_CTX_copy_ex(copy,ctx)&&EVP_DigestFinal_ex(copy,bytes,&size)&&size==32;
    EVP_MD_CTX_free(copy);if(!ok)return -EIO;
    for(unsigned i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",bytes[i]);
    return 0;
}
int dm_transfer_hash_prefix(int fd,uint64_t length,EVP_MD_CTX *ctx,const struct dm_transfer_callbacks *callbacks)
{
    unsigned char data[65536];uint64_t offset=0;
    if(!EVP_DigestInit_ex(ctx,EVP_sha256(),NULL))return -EIO;
    while(offset<length){
        if(dm_transfer_cancelled(callbacks))return -ECANCELED;
        size_t wanted=length-offset<sizeof(data)?(size_t)(length-offset):sizeof(data);
        ssize_t n=pread(fd,data,wanted,(off_t)offset);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)return n<0?-errno:-EIO;
        if(!EVP_DigestUpdate(ctx,data,(size_t)n))return -EIO;
        offset+=(size_t)n;
    }
    return 0;
}
int dm_transfer_sha256_fd(int fd,uint64_t length,char hex[65],const struct dm_transfer_callbacks *callbacks)
{
    if(length>INT64_MAX||!hex)return -EINVAL;
    EVP_MD_CTX *ctx=EVP_MD_CTX_new();if(!ctx)return -ENOMEM;
    int rc=dm_transfer_hash_prefix(fd,length,ctx,callbacks);if(!rc)rc=dm_transfer_hash_finish(ctx,hex);
    EVP_MD_CTX_free(ctx);return rc;
}
