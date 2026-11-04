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
#include <unistd.h>

/* 1 means no previous transaction. Other failures must stop automatic installs. */
int ota_device_read(const char *root, const char *device, char text[OTA_REPORT_SIZE])
{
    char dir[OTA_PATH], path[OTA_PATH];
    if (!ota_identifier(device) || !isalnum((unsigned char)*device) ||
        ota_path(dir, root, "devices") || ota_path(path, dir, device))
        return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return errno == ENOENT ? 1 : -1;
    struct stat st;
    int rc = -1;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size < OTA_REPORT_SIZE) {
        ssize_t n = read(fd, text, (size_t)st.st_size);
        if (n == st.st_size) {
            text[n] = 0;
            struct ota_report r;
            if (!ota_report_parse(text, &r) && !strcmp(r.device, device))
                rc = 0;
        }
    }
    close(fd);
    return rc;
}

int ota_report_targets(const char *root, const struct ota_report *report)
{
    char relative[64], directory[OTA_PATH], manifest[OTA_PATH]; struct release release;
    snprintf(relative, sizeof(relative), "releases/%llu", report->publication);
    if (ota_path(directory, root, relative) || ota_path(manifest, directory, "manifest")) return 0;
    if (access(manifest, F_OK)) return errno == ENOENT;
    return !ota_read(manifest, &release) && !strcmp(release.hwid, report->hwid);
}

int ota_device_write(const char *root, const struct ota_report *r)
{
    char dir[OTA_PATH], path[OTA_PATH], temp[OTA_PATH], text[OTA_REPORT_SIZE];
    if (ota_report_format(text, r) || ota_path(dir, root, "devices") || ota_mkdir(dir) ||
        ota_sync_dir(root) || ota_path(path, dir, r->device) ||
        ota_path(temp, dir, ".report-XXXXXX"))
        return -1;
    if (ota_history_record(root, r) || ota_quarantine_report(root, r)) return -1;
    int fd = mkstemp(temp);
    if (fd < 0)
        return -1;
    size_t size = strlen(text);
    int rc = write(fd, text, size) == (ssize_t)size && !fsync(fd) ? 0 : -1;
    if (close(fd)) rc = -1;
    if (!rc && (rename(temp, path) || ota_sync_dir(dir))) rc = -1;
    unlink(temp);
    return rc;
}

int ota_devices_json(const char *root, FILE *out)
{
    char dir[OTA_PATH], path[OTA_PATH], text[OTA_REPORT_SIZE];
    if (ota_path(dir, root, "devices")) return -1;
    DIR *d = opendir(dir);
    if (!d) return errno == ENOENT ? 0 : -1;
    struct dirent *entry;
    unsigned count = 0;
    while ((entry = readdir(d))) {
        if (!isalnum((unsigned char)entry->d_name[0])) continue;
        struct ota_report r;
        struct stat st;
        if (ota_device_read(root, entry->d_name, text) || ota_report_parse(text, &r) ||
            ota_path(path, dir, entry->d_name) || stat(path, &st)) continue;
        fprintf(out, "%s{\"device\":\"%s\",\"hwid\":\"%s\","
            "\"publication\":%llu,\"version\":%u,\"running\":%u,\"phase\":\"%s\",\"updated\":%lld}",
            count++ ? "," : "", r.device, r.hwid, r.publication,
            r.version, r.running, r.phase, (long long)st.st_mtime);
    }
    closedir(d);
    return ferror(out) ? -1 : 0;
}

/* Recovery never substitutes a different HWID, version or content identity. */
static int recovery_path(const char *root, const char *request, int rootfs, char directory[OTA_PATH])
{
    char hwid[17], id[65], hash[65] = "", extra, canonical[384], releases[OTA_PATH];
    unsigned version;
    if (rootfs) {
        if (sscanf(request, "/recovery/%16[^/]/rootfs/%u/%64[^/]/%64[^/]/rootfs.itb%c",
            hwid, &version, id, hash, &extra) != 4) return -1;
        snprintf(canonical, sizeof(canonical), "/recovery/%s/rootfs/%u/%s/%s/rootfs.itb", hwid, version, id, hash);
    } else {
        if (sscanf(request, "/recovery/%16[^/]/kernel/%u/%64[^/]/kernel.itb%c", hwid, &version, id, &extra) != 3) return -1;
        snprintf(canonical, sizeof(canonical), "/recovery/%s/kernel/%u/%s/kernel.itb", hwid, version, id);
    }
    if (!version || !ota_hwid_valid(hwid) || strlen(id) != 64 || strspn(id, "0123456789abcdef") != 64 ||
        (rootfs && (strlen(hash) != 64 || strspn(hash, "0123456789abcdef") != 64)) ||
        strcmp(canonical, request) || ota_path(releases, root, "releases")) return -1;
    DIR *dir = opendir(releases);
    if (!dir) return -1;
    struct dirent *entry; int rc = -1;
    while ((entry = readdir(dir))) {
        char *end, path[OTA_PATH], manifest[OTA_PATH];
        unsigned long long number = strtoull(entry->d_name, &end, 10);
        if (!number || *end || number > 2147483647 || ota_path(path, releases, entry->d_name) || ota_path(manifest, path, "manifest")) continue;
        struct release release; struct ota_catalog_identity identity;
        if (ota_read(manifest, &release) || strcmp(release.hwid, hwid) || ota_catalog_get(root, path, &identity) ||
            identity.version != version || strcmp(identity.hash, id) || (rootfs && strcmp(identity.rootfs, hash))) continue;
        strcpy(directory, path); rc = 0; break;
    }
    closedir(dir);
    return rc;
}
int ota_recovery_path(const char *root, const char *request, char directory[OTA_PATH])
{
    return recovery_path(root, request, 0, directory);
}
int ota_rootfs_recovery_path(const char *root, const char *request, char directory[OTA_PATH])
{
    return recovery_path(root, request, 1, directory);
}
