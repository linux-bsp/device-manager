#ifndef DEVICE_MANAGER_SOL_ROUTE_H
#define DEVICE_MANAGER_SOL_ROUTE_H

#include "http.h"
#include "router.h"
#include <stdbool.h>

/* Returns true when the request belonged to the SOL API. */
bool sol_route_client(int client, const struct server_context *context,
                      const struct http_request *request);

#endif
