// SPDX-License-Identifier: GPL-2.0+
#define _POSIX_C_SOURCE 200809L
#include "dm/transfer-server.h"
#include "private.h"
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int64_t monotonic_ms(void)
{
    struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return -1;
    return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static int decimal(const char *p,uint64_t *value,const char **end)
{
    uint64_t n=0;if(*p<'0'||*p>'9')return -EINVAL;
    while(*p>='0'&&*p<='9'){
        unsigned digit=(unsigned)(*p++-'0');if(n>(UINT64_MAX-digit)/10)return -EOVERFLOW;
        n=n*10+digit;
    }
    *value=n;*end=p;return 0;
}
static int wait_read(int fd,int64_t deadline,const struct dm_transfer_callbacks *callbacks)
{
    for(;;){
        if(dm_transfer_cancelled(callbacks))return -ECANCELED;
        int64_t now=monotonic_ms();if(now<0)return -EIO;if(now>=deadline)return -ETIMEDOUT;
        int timeout=deadline-now>100?100:(int)(deadline-now);
        struct pollfd p={.fd=fd,.events=POLLIN};int rc=poll(&p,1,timeout);
        if(rc<0&&errno==EINTR)continue;
        if(rc<0)return -errno;
        if(rc&&(p.revents&(POLLIN|POLLHUP)))return 0;
        if(rc&&(p.revents&(POLLERR|POLLNVAL)))return -EIO;
    }
}
int dm_transfer_send_all(int fd,const void *data,size_t length)
{
    const unsigned char *p=data;
    while(length){
        ssize_t n=send(fd,p,length,MSG_NOSIGNAL);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)return n<0?-errno:-EPIPE;
        p+=n;length-=(size_t)n;
    }
    return 0;
}
int dm_transfer_receive_exact(int socket,void *data,size_t length,long timeout_ms,const struct dm_transfer_callbacks *callbacks)
{
    int64_t now=monotonic_ms();if(now<0||timeout_ms<=0)return -EINVAL;
    int64_t deadline=now+timeout_ms;unsigned char *p=data;size_t left=length;
    while(left){
        int rc=wait_read(socket,deadline,callbacks);if(rc)return rc;
        ssize_t n=recv(socket,p,left,MSG_DONTWAIT);
        if(n<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;
        if(n<=0)return n<0?-errno:-ECONNRESET;
        p+=n;left-=(size_t)n;
    }
    return 0;
}
int dm_transfer_receive_file(int socket,int fd,uint64_t length,uint64_t maximum,long timeout_ms,const struct dm_transfer_callbacks *callbacks)
{
    if(length>maximum||timeout_ms<=0)return -EFBIG;
    int64_t start=monotonic_ms();if(start<0)return -EIO;
    char buffer[65536];uint64_t received=0;
    while(received<length){
        int64_t now=monotonic_ms(),remaining=timeout_ms-(now-start);
        if(now<0||remaining<=0)return -ETIMEDOUT;
        size_t count=length-received<sizeof(buffer)?(size_t)(length-received):sizeof(buffer);
        int rc=dm_transfer_receive_exact(socket,buffer,count,(long)remaining,callbacks);if(rc)return rc;
        rc=dm_transfer_write_all(fd,buffer,count);if(rc)return rc;
        received+=count;
        if(callbacks&&callbacks->progress)callbacks->progress(callbacks->context,received,length);
    }
    return fsync(fd)?-errno:0;
}
int dm_transfer_http_headers(int socket,int status,const char *reason,const char *type,uint64_t length)
{
    char text[512];
    if(!reason||!type||strpbrk(reason,"\r\n")||strpbrk(type,"\r\n"))return -EINVAL;
    int n=snprintf(text,sizeof(text),"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %" PRIu64 "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",status,reason,type,length);
    return n<0||n>=(int)sizeof(text)?-EOVERFLOW:dm_transfer_send_all(socket,text,(size_t)n);
}
int dm_transfer_http_reply(int socket,int status,const char *reason,const char *type,const void *body,size_t length,int head)
{
    int rc=dm_transfer_http_headers(socket,status,reason,type,length);
    return rc||head?rc:dm_transfer_send_all(socket,body,length);
}
int dm_transfer_http_header(const struct dm_transfer_http_request *r,const char *name,const char **value)
{
    int found=0;*value=NULL;
    for(size_t i=0;i<r->header_count;i++)if(!strcasecmp(name,r->headers[i].name)){
        if(found)return -EPROTO;
        *value=r->headers[i].value;found=1;
    }
    return found?0:1;
}
int dm_transfer_http_read(int fd,struct dm_transfer_http_request *r)
{
    memset(r,0,sizeof(*r));strcpy(r->peer_ip,"unknown");
    struct timeval timeout={.tv_sec=15};
    (void)setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    (void)setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    int64_t start=monotonic_ms();if(start<0)return -EIO;
    size_t used=0;int complete=0;
    while(used<sizeof(r->storage)-1){
        int64_t now=monotonic_ms();if(now<0||now-start>=15000)return -ETIMEDOUT;
        int rc=dm_transfer_receive_exact(fd,r->storage+used,1,15000-(long)(now-start),NULL);if(rc)return rc;
        if(!r->storage[used])return 400;
        used++;r->storage[used]=0;
        if(used>=4&&!memcmp(r->storage+used-4,"\r\n\r\n",4)){complete=1;break;}
    }
    if(!complete)return 431;
    char extra,*line=strstr(r->storage,"\r\n");if(!line)return 400;
    *line=0;
    if(sscanf(r->storage,"%15s %2047s %15s %c",r->method,r->path,r->protocol,&extra)!=3||
       (strcmp(r->protocol,"HTTP/1.1")&&strcmp(r->protocol,"HTTP/1.0")))return 400;
    r->head=!strcmp(r->method,"HEAD");line+=2;
    while(*line){
        char *end=strstr(line,"\r\n");if(!end)return 400;*end=0;if(!*line)break;
        char *colon=strchr(line,':');if(!colon||colon==line||r->header_count>=64)return 400;*colon=0;
        if(strspn(line,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")!=strlen(line))return 400;
        char *value=colon+1;while(*value==' '||*value=='\t')value++;
        char *tail=value+strlen(value);while(tail>value&&(tail[-1]==' '||tail[-1]=='\t'))*--tail=0;
        for (const unsigned char *p=(const void *)value; *p; p++)
            if ((*p<32 && *p!='\t') || *p==127) return 400;
        r->headers[r->header_count++]=(struct dm_transfer_http_header){line,value};line=end+2;
    }
    const char *value,*end;
    int rc=dm_transfer_http_header(r,"Content-Length",&value);if(rc<0)return 400;
    if(!rc){if(decimal(value,&r->length,&end)||*end)return 400;r->has_length=1;}
    if(dm_transfer_http_header(r,"Transfer-Encoding",&value)!=1)return 400;
    rc=dm_transfer_http_header(r,"Expect",&value);if(rc<0||(!rc&&strcasecmp(value,"100-continue")))return 400;
    r->expect=!rc;
    struct sockaddr_storage peer;socklen_t size=sizeof(peer);
    if(!getpeername(fd,(struct sockaddr *)&peer,&size)){
        if(peer.ss_family==AF_INET)inet_ntop(AF_INET,&((struct sockaddr_in *)&peer)->sin_addr,r->peer_ip,sizeof(r->peer_ip));
        else if(peer.ss_family==AF_INET6)inet_ntop(AF_INET6,&((struct sockaddr_in6 *)&peer)->sin6_addr,r->peer_ip,sizeof(r->peer_ip));
    }
    return 0;
}
int dm_transfer_serve_file(const struct dm_transfer_serve *s)
{
    if(!s||s->size>INT64_MAX)return -EINVAL;
    uint64_t offset=0;
    if(s->range){
        const char *end;
        if(strncmp(s->range,"bytes=",6)||decimal(s->range+6,&offset,&end)||strcmp(end,"-")||offset>=s->size){
            char text[256];int n=snprintf(text,sizeof(text),"HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%" PRIu64 "\r\nContent-Length: 0\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",s->size);
            int rc=dm_transfer_send_all(s->socket,text,(size_t)n);return rc?rc:1;
        }
    }
    const char *type=s->content_type?s->content_type:"application/octet-stream";
    if(strpbrk(type,"\r\n")||(s->filename&&strpbrk(s->filename,"\r\n\"\\")))return -EINVAL;
    char extra[384]="",text[1024];size_t used=0;
    if(s->range){int n=snprintf(extra,sizeof(extra),"Content-Range: bytes %" PRIu64 "-%" PRIu64 "/%" PRIu64 "\r\n",offset,s->size-1,s->size);if(n<0||n>=(int)sizeof(extra))return -EOVERFLOW;used=(size_t)n;}
    if(s->filename){int n=snprintf(extra+used,sizeof(extra)-used,"Content-Disposition: attachment; filename=\"%s\"\r\n",s->filename);if(n<0||n>=(int)(sizeof(extra)-used))return -EOVERFLOW;}
    int n=snprintf(text,sizeof(text),"HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %" PRIu64 "\r\n%sAccept-Ranges: bytes\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",s->range?"206 Partial Content":"200 OK",type,s->size-offset,extra);
    if(n<0||n>=(int)sizeof(text))return -EOVERFLOW;
    int rc=dm_transfer_send_all(s->socket,text,(size_t)n);if(rc||s->head)return rc;
    char data[65536];uint64_t sent=offset;
    while(sent<s->size){
        if(dm_transfer_cancelled(&s->callbacks))return -ECANCELED;
        size_t count=s->size-sent<sizeof(data)?(size_t)(s->size-sent):sizeof(data);
        ssize_t got=pread(s->fd,data,count,(off_t)sent);if(got<0&&errno==EINTR)continue;
        if(got<=0)return got<0?-errno:-EIO;
        rc=dm_transfer_send_all(s->socket,data,(size_t)got);if(rc)return rc;
        sent+=(size_t)got;
        if(s->callbacks.progress)s->callbacks.progress(s->callbacks.context,sent,s->size);
    }
    return 0;
}
