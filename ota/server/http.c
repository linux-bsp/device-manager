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
    const char *cursor = data;
    while (length) {
        ssize_t count = send(client, cursor, length, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        cursor += count;
        length -= (size_t)count;
    }
    return 0;
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

static int decimal_length(const char *value, unsigned long long *length)
{
    char *end;
    if (!*value)
        return -1;
    for (const char *p = value; *p; p++)
        if (*p < '0' || *p > '9')
            return -1;
    errno = 0;
    *length = strtoull(value, &end, 10);
    return errno || *end ? -1 : 0;
}

static int set_once(const char **field, const char *value)
{
    if (*field)
        return -1;
    *field = value;
    return 0;
}

static void peer_address(int client, char address[46])
{
    struct sockaddr_storage peer;
    socklen_t size = sizeof(peer);
    strcpy(address, "unknown");
    if (getpeername(client, (struct sockaddr *)&peer, &size))
        return;
    if (peer.ss_family == AF_INET)
        (void)inet_ntop(AF_INET, &((struct sockaddr_in *)&peer)->sin_addr, address, 46);
    else if (peer.ss_family == AF_INET6)
        (void)inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&peer)->sin6_addr, address, 46);
}

int http_read_request(int client, struct http_request *request)
{
    char extra;
    size_t used = 0;
    bool complete = false;
    struct timeval timeout = {.tv_sec = 15, .tv_usec = 0};
    memset(request, 0, sizeof(*request));
    (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    while (used < sizeof(request->storage) - 1) {
        /* Leave binary upload bytes on the socket for the route handler. */
        ssize_t count = recv(client, request->storage + used, 1, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0 || request->storage[used] == '\0')
            return -1;
        used += (size_t)count;
        request->storage[used] = '\0';
        if (used >= 4 && !memcmp(request->storage + used - 4, "\r\n\r\n", 4)) {
            complete = true;
            break;
        }
    }
    if (!complete)
        return 431;
    char *line_end = strstr(request->storage, "\r\n");
    if (!line_end)
        return 400;
    *line_end = '\0';
    if (sscanf(request->storage, "%15s %2047s %15s %c", request->method,
               request->path, request->protocol, &extra) != 3 ||
        (strcmp(request->protocol, "HTTP/1.0") && strcmp(request->protocol, "HTTP/1.1")))
        return 400;
    request->head = !strcmp(request->method, "HEAD");
    char *line = line_end + 2;
    while (*line) {
        char *end = strstr(line, "\r\n");
        if (!end)
            return 400;
        *end = '\0';
        if (!*line)
            break;
        char *colon = strchr(line, ':');
        if (!colon)
            return 400;
        *colon = '\0';
        char *value = colon + 1;
        while (*value == ' ' || *value == '\t')
            value++;
        char *tail = value + strlen(value);
        while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t'))
            *--tail = '\0';
        if (!strcasecmp(line, "Content-Length")) {
            if (request->has_length || decimal_length(value, &request->length))
                return 400;
            request->has_length = true;
        } else if (!strcasecmp(line, "Authorization")) {
            if (set_once(&request->authorization, value)) return 400;
        } else if (!strcasecmp(line, "Transfer-Encoding")) {
            return 400; /* Chunked uploads are unsupported. */
        } else if (!strcasecmp(line, "X-OTA-Request")) {
            if (request->admin || strcmp(value, "1")) return 400;
            request->admin = true;
        } else if (!strcasecmp(line, "Origin")) {
            if (set_once(&request->origin, value)) return 400;
        } else if (!strcasecmp(line, "Host")) {
            if (set_once(&request->host, value)) return 400;
        } else if (!strcasecmp(line, "Content-Type")) {
            if (set_once(&request->type, value)) return 400;
        } else if (!strcasecmp(line, "X-OTA-Device")) {
            if (set_once(&request->device, value)) return 400;
        } else if (!strcasecmp(line, "X-OTA-HWID")) {
            if (set_once(&request->hwid, value)) return 400;
        } else if (!strcasecmp(line, "X-OTA-IP")) {
            if (set_once(&request->ip, value)) return 400;
        } else if (!strcasecmp(line, "X-OTA-MAC")) {
            if (set_once(&request->mac, value)) return 400;
        } else if (!strcasecmp(line, "X-OTA-Hostname")) {
            if (set_once(&request->hostname, value)) return 400;
        } else if (!strcasecmp(line, "X-OTA-Boot")) {
            if (set_once(&request->boot, value)) return 400;
        } else if (!strcasecmp(line, "Range")) {
            if (set_once(&request->range, value)) return 400;
        } else if (!strcasecmp(line, "Expect")) {
            if (request->expect || strcasecmp(value, "100-continue")) return 400;
            request->expect = true;
        }
        line = end + 2;
    }
    peer_address(client, request->peer_ip);
    return 0;
}
