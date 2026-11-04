#ifndef DM_CLOCK_CONTROL_H
#define DM_CLOCK_CONTROL_H
#include "http.h"
#include "router.h"
bool clock_control_route(int client,const struct server_context *context,const struct http_request *request);
#endif
