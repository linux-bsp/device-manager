/* Test-only libc interposition. No fault controls are linked into firmware. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int injected(int fd)
{
    const char *prefix=getenv("DM_TEST_SYNC_PATH"), *suffix=getenv("DM_TEST_SYNC_SUFFIX");
    const char *marker=getenv("DM_TEST_SYNC_MARKER"), *error=getenv("DM_TEST_SYNC_ERRNO");
    if(!prefix||!marker||access(marker,F_OK))return 0;
    char link[64],path[4096];
    snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
    ssize_t n=readlink(link,path,sizeof(path)-1);
    if(n<0)return 0;
    path[n]=0;
    if(strncmp(path,prefix,strlen(prefix)))return 0;
    size_t length=suffix?strlen(suffix):0;
    if(length&&((size_t)n<length||strcmp(path+n-length,suffix)))return 0;
    errno=error?atoi(error):EIO;
    return 1;
}
int fsync(int fd)
{
    if(injected(fd))return -1;
    int (*real_sync)(int)=dlsym(RTLD_NEXT,"fsync");
    return real_sync(fd);
}
int fdatasync(int fd)
{
    if(injected(fd))return -1;
    int (*real_sync)(int)=dlsym(RTLD_NEXT,"fdatasync");
    return real_sync(fd);
}
