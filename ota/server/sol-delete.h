#ifndef DEVICE_SOL_DELETE_H
#define DEVICE_SOL_DELETE_H
#include <sqlite3.h>
#include <stddef.h>
int sol_delete_schema(sqlite3 *db);
int sol_delete_record(sqlite3 *db,const char *device,const char *boot,const char *peer,char **out);
int sol_deleted(sqlite3 *db,const char *device,const char *boot);
/* Returns 0 for an undeleted record, otherwise an HTTP status and cursor ACK. */
int sol_deleted_upload(sqlite3 *db,const char *device,const char *boot,const char *stage,long long gen,long long offset,size_t length,char **out);
#endif
