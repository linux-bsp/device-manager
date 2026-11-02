#ifndef DEVICE_MANAGER_HTTP_H
#define DEVICE_MANAGER_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Header values point into storage and remain valid for the request lifetime. */
struct http_request {
    char storage[8192];
    char method[16];
    char path[2048];
    char protocol[16];
    char peer_ip[46];
    unsigned long long length;
    bool head, has_length, admin, expect;
    const char *origin, *host, *type, *device, *authorization;
    const char *ip, *mac, *hostname, *boot;
};

/* Returns 0, 400, 431, or -1 for a closed connection. */
int http_read_request(int client, struct http_request *request);
int http_send_all(int client, const void *data, size_t length);
int http_headers(int client, int status, const char *reason,
                 const char *type, uintmax_t length);
void http_text(int client, int status, const char *reason,
               const char *body, bool head);
void http_json(int client, int status, const char *reason,
               const char *body, bool head);

#endif
