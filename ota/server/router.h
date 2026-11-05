#ifndef DEVICE_MANAGER_ROUTER_H
#define DEVICE_MANAGER_ROUTER_H

#include <signal.h>

struct server_context {
    const char *directory;
    int root;
    const volatile sig_atomic_t *running;
};

struct http_request;
void server_route_request(int client, const struct server_context *context, struct http_request *request);

#endif
