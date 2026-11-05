// SPDX-License-Identifier: GPL-2.0+
#ifndef DM_TRANSFER_SERVER_H
#define DM_TRANSFER_SERVER_H
#include <stddef.h>
#include <stdint.h>
#include "transfer.h"

struct dm_transfer_http_header { const char *name,*value; };
struct dm_transfer_http_request {
    char storage[8192],method[16],path[2048],protocol[16],peer_ip[46];
    uint64_t length;
    int head,has_length,expect;
    struct dm_transfer_http_header headers[64];
    size_t header_count;
};
/* 0 success; HTTP parse status 400/431; negative errno for transport errors. */
int dm_transfer_http_read(int socket,struct dm_transfer_http_request *request);
/* One nonblocking, <=2048-byte step, without consuming any body bytes.
 * 1 complete, 0 pending, 400/431 invalid header, negative errno on I/O.
 * Caller owns the total deadline and initializes used=0. */
int dm_transfer_http_header_step(int socket, char *header, size_t capacity, size_t *used);
/* Parse one exact header block (including CRLFCRLF); data must not overlap request. */
int dm_transfer_http_parse(int socket, struct dm_transfer_http_request *request,
                            const char *header, size_t length);
/* 0 found, 1 absent, -EPROTO ambiguous. Values live with the request. */
int dm_transfer_http_header(const struct dm_transfer_http_request *request,const char *name,const char **value);
int dm_transfer_send_all(int socket,const void *data,size_t length);
int dm_transfer_receive_exact(int socket,void *data,size_t length,long timeout_ms,
                                const struct dm_transfer_callbacks *callbacks);
int dm_transfer_receive_file(int socket,int fd,uint64_t length,uint64_t maximum,long timeout_ms,
                               const struct dm_transfer_callbacks *callbacks);
int dm_transfer_http_headers(int socket,int status,const char *reason,const char *type,uint64_t length);
int dm_transfer_http_reply(int socket,int status,const char *reason,const char *type,
                             const void *body,size_t length,int head);
struct dm_transfer_serve {
    int socket,fd,head;
    uint64_t size;
    const char *range,*content_type,*filename;
    struct dm_transfer_callbacks callbacks;
};
/* 0 body (or HEAD) sent, 1 invalid range replied with 416; negative on I/O. */
int dm_transfer_serve_file(const struct dm_transfer_serve *serve);
#endif
