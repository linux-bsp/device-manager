#define _POSIX_C_SOURCE 200809L
#include "http.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>

int http_send_all(int client, const void *data, size_t length)
{
    return dm_transfer_send_all(client, data, length);
}

int http_headers(int client, int status, const char *reason,
                 const char *type, uintmax_t length)
{
    char buffer[512];
    int count = snprintf(buffer, sizeof(buffer),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
        "Content-Length: %ju\r\nConnection: close\r\n"
        "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
        "Allow: GET, HEAD, POST, DELETE\r\n\r\n", status, reason, type, length);
    if (count < 0 || (size_t)count >= sizeof(buffer))
        return -1;
    return http_send_all(client, buffer, (size_t)count);
}

void http_text(int client, int status, const char *reason,
               const char *body, bool head)
{
    size_t length = strlen(body);
    if (!http_headers(client, status, reason, "text/plain; charset=utf-8", length) && !head)
        (void)http_send_all(client, body, length);
}

void http_json(int client, int status, const char *reason,
               const char *body, bool head)
{
    size_t length = strlen(body);
    if (!http_headers(client, status, reason, "application/json; charset=utf-8", length) && !head)
        (void)http_send_all(client, body, length);
}

static int request_fields(struct http_request *request)
{
    int rc;
    struct dm_transfer_http_request *r = &request->transport;
    memcpy(request->method, r->method, sizeof(request->method));
    memcpy(request->path, r->path, sizeof(request->path));
    memcpy(request->protocol, r->protocol, sizeof(request->protocol));
    memcpy(request->peer_ip, r->peer_ip, sizeof(request->peer_ip));
    request->length = r->length; request->head = r->head;
    request->has_length = r->has_length; request->expect = r->expect;
    struct { const char *name; const char **value; } fields[] = {
        {"Authorization", &request->authorization}, {"Origin", &request->origin},
        {"Host", &request->host}, {"Content-Type", &request->type}, {"Range", &request->range},
        {"X-OTA-Device", &request->device}, {"X-OTA-HWID", &request->hwid},
        {"X-OTA-IP", &request->ip}, {"X-OTA-MAC", &request->mac},
        {"X-OTA-Hostname", &request->hostname}, {"X-OTA-Boot", &request->boot}
    };
    for (size_t i = 0; i < sizeof(fields)/sizeof(fields[0]); i++)
        if (dm_transfer_http_header(r, fields[i].name, fields[i].value) < 0) return 400;
    const char *admin;
    rc = dm_transfer_http_header(r, "X-OTA-Request", &admin);
    if (rc < 0 || (!rc && strcmp(admin, "1"))) return 400;
    request->admin = !rc;
    return 0;
}

int http_read_request(int client, struct http_request *request)
{
    memset(request, 0, sizeof(*request));
    int rc = dm_transfer_http_read(client, &request->transport);
    return rc ? rc : request_fields(request);
}

int http_parse_request(int client, struct http_request *request, const char *header, size_t length)
{
    memset(request, 0, sizeof(*request));
    int rc = dm_transfer_http_parse(client, &request->transport, header, length);
    return rc ? rc : request_fields(request);
}
