#define _XOPEN_SOURCE 700
#include "ota-admin.h"
#include "ota-device.h"
#include <arpa/inet.h>
#include <inttypes.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static sqlite3 *catalog;
static char catalog_path[OTA_PATH];

static int database(const char *root)
{
    char path[OTA_PATH];
    if (ota_path(path, root, "firmware.db")) return -1;
    if (catalog && !strcmp(path, catalog_path)) return 0;
    if (catalog) sqlite3_close(catalog);
    catalog = NULL;
    if (sqlite3_open_v2(path, &catalog, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK)
        goto fail;
    sqlite3_busy_timeout(catalog, 2000);
    const char *schema = "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;"
        "CREATE TABLE IF NOT EXISTS firmware_identity ("
        "path TEXT PRIMARY KEY, fingerprint TEXT NOT NULL, version INTEGER NOT NULL,"
        "tag TEXT NOT NULL, firmware_id TEXT NOT NULL, kernel_sha256 TEXT NOT NULL,"
        "rootfs_sha256 TEXT NOT NULL, kernel_bytes INTEGER NOT NULL, rootfs_bytes INTEGER NOT NULL, uploaded_at INTEGER NOT NULL, verified_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS firmware_downloads (id INTEGER PRIMARY KEY,"
        "publication INTEGER NOT NULL, device TEXT NOT NULL, completed_at INTEGER NOT NULL);"
        "CREATE INDEX IF NOT EXISTS downloads_publication ON firmware_downloads(publication);"
        "CREATE TABLE IF NOT EXISTS device_requests (id INTEGER PRIMARY KEY,device TEXT NOT NULL,"
        "publication INTEGER NOT NULL,transaction_boot TEXT NOT NULL,request_boot TEXT NOT NULL,phase TEXT NOT NULL,"
        "ip TEXT NOT NULL,mac TEXT NOT NULL,hostname TEXT NOT NULL,peer_ip TEXT NOT NULL,"
        "first_seen INTEGER NOT NULL,last_seen INTEGER NOT NULL,requests INTEGER NOT NULL,"
        "UNIQUE(device,publication,transaction_boot,request_boot,phase,ip,mac,hostname,peer_ip));"
        "CREATE INDEX IF NOT EXISTS requests_transaction ON device_requests(device,publication,transaction_boot);";
    if (sqlite3_exec(catalog, schema, NULL, NULL, NULL) != SQLITE_OK) goto fail;
    strcpy(catalog_path, path);
    return 0;
fail:
    if (catalog) sqlite3_close(catalog);
    catalog = NULL;
    return -1;
}

/* Detect replacement/deletion/modification cheaply. This is catalog freshness,
 * not a replacement for payload verification on publish/download/recovery. */
static int fingerprint(const char *directory, char result[1536])
{
    const char *files[] = {"manifest", "release.itb", "kernel.itb", "rootfs.itb"};
    size_t used = 0;
    for (unsigned i = 0; i < 4; i++) {
        struct stat st; char path[OTA_PATH];
        if (ota_path(path, directory, files[i]) || lstat(path, &st) || !S_ISREG(st.st_mode)) return -1;
        int n = snprintf(result + used, 1536 - used,
            "%ju:%ju:%jd:%jd:%ld:%jd:%ld;", (uintmax_t)st.st_dev,
            (uintmax_t)st.st_ino, (intmax_t)st.st_size, (intmax_t)st.st_mtim.tv_sec,
            st.st_mtim.tv_nsec, (intmax_t)st.st_ctim.tv_sec, st.st_ctim.tv_nsec);
        if (n < 0 || (size_t)n >= 1536 - used) return -1;
        used += (size_t)n;
    }
    return 0;
}
static int hash_string(const char *s)
{
    return s && strlen(s) == 64 && strspn(s, "0123456789abcdef") == 64;
}

int ota_catalog_get(const char *root, const char *directory, struct ota_catalog_identity *identity)
{
    char before[1536], after[1536], body[512], extra;
    if (database(root) || fingerprint(directory, before)) return -1;
    sqlite3_stmt *query = NULL;
    const char *sql = "SELECT version,tag,firmware_id,kernel_sha256,rootfs_sha256,uploaded_at,kernel_bytes,rootfs_bytes "
        "FROM firmware_identity WHERE path=?1 AND fingerprint=?2";
    if (sqlite3_prepare_v2(catalog, sql, -1, &query, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(query, 1, directory, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(query, 2, before, -1, SQLITE_TRANSIENT);
    int result = sqlite3_step(query), valid = 0;
    if (result == SQLITE_ROW) {
        sqlite3_int64 version = sqlite3_column_int64(query, 0);
        const char *tag = (const char *)sqlite3_column_text(query, 1);
        const char *hash = (const char *)sqlite3_column_text(query, 2);
        const char *kernel = (const char *)sqlite3_column_text(query, 3);
        const char *rootfs = (const char *)sqlite3_column_text(query, 4);
        char expected[40];
        snprintf(expected, sizeof(expected), "v%lu.%lu.%lu", (unsigned long)version >> 16,
            ((unsigned long)version >> 8) & 255, (unsigned long)version & 255);
        if (version > 0 && version <= UINT32_MAX && tag && !strcmp(tag, expected) &&
            hash_string(hash) && hash_string(kernel) && hash_string(rootfs)) {
            identity->version = (unsigned long)version;
            identity->uploaded_at = sqlite3_column_int64(query, 5);
            identity->kernel_bytes = (unsigned long long)sqlite3_column_int64(query, 6);
            identity->rootfs_bytes = (unsigned long long)sqlite3_column_int64(query, 7);
            strcpy(identity->tag, tag); strcpy(identity->hash, hash);
            strcpy(identity->kernel, kernel); strcpy(identity->rootfs, rootfs);
            valid = identity->kernel_bytes > 0 && identity->kernel_bytes <= OTA_MAX_FILE &&
                    identity->rootfs_bytes > 0 && identity->rootfs_bytes <= OTA_MAX_FILE;
        }
    }
    sqlite3_finalize(query);
    if (valid) return 0;
    if (result != SQLITE_DONE && result != SQLITE_ROW) return -1;
    unsigned long long publication, sizes[2]; unsigned version;
    if (ota_image_identity(directory, 1, body, sizeof(body)) ||
        ota_content_identity(body, identity->hash, &identity->version) ||
        sscanf(body, "BSPOTAIMAGE1 %llu %u %llu %64s %llu %64s %c", &publication,
            &version, &sizes[0], identity->kernel, &sizes[1], identity->rootfs, &extra) != 6 ||
        fingerprint(directory, after) || strcmp(before, after)) return -1;
    identity->kernel_bytes = sizes[0]; identity->rootfs_bytes = sizes[1];
    snprintf(identity->tag, sizeof(identity->tag), "v%lu.%lu.%lu", identity->version >> 16,
        (identity->version >> 8) & 255, identity->version & 255);
    char manifest[OTA_PATH]; struct stat st;
    if (ota_path(manifest, directory, "manifest") || stat(manifest, &st)) return -1;
    identity->uploaded_at = (long long)st.st_mtime;
    if (strstr(directory, "/releases/")) {
        const char *uploaded = "SELECT MIN(uploaded_at) FROM firmware_identity WHERE firmware_id=?1 AND path LIKE '%/drafts/%'";
        if (sqlite3_prepare_v2(catalog, uploaded, -1, &query, NULL) != SQLITE_OK) return -1;
        sqlite3_bind_text(query, 1, identity->hash, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(query) == SQLITE_ROW && sqlite3_column_type(query, 0) != SQLITE_NULL)
            identity->uploaded_at = sqlite3_column_int64(query, 0);
        sqlite3_finalize(query);
    }
    sql = "INSERT OR REPLACE INTO firmware_identity "
        "(path,fingerprint,version,tag,firmware_id,kernel_sha256,rootfs_sha256,uploaded_at,kernel_bytes,rootfs_bytes,verified_at) "
        "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,CAST(strftime('%s','now') AS INTEGER))";
    if (sqlite3_prepare_v2(catalog, sql, -1, &query, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(query, 1, directory, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(query, 2, before, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(query, 3, identity->version);
    sqlite3_bind_text(query, 4, identity->tag, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(query, 5, identity->hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(query, 6, identity->kernel, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(query, 7, identity->rootfs, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(query, 8, identity->uploaded_at);
    sqlite3_bind_int64(query, 9, (sqlite3_int64)identity->kernel_bytes);
    sqlite3_bind_int64(query, 10, (sqlite3_int64)identity->rootfs_bytes);
    result = sqlite3_step(query);
    sqlite3_finalize(query);
    return result == SQLITE_DONE ? 0 : -1;
}

int ota_catalog_download(const char *root, unsigned long long publication, const char *device)
{
    if (database(root)) return -1;
    sqlite3_stmt *query = NULL;
    const char *sql = "INSERT INTO firmware_downloads(publication,device,completed_at) "
        "VALUES(?1,?2,CAST(strftime('%s','now') AS INTEGER))";
    if (sqlite3_prepare_v2(catalog, sql, -1, &query, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(query, 1, (sqlite3_int64)publication);
    sqlite3_bind_text(query, 2, device ? device : "", -1, SQLITE_TRANSIENT);
    int result = sqlite3_step(query); sqlite3_finalize(query);
    return result == SQLITE_DONE ? 0 : -1;
}
int ota_catalog_downloads_json(const char *root, FILE *out)
{
    if (database(root)) return -1;
    sqlite3_stmt *query = NULL;
    const char *sql = "SELECT publication,COUNT(*),SUM(device!=''),COUNT(DISTINCT NULLIF(device,'')) "
        "FROM firmware_downloads GROUP BY publication";
    if (sqlite3_prepare_v2(catalog, sql, -1, &query, NULL) != SQLITE_OK) return -1;
    int result, count = 0;
    while ((result = sqlite3_step(query)) == SQLITE_ROW) {
        fprintf(out, "%s{\"publication\":%lld,\"downloads\":%lld,\"identified_downloads\":%lld,\"unique_devices\":%lld}",
            count++ ? "," : "", (long long)sqlite3_column_int64(query,0),
            (long long)sqlite3_column_int64(query,1), (long long)sqlite3_column_int64(query,2),
            (long long)sqlite3_column_int64(query,3));
    }
    sqlite3_finalize(query);
    return result == SQLITE_DONE && !ferror(out) ? 0 : -1;
}

/* All interpolated JSON values below are restricted to safe ASCII tokens. */
int ota_network_valid(const struct ota_network_context *n)
{
    unsigned char address[16];
    if (!n->ip || !n->mac || !n->hostname || !n->boot) return 0;
    if (strcmp(n->ip, "unknown") && inet_pton(AF_INET, n->ip, address) != 1 &&
        inet_pton(AF_INET6, n->ip, address) != 1) return 0;
    if (strcmp(n->mac, "unknown")) {
        if (strlen(n->mac) != 17) return 0;
        for (int i = 0; i < 17; i++)
            if (i % 3 == 2 ? n->mac[i] != ':' : !strchr("0123456789abcdefABCDEF", n->mac[i])) return 0;
    }
    size_t length = strlen(n->hostname);
    return length > 0 && length <= 253 &&
        strspn(n->hostname, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == length &&
        ota_identifier(n->boot);
}
int ota_catalog_request(const char *root, const char *device, unsigned long long publication,
                        const char *transaction, const char *phase, const struct ota_network_context *n)
{
    if (!n->ip) return 0; /* Historical clients never supplied these facts. */
    if (!ota_network_valid(n) || !ota_identifier(device) || !ota_identifier(transaction) || !ota_identifier(phase) || database(root)) return -1;
    sqlite3_stmt *query = NULL;
    const char *sql = "INSERT INTO device_requests(device,publication,transaction_boot,request_boot,phase,ip,mac,hostname,peer_ip,first_seen,last_seen,requests) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,unixepoch(),unixepoch(),1) "
        "ON CONFLICT(device,publication,transaction_boot,request_boot,phase,ip,mac,hostname,peer_ip) "
        "DO UPDATE SET last_seen=unixepoch(),requests=requests+1";
    if (sqlite3_prepare_v2(catalog, sql, -1, &query, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(query, 1, device, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(query, 2, (sqlite3_int64)publication);
    const char *values[] = {transaction, n->boot, phase, n->ip, n->mac, n->hostname, n->peer_ip};
    for (int i = 0; i < 7; i++) sqlite3_bind_text(query, i + 3, values[i], -1, SQLITE_TRANSIENT);
    int result = sqlite3_step(query); sqlite3_finalize(query);
    return result == SQLITE_DONE ? 0 : -1;
}
int ota_catalog_network_json(const char *root, FILE *out, const struct ota_report *r)
{
    if (database(root)) return -1;
    sqlite3_stmt *query = NULL;
    const char *sql = "SELECT ip,mac,hostname,peer_ip,request_boot,phase,first_seen,last_seen,requests "
        "FROM device_requests WHERE device=?1 AND publication=?2 AND transaction_boot=?3 ORDER BY id";
    if (sqlite3_prepare_v2(catalog, sql, -1, &query, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(query, 1, r->device, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(query, 2, (sqlite3_int64)r->publication);
    sqlite3_bind_text(query, 3, r->boot, -1, SQLITE_TRANSIENT);
    int result, count = 0;
    while ((result = sqlite3_step(query)) == SQLITE_ROW) {
        fprintf(out, "%s{\"ip\":\"%s\",\"mac\":\"%s\",\"hostname\":\"%s\",\"peer_ip\":\"%s\","
            "\"request_boot\":\"%s\",\"phase\":\"%s\",\"first_seen\":%lld,\"last_seen\":%lld,\"requests\":%lld}",
            count++ ? "," : "", sqlite3_column_text(query,0), sqlite3_column_text(query,1),
            sqlite3_column_text(query,2), sqlite3_column_text(query,3), sqlite3_column_text(query,4), sqlite3_column_text(query,5),
            (long long)sqlite3_column_int64(query,6), (long long)sqlite3_column_int64(query,7), (long long)sqlite3_column_int64(query,8));
    }
    sqlite3_finalize(query);
    return result == SQLITE_DONE && !ferror(out) ? 0 : -1;
}
