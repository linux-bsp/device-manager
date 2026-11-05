#define _XOPEN_SOURCE 700
/* Immutable server receipt times, independent of device wall-clock and heartbeats. */
#include "sol-connections.h"
#include <stdio.h>
#include <sys/stat.h>

int sol_connections_schema(sqlite3 *db, const char *root)
{
    const char *schema =
        "CREATE TABLE IF NOT EXISTS sol_connections("
        "device TEXT,boot TEXT,first_seen INTEGER NOT NULL,source TEXT NOT NULL,PRIMARY KEY(device,boot));"
        "INSERT OR IGNORE INTO sol_connections SELECT device,boot,seen,'legacy-last-receipt' FROM boots;"
        "INSERT OR IGNORE INTO sol_connections SELECT device,boot,received,'archive-receipt' FROM bundles WHERE committed=1;";
    if (sqlite3_exec(db, schema, NULL, NULL, NULL) != SQLITE_OK) return -1;
    /* Older live rows only retained the last heartbeat. Use the oldest OTA
     * request for that same boot where available, and disclose its provenance. */
    char path[4096]; struct stat st;
    int n = snprintf(path, sizeof(path), "%s/firmware.db", root);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;
    if (lstat(path, &st) || !S_ISREG(st.st_mode)) return 0;
    sqlite3 *history = NULL;
    if (sqlite3_open_v2(path, &history, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        sqlite3_close(history); return 0;
    }
    sqlite3_stmt *read = NULL, *write = NULL;
    const char *query = "SELECT device,request_boot,MIN(first_seen) FROM device_requests GROUP BY device,request_boot";
    const char *update = "UPDATE sol_connections SET first_seen=MIN(first_seen,?3),source='ota-history' "
        "WHERE device=?1 AND boot=?2 AND source IN('legacy-last-receipt','archive-receipt')";
    int result = 0;
    if (sqlite3_prepare_v2(history, query, -1, &read, NULL) == SQLITE_OK) {
        if (sqlite3_prepare_v2(db, update, -1, &write, NULL) != SQLITE_OK) result = -1;
        while (!result && sqlite3_step(read) == SQLITE_ROW) {
            sqlite3_bind_text(write, 1, (const char *)sqlite3_column_text(read, 0), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(write, 2, (const char *)sqlite3_column_text(read, 1), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(write, 3, sqlite3_column_int64(read, 2));
            if (sqlite3_step(write) != SQLITE_DONE) result = -1;
            sqlite3_reset(write); sqlite3_clear_bindings(write);
        }
    }
    sqlite3_finalize(read); sqlite3_finalize(write); sqlite3_close(history);
    return result;
}
