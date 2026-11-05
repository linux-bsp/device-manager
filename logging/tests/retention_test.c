#define _GNU_SOURCE
#include "../retention.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static int sync_failure;
int __real_fsync(int fd);
int __wrap_fsync(int fd)
{
    if(sync_failure){errno=EIO;return -1;}
    return __real_fsync(fd);
}
int main(void)
{
    char directory[]="/tmp/log-retention-XXXXXX",path[256];
    assert(mkdtemp(directory));
    for(unsigned i=0;i<700;i++)
        assert(!log_retention_record(directory,"boot-test","stream-limit","kernel",i,4096));
    for(int old=0;old<2;old++) {
        snprintf(path,sizeof(path),"%s/.retention.log%s",directory,old?".1":"");
        struct stat st;assert(!stat(path,&st));assert(st.st_size>0&&st.st_size<=65536+512);
        FILE *f=fopen(path,"r");assert(f);char line[512];unsigned count=0;
        while(fgets(line,sizeof(line),f)){assert(line[0]=='{');count++;}
        assert(count);fclose(f);
    }
    sync_failure=1;
    assert(log_retention_record(directory,"boot-test","boot-limit","all",0,4096)==-1);
    assert(errno==EIO);sync_failure=0;
    for(int old=0;old<2;old++) {
        snprintf(path,sizeof(path),"%s/.retention.log%s",directory,old?".1":"");assert(!unlink(path));
    }
    assert(!rmdir(directory));
    puts("PASS bounded retention journal, rotation and failed durable acknowledgement");
    return 0;
}
