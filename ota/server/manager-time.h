#ifndef MANAGER_TIME_H
#define MANAGER_TIME_H
#include "http.h"
struct server_context;
bool manager_time_route(int client, const struct server_context *context, const struct http_request *request);
#endif
