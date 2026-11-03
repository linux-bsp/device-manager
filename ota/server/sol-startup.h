#ifndef SOL_STARTUP_H
#define SOL_STARTUP_H
#include <sqlite3.h>
#include <stddef.h>
int sol_startup_schema(sqlite3 *db);
int sol_startup_request(sqlite3 *db, const char *method, const char *path,
                        const char *peer, const char *body, size_t length, char **out);
#endif
