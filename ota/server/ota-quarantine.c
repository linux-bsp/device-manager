#define _XOPEN_SOURCE 700
#include "ota-admin.h"
#include "ota-device.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct audit_event {
    long long when;
    char action[32], kernel[65], rootfs[65];
    unsigned long long failed, source, replacement;
    struct ota_report report;
};
static int identity_key(const char *directory, char key[130], char kernel[65], char rootfs[65])
{
    char body[512], extra; unsigned long long publication, sizes[2]; unsigned version;
    if (ota_image_identity(directory, 1, body, sizeof(body)) ||
        sscanf(body, "BSPOTAIMAGE1 %llu %u %llu %64s %llu %64s %c", &publication, &version,
            &sizes[0], kernel, &sizes[1], rootfs, &extra) != 6) return -1;
    snprintf(key, 130, "%s-%s", kernel, rootfs); return 0;
}
int ota_quarantine_hashes_blocked(const char *root, const char *kernel, const char *rootfs)
{
    char key[130], base[OTA_PATH], path[OTA_PATH]; struct stat st;
    if (strlen(kernel) != 64 || strspn(kernel, "0123456789abcdef") != 64 ||
        strlen(rootfs) != 64 || strspn(rootfs, "0123456789abcdef") != 64) return -1;
    snprintf(key, sizeof(key), "%s-%s", kernel, rootfs);
    if (ota_path(base, root, "quarantine")) return -1;
    if (lstat(base, &st)) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode) || ota_path(path, base, key)) return -1;
    if (lstat(path, &st)) return errno == ENOENT ? 0 : -1;
    return S_ISREG(st.st_mode) ? 1 : -1;
}
int ota_quarantine_blocked(const char *root, const char *directory)
{
    char key[130], kernel[65], rootfs[65], base[OTA_PATH]; struct stat st;
    if (ota_path(base, root, "quarantine")) return -1;
    if (lstat(base, &st)) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode) || identity_key(directory, key, kernel, rootfs)) return -1;
    return ota_quarantine_hashes_blocked(root, kernel, rootfs);
}

static int write_event(const char *directory, const char *name, const struct audit_event *e)
{
    char path[OTA_PATH], temporary[OTA_PATH], text[OTA_REPORT_SIZE];
    if (ota_path(path, directory, name) || ota_path(temporary, directory, ".event-XXXXXX") ||
        ota_report_format(text, &e->report)) return -1;
    int fd = mkstemp(temporary); if (fd < 0) return -1;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(temporary); return -1; }
    int rc = fprintf(f, "BSPOTAAUDIT1 %lld %s %llu %llu %llu %s %s\n%s", e->when, e->action,
        e->failed, e->source, e->replacement, e->kernel, e->rootfs, text) < 0 ? -1 : 0;
    if (fflush(f) || fsync(fd)) rc = -1;
    if (fclose(f)) rc = -1;
    if (!rc && link(temporary, path) && errno != EEXIST) rc = -1;
    unlink(temporary);
    if (!rc) rc = ota_sync_dir(directory);
    return rc;
}
static int audit(const char *root, struct audit_event *e, const char *action)
{
    char directory[OTA_PATH], name[256];
    snprintf(e->action, sizeof(e->action), "%s", action);
    if (ota_path(directory, root, "audit") || ota_mkdir(directory) || ota_sync_dir(root)) return -1;
    int n = snprintf(name, sizeof(name), "%llu-%s-%s-%s-%s-%llu", e->failed,
        e->report.device, e->report.boot, e->report.phase, action, e->replacement);
    if (n < 0 || n >= (int)sizeof(name)) return -1;
    return write_event(directory, name, e);
}
static int next_publication(const char *releases, unsigned long long *next)
{
    DIR *d = opendir(releases); if (!d) return -1;
    struct dirent *entry; *next = 1;
    while ((entry = readdir(d))) {
        char *end; unsigned long long n = strtoull(entry->d_name, &end, 10);
        if (*entry->d_name && !*end && n >= *next) *next = n + 1;
    }
    closedir(d); return *next <= 2147483647 ? 0 : -1;
}
/* Caller owns the publisher lock. Only copy a previously health-confirmed image. */
static int fallback(const char *root, struct audit_event *e)
{
    char base[OTA_PATH], source[OTA_PATH], target[OTA_PATH], temporary[OTA_PATH];
    char path[OTA_PATH], to[OTA_PATH], name[32], latest[OTA_PATH]; struct release release;
    e->source = ota_history_last_good(root, e->failed, e->report.product, e->report.layout);
    if (!e->source) return audit(root, e, "no-confirmed-fallback");
    snprintf(name, sizeof(name), "%llu", e->source);
    if (ota_path(base, root, "releases") || ota_path(source, base, name) ||
        ota_path(path, source, "manifest") || ota_read(path, &release) || ota_verify(source, &release) ||
        next_publication(base, &e->replacement) || ota_path(temporary, base, ".fallback-XXXXXX") || !mkdtemp(temporary)) return -1;
    if (audit(root, e, "fallback-selected")) return -1;
    for (unsigned i = 0; i < 3; i++) {
        if (ota_path(path, source, ota_files[i]) || ota_path(to, temporary, ota_files[i]) || ota_copy(path, to)) return -1;
    }
    release.version = e->replacement;
    snprintf(name, sizeof(name), "%llu", e->replacement);
    if (ota_path(path, temporary, "manifest") || ota_write(path, &release) || ota_sync_dir(temporary) ||
        ota_path(target, base, name) || rename(temporary, target) || ota_sync_dir(base) ||
        ota_path(latest, root, "latest") || ota_path(to, root, ".fallback-latest") ||
        ota_write(to, &release) || rename(to, latest) || ota_sync_dir(root)) return -1;
    return audit(root, e, "fallback-published");
}
int ota_quarantine_report(const char *root, const struct ota_report *report)
{
    if (!ota_report_incident(report)) return 0;
    char relative[64], directory[OTA_PATH], manifest[OTA_PATH], key[130];
    char base[OTA_PATH], latest[OTA_PATH], saved[OTA_PATH]; struct release release;
    struct audit_event e = {.when = (long long)time(NULL), .failed = report->publication, .report = *report};
    snprintf(relative, sizeof(relative), "releases/%llu", report->publication);
    if (ota_path(directory, root, relative) || ota_path(manifest, directory, "manifest")) return -1;
    if (access(manifest, F_OK)) return errno == ENOENT ? 0 : -1;
    if (ota_read(manifest, &release) || strcmp(release.product, report->product) || strcmp(release.layout, report->layout)) return -1;
    unsigned long version;
    if (ota_fit_version(directory, &version) || version != report->version ||
        identity_key(directory, key, e.kernel, e.rootfs)) return -1;
    int lock = ota_lock(root); if (lock < 0) return -1;
    int rc = -1;
    if (ota_path(base, root, "quarantine") || ota_mkdir(base) || ota_sync_dir(root)) goto done;
    strcpy(e.action, "quarantined");
    if (write_event(base, key, &e) || audit(root, &e, "quarantined") || ota_path(latest, root, "latest")) goto done;
    if (!access(latest, F_OK)) {
        struct release active; char active_key[130], kernel[65], rootfs[65];
        if (ota_read(latest, &active)) goto done;
        snprintf(relative, sizeof(relative), "releases/%llu", active.version);
        if (ota_path(directory, root, relative) || identity_key(directory, active_key, kernel, rootfs)) goto done;
        if (strcmp(key, active_key)) { rc = 0; goto done; } /* Never replace a different current release. */
        snprintf(relative, sizeof(relative), "withdrawn-%llu", active.version);
        if (audit(root, &e, "distribution-stopped") || ota_path(saved, base, relative) ||
            rename(latest, saved) || ota_sync_dir(root) || ota_sync_dir(base)) goto done;
    } else if (errno != ENOENT) goto done;
    rc = fallback(root, &e);
done:
    close(lock); return rc;
}
int ota_quarantine_json(const char *root, void *stream)
{
    FILE *out = stream; char directory[OTA_PATH], path[OTA_PATH];
    if (ota_path(directory, root, "audit")) return -1;
    DIR *dir = opendir(directory); if (!dir) return errno == ENOENT ? 0 : -1;
    struct dirent *entry; unsigned count = 0; int rc = 0;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        if (ota_path(path, directory, entry->d_name)) { rc = -1; break; }
        int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC); struct stat st;
        if (fd < 0) { rc = -1; break; }
        if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); rc = -1; break; }
        FILE *f = fdopen(fd, "r"); if (!f) { close(fd); rc = -1; break; }
        char line[512], text[OTA_REPORT_SIZE], extra; struct audit_event e;
        int bad = !fgets(line, sizeof(line), f) || !fgets(text, sizeof(text), f) ||
            sscanf(line, "BSPOTAAUDIT1 %lld %31s %llu %llu %llu %64s %64s %c", &e.when, e.action,
                &e.failed, &e.source, &e.replacement, e.kernel, e.rootfs, &extra) != 7 ||
            !ota_identifier(e.action) || strlen(e.kernel) != 64 || strlen(e.rootfs) != 64 ||
            strspn(e.kernel, "0123456789abcdef") != 64 || strspn(e.rootfs, "0123456789abcdef") != 64 || ota_report_parse(text, &e.report);
        fclose(f); if (bad) { rc = -1; break; }
        fprintf(out, "%s{\"timestamp\":%lld,\"action\":\"%s\",\"publication\":%llu,\"source_publication\":%llu,\"replacement_publication\":%llu,"
            "\"device\":\"%s\",\"boot\":\"%s\",\"version\":%u,\"category\":\"%s\",\"reason\":\"%s\",\"kernel_sha256\":\"%s\",\"rootfs_sha256\":\"%s\"}",
            count++ ? "," : "", e.when, e.action, e.failed, e.source, e.replacement, e.report.device, e.report.boot,
            e.report.version, ota_report_category(&e.report), e.report.reason[0] ? e.report.reason : "not-reported", e.kernel, e.rootfs);
    }
    closedir(dir); return rc || ferror(out) ? -1 : 0;
}
