#ifndef SOL_CONNECTIONS_H
#define SOL_CONNECTIONS_H
#include <sqlite3.h>
int sol_connections_schema(sqlite3 *db, const char *root);
#endif
