#define _POSIX_C_SOURCE 200809L
#include "dm/transfer.h"
#include "dm/transfer-server.h"
#include "dm_fw_source_priv.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint64_t done, cancel_at;
static int cancelled(void *unused) { (void)unused; return cancel_at && done >= cancel_at; }
static void progress(void *unused, uint64_t value, uint64_t total)
{
    (void)unused; (void)total; done = value;
}
static uint64_t number(const char *s) { return strtoull(s, NULL, 10); }
struct cursor { uint64_t offset, size; int calls, rewind, bad, stuck; };
static int exchange(void *context, uint64_t offset, const void *data, size_t length, uint64_t *next)
{
    struct cursor *c = context;
    if (!c->calls++) { *next = c->offset; return 0; }
    if (offset != c->offset || !data || !length) return -EPROTO;
    if (c->bad) { *next = c->size+1; return 0; }
    if (c->stuck) { *next = offset; return 0; }
    if (c->rewind) { c->rewind = 0; c->offset = 0; }
    else c->offset += length;
    *next = c->offset; return 0;
}
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    int rc;
    if (!strcmp(argv[1], "source")) {
        struct fw_layout layout={0};struct fw_artifact artifact={0};char digest[65]="";
        snprintf(layout.work_dir,sizeof(layout.work_dir),"%s",argv[3]);
        rc=fw_source_get_release(&layout,argv[2],&artifact);
        if (!rc) {
            int fd=open(artifact.path,O_RDONLY);off_t size=lseek(fd,0,SEEK_END);
            rc=size<0?-errno:dm_transfer_sha256_fd(fd,(uint64_t)size,digest,NULL);close(fd);
        }
        fw_source_put(&artifact);
        printf("%d %s\n",rc,digest);return 0;
    }
    if (!strcmp(argv[1], "parse")) {
        struct dm_transfer_http_request r;
        rc = dm_transfer_http_read(atoi(argv[2]), &r);
        printf("%d %llu %d\n", rc, (unsigned long long)r.length, r.head); return 0;
    }
    if (!strcmp(argv[1], "receive")) {
        char buffer[8]; rc = dm_transfer_receive_exact(atoi(argv[2]), buffer, sizeof(buffer), 100, NULL);
        printf("%d\n", rc); return 0;
    }
    if (!strcmp(argv[1], "serve")) {
        struct dm_transfer_serve s = {.socket=atoi(argv[2]), .fd=open(argv[3], O_RDONLY),
            .size=number(argv[4]), .head=atoi(argv[5]), .range=strcmp(argv[6], "-")?argv[6]:NULL,
            .filename=argc>7?argv[7]:NULL};
        rc = dm_transfer_serve_file(&s); close(s.fd); printf("%d\n", rc); return 0;
    }
    if (!strcmp(argv[1], "push")) {
        struct cursor c = {.offset=number(argv[4]), .size=number(argv[3]),
            .rewind=atoi(argv[6]), .bad=atoi(argv[7]), .stuck=atoi(argv[8])};
        struct dm_transfer_push p = {.fd=open(argv[2], O_RDONLY), .size=c.size, .chunk_size=13,
            .max_chunks=(unsigned)atoi(argv[5]), .exchange=exchange, .context=&c};
        uint64_t next=0; rc=dm_transfer_push_file(&p, &next); close(p.fd);
        printf("%d %llu %d\n", rc, (unsigned long long)next, c.calls); return 0;
    }
    struct dm_transfer_request request;
    struct dm_transfer_result result = {0};
    dm_transfer_request_init(&request); request.url=argv[2];
    request.connect_timeout_ms=500; request.timeout_ms=3000;
    request.callbacks=(struct dm_transfer_callbacks){.cancelled=cancelled,.progress=progress};
    if (!strcmp(argv[1], "download")) {
        cancel_at=number(argv[7]);
        struct dm_transfer_download d = {.request=request,.path=argv[3],
            .sha256=strcmp(argv[4],"-")?argv[4]:NULL, .size=number(argv[5]),
            .maximum=number(argv[6]), .resume=atoi(argv[8]), .attempts=argc>9?(unsigned)atoi(argv[9]):1};
        rc=dm_transfer_download_file(&d, &result);
    } else {
        char response[128]; request.response=response; request.response_capacity=sizeof(response);
        if (!strcmp(argv[1],"upload")) {
            request.method="POST"; request.upload_fd=open(argv[3],O_RDONLY); request.upload_size=number(argv[4]);
        } else if (!strcmp(argv[1],"post")) { request.method="POST"; request.body="body"; request.body_size=4; }
        rc=dm_transfer_perform(&request,&result);
        if (request.upload_fd>=0) close(request.upload_fd);
    }
    printf("{\"rc\":%d,\"kind\":%d,\"status\":%ld,\"resume\":%llu,\"durable\":%llu,\"cached\":%d,\"received\":%llu}\n",
        rc,result.kind,result.http_status,(unsigned long long)result.resume_from,
        (unsigned long long)result.durable_bytes,result.cached,(unsigned long long)result.received);
    return 0;
}
