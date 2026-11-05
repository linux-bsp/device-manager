#ifndef SOL_BUNDLE_H
#define SOL_BUNDLE_H
#include <sqlite3.h>
#include "router.h"
#include "http.h"
int sol_bundle_cleanup(const struct server_context *context, const char *path);
int sol_bundle_schema(sqlite3 *db);
bool sol_bundle_route(int client, const struct server_context *context,
                      const struct http_request *request);
#endif
