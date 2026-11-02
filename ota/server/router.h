#ifndef DEVICE_MANAGER_ROUTER_H
#define DEVICE_MANAGER_ROUTER_H

#include <signal.h>

struct server_context {
    const char *directory;
    int root;
    const volatile sig_atomic_t *running;
};

void server_route_client(int client, const struct server_context *context);

#endif
