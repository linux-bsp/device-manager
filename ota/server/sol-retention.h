#ifndef SOL_RETENTION_H
#define SOL_RETENTION_H
#include <sqlite3.h>
#define SOL_KEEP_BOOTS 16
#define SOL_KEEP_CHUNKS 16384
int sol_retention_schema(sqlite3 *db);
int sol_retention_apply(sqlite3 *db, const char *device, const char *boot,
                        const char *stage, long long generation);
int sol_retention_query(sqlite3 *db, const char *device, char **out);
#endif
