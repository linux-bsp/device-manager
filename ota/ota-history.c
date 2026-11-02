#define _POSIX_C_SOURCE 200809L
#include "ota-device.h"
#include "ota-admin.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* One atomic journal per device/publication/source boot. Latest-device state
 * remains the agent's recovery API; history must be durable before its ACK. */
struct history {
    struct ota_report last;
    char text[OTA_REPORT_SIZE];
    long long first, updated;
    unsigned count, imported;
    struct ota_report incident;
};

static int same_transaction(const struct ota_report *a, const struct ota_report *b)
{
    return !strcmp(a->device, b->device) && !strcmp(a->boot, b->boot) &&
        a->publication == b->publication && a->version == b->version &&
        a->target == b->target && !strcmp(a->product, b->product) &&
        !strcmp(a->layout, b->layout);
}

/* Copy validated lines, optionally emit events. Never silently drop corruption. */
static int read_history(const char *path, struct history *h, FILE *copy, FILE *events)
{
    memset(h, 0, sizeof(*h));
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? 1 : -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return -1; }
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return -1; }
    char line[OTA_REPORT_SIZE + 64];
    int rc = 0;
    while (fgets(line, sizeof(line), f)) {
        long long when; unsigned imported; int offset = 0;
        struct ota_report r;
        if (sscanf(line, "%lld %u %n", &when, &imported, &offset) != 2 ||
            when < 0 || imported > 1 || !offset ||
            ota_report_parse(line + offset, &r) ||
            (h->count && (!same_transaction(&h->last, &r) || imported))) {
            rc = -1; break;
        }
        if (!h->count) { h->first = when; h->imported = imported; }
        if (copy && fputs(line, copy) == EOF) { rc = -1; break; }
        if (events) fprintf(events, "%s{\"phase\":\"%s\",\"running\":%u,\"reported_at\":%lld,\"category\":\"%s\",\"reason\":\"%s\"}",
            h->count ? "," : "", r.phase, r.running, when, ota_report_category(&r), r.reason[0] ? r.reason : "not-reported");
        if (ota_report_incident(&r)) h->incident = r;
        h->last = r;
        strcpy(h->text, line + offset);
        h->updated = when;
        h->count++;
    }
    if (ferror(f) || !h->count) rc = -1;
    fclose(f);
    return rc;
}

static int record(const char *root, const struct ota_report *r, long long when, int seed)
{
    char base[OTA_PATH], dir[OTA_PATH], path[OTA_PATH], temp[OTA_PATH];
    char name[96], text[OTA_REPORT_SIZE];
    if (ota_report_format(text, r) || ota_path(base, root, "history") ||
        ota_mkdir(base) || ota_sync_dir(root) || ota_path(dir, base, r->device) ||
        ota_mkdir(dir) || ota_sync_dir(base)) return -1;
    snprintf(name, sizeof(name), "%llu-%s", r->publication, r->boot);
    if (ota_path(path, dir, name) || ota_path(temp, dir, ".history-XXXXXX")) return -1;
    struct history h;
    int exists = read_history(path, &h, NULL, NULL);
    if (exists < 0 || (!exists && !same_transaction(&h.last, r))) return -1;
    if (!exists && (seed || !strcmp(text, h.text))) return 0;
    int fd = mkstemp(temp);
    if (fd < 0) return -1;
    FILE *out = fdopen(fd, "w");
    if (!out) { close(fd); unlink(temp); return -1; }
    int rc = 0;
    if (!exists && read_history(path, &h, out, NULL)) rc = -1;
    if (!rc && fprintf(out, "%lld %u %s", when, seed ? 1U : 0U, text) < 0) rc = -1;
    if (fflush(out) || fsync(fd)) rc = -1;
    if (fclose(out)) rc = -1;
    if (!rc && (rename(temp, path) || ota_sync_dir(dir))) rc = -1;
    unlink(temp);
    return rc;
}

int ota_history_record(const char *root, const struct ota_report *r)
{
    return record(root, r, (long long)time(NULL), 0);
}

int ota_history_seed(const char *root)
{
    char dir[OTA_PATH], path[OTA_PATH], text[OTA_REPORT_SIZE];
    if (ota_path(dir, root, "devices")) return -1;
    DIR *d = opendir(dir);
    if (!d) return errno == ENOENT ? 0 : -1;
    struct dirent *entry;
    int rc = 0;
    while ((entry = readdir(d))) {
        if (!isalnum((unsigned char)entry->d_name[0])) continue;
        struct ota_report r; struct stat st;
        if (ota_device_read(root, entry->d_name, text) || ota_report_parse(text, &r) ||
            ota_path(path, dir, entry->d_name) || lstat(path, &st) ||
            record(root, &r, (long long)st.st_mtime, 1)) { rc = -1; break; }
    }
    closedir(d);
    return rc;
}

int ota_history_json(const char *root, FILE *out)
{
    char base[OTA_PATH], dir[OTA_PATH], path[OTA_PATH];
    if (ota_path(base, root, "history")) return -1;
    DIR *devices = opendir(base);
    if (!devices) return errno == ENOENT ? 0 : -1;
    struct dirent *device, *entry;
    int rc = 0; unsigned count = 0;
    while (!rc && (device = readdir(devices))) {
        if (device->d_name[0] == '.') continue;
        struct stat st;
        if (!ota_identifier(device->d_name) || ota_path(dir, base, device->d_name) ||
            lstat(dir, &st) || !S_ISDIR(st.st_mode)) { rc = -1; break; }
        DIR *records = opendir(dir);
        if (!records) { rc = -1; break; }
        while ((entry = readdir(records))) {
            if (entry->d_name[0] == '.') continue;
            struct history h;
            char *events = NULL, name[96]; size_t size;
            FILE *f = open_memstream(&events, &size);
            if (!f) { rc = -1; break; }
            int failed = ota_path(path, dir, entry->d_name) || read_history(path, &h, NULL, f);
            if (fclose(f)) failed = 1;
            if (failed) { free(events); rc = -1; break; }
            const struct ota_report *r = &h.last;
            snprintf(name, sizeof(name), "%llu-%s", r->publication, r->boot);
            if (strcmp(device->d_name, r->device) || strcmp(entry->d_name, name)) {
                free(events); rc = -1; break;
            }
            fprintf(out, "%s{\"device\":\"%s\",\"publication\":%llu,\"version\":%u,"
                "\"target\":%u,\"source_boot\":\"%s\",\"product\":\"%s\",\"layout\":\"%s\","
                "\"running\":%u,\"phase\":\"%s\",\"started\":%lld,\"updated\":%lld,"
                "\"imported\":%s,\"events\":[%s],\"category\":\"%s\",\"reason\":\"%s\",\"network\":[", count++ ? "," : "", r->device,
                r->publication, r->version, r->target, r->boot, r->product, r->layout,
                r->running, r->phase, h.first, h.updated, h.imported ? "true" : "false", events, ota_report_category(r), r->reason[0] ? r->reason : "not-reported");
            if (ota_catalog_network_json(root, out, r)) { free(events); rc = -1; break; }
            fputs("]}", out);
            free(events);
        }
        closedir(records);
    }
    closedir(devices);
    return rc || ferror(out) ? -1 : 0;
}


static int walk_history(const char *root, int (*visit)(const struct history *, void *), void *context)
{
    char base[OTA_PATH], dir[OTA_PATH], path[OTA_PATH];
    if (ota_path(base, root, "history")) return -1;
    DIR *devices = opendir(base);
    if (!devices) return errno == ENOENT ? 0 : -1;
    struct dirent *device, *entry; int rc = 0;
    while (!rc && (device = readdir(devices))) {
        if (device->d_name[0] == '.') continue;
        struct stat st;
        if (!ota_identifier(device->d_name) || ota_path(dir, base, device->d_name) ||
            lstat(dir, &st) || !S_ISDIR(st.st_mode)) { rc = -1; break; }
        DIR *records = opendir(dir);
        if (!records) { rc = -1; break; }
        while ((entry = readdir(records))) {
            if (entry->d_name[0] == '.') continue;
            struct history h; char name[96];
            if (ota_path(path, dir, entry->d_name) || read_history(path, &h, NULL, NULL) ||
                strcmp(h.last.device, device->d_name)) { rc = -1; break; }
            snprintf(name, sizeof(name), "%llu-%s", h.last.publication, h.last.boot);
            if (strcmp(name, entry->d_name) || visit(&h, context)) { rc = -1; break; }
        }
        closedir(records);
    }
    closedir(devices); return rc;
}

struct good_query { const char *root, *product, *layout; unsigned long long before, best; };
static int find_good(const struct history *h, void *context)
{
    struct good_query *q = context; const struct ota_report *r = &h->last;
    if (strcmp(r->phase, "success") || r->publication >= q->before || r->publication <= q->best ||
        strcmp(r->product, q->product) || strcmp(r->layout, q->layout)) return 0;
    char directory[OTA_PATH], relative[64];
    snprintf(relative, sizeof(relative), "releases/%llu", r->publication);
    if (ota_path(directory, q->root, relative)) return -1;
    if (!ota_quarantine_blocked(q->root, directory)) q->best = r->publication;
    return 0;
}
unsigned long long ota_history_last_good(const char *root, unsigned long long before,
                                        const char *product, const char *layout)
{
    struct good_query q = {root, product, layout, before, 0};
    return walk_history(root, find_good, &q) ? 0 : q.best;
}
static int replay_incident(const struct history *h, void *context)
{
    return h->incident.publication ? ota_quarantine_report(context, &h->incident) : 0;
}
int ota_history_incidents(const char *root) { return walk_history(root, replay_incident, (void *)root); }

struct summary_rows { const char *root; struct history *rows; size_t count; };
static int collect_summary(const struct history *h, void *context)
{
    struct summary_rows *s = context;
    for (size_t i = 0; i < s->count; i++) {
        struct history *old = &s->rows[i];
        if (old->last.publication != h->last.publication || strcmp(old->last.device, h->last.device)) continue;
        char text[OTA_REPORT_SIZE]; struct ota_report current;
        int is_current = !ota_device_read(s->root, h->last.device, text) && !ota_report_parse(text, &current) &&
            current.publication == h->last.publication && !strcmp(current.boot, h->last.boot);
        int old_current = !ota_device_read(s->root, old->last.device, text) && !ota_report_parse(text, &current) &&
            current.publication == old->last.publication && !strcmp(current.boot, old->last.boot);
        if (is_current || (!old_current && (h->updated > old->updated ||
            (h->updated == old->updated && h->first > old->first)))) *old = *h;
        return 0;
    }
    struct history *rows = realloc(s->rows, (s->count + 1) * sizeof(*rows));
    if (!rows) return -1;
    s->rows = rows; s->rows[s->count++] = *h; return 0;
}
int ota_history_summary(const char *root, FILE *out)
{
    struct summary_rows s = {.root = root}; unsigned emitted = 0;
    int rc = walk_history(root, collect_summary, &s);
    if (rc) { free(s.rows); return rc; }
    for (size_t i = 0; i < s.count; i++) {
        unsigned long long publication = s.rows[i].last.publication;
        size_t j; for (j = 0; j < i; j++) if (s.rows[j].last.publication == publication) break;
        if (j < i) continue;
        unsigned success = 0, failed = 0, device = 0, pending = 0, unchanged = 0, total = 0;
        for (j = i; j < s.count; j++) {
            if (s.rows[j].last.publication != publication) continue;
            const char *category = ota_report_category(&s.rows[j].last); total++;
            if (!strcmp(category, "success")) success++;
            else if (!strcmp(category, "upgrade-failure")) failed++;
            else if (!strcmp(category, "device-error")) device++;
            else if (!strcmp(category, "up-to-date")) unchanged++;
            else pending++;
        }
        fprintf(out, "%s{\"publication\":%llu,\"total_devices\":%u,\"success\":%u,\"upgrade_failed\":%u,\"device_errors\":%u,\"in_progress\":%u,\"up_to_date\":%u}",
            emitted++ ? "," : "", publication, total, success, failed, device, pending, unchanged);
    }
    free(s.rows); return ferror(out) ? -1 : 0;
}
