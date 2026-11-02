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
        fprintf(out, "%s{\"device\":\"%s\",\"product\":\"%s\",\"layout\":\"%s\","
            "\"publication\":%llu,\"version\":%u,\"running\":%u,\"phase\":\"%s\",\"updated\":%lld}",
            count++ ? "," : "", r.device, r.product, r.layout, r.publication,
            r.version, r.running, r.phase, (long long)st.st_mtime);
    }
    closedir(d);
    return ferror(out) ? -1 : 0;
}

/* Exact component version only. Never substitute the newest kernel for an old rootfs. */
int ota_recovery_path(const char *root, const char *request, char directory[OTA_PATH])
{
    char product[64], layout[64], extra, canonical[256], releases[OTA_PATH];
    unsigned version;
    if (sscanf(request, "/recovery/%63[^/]/%63[^/]/kernel/%u/kernel.itb%c",
        product, layout, &version, &extra) != 3 || !version ||
        !ota_identifier(product) || !ota_identifier(layout)) return -1;
    snprintf(canonical, sizeof(canonical), "/recovery/%s/%s/kernel/%u/kernel.itb", product, layout, version);
    if (strcmp(canonical, request) || ota_path(releases, root, "releases")) return -1;
    DIR *d = opendir(releases);
    if (!d) return -1;
    struct dirent *entry;
    int rc = -1;
    char selected_hash[65] = "";
    while ((entry = readdir(d))) {
        char *end, path[OTA_PATH], manifest[OTA_PATH], fit[OTA_PATH], conf[128], node[256], value[128];
        unsigned long number = strtoul(entry->d_name, &end, 10);
        if (!number || *end || number > 2147483647 || ota_path(path, releases, entry->d_name)) continue;
        struct stat st;
        if (lstat(path, &st) || !S_ISDIR(st.st_mode)) continue;
        struct release r;
        if (ota_path(manifest, path, "manifest") || ota_read(manifest, &r) ||
            strcmp(product, r.product) || strcmp(layout, r.layout) ||
            ota_path(fit, path, "kernel.itb")) continue;
        char *getconf[] = {"fdtget", "-t", "s", fit, "/configurations", "default", NULL};
        if (ota_run(getconf, conf, sizeof(conf))) continue;
        conf[strcspn(conf, "\r\n")] = 0;
        if (!ota_identifier(conf)) continue;
        snprintf(node, sizeof(node), "/configurations/%s", conf);
        char *getversion[] = {"fdtget", "-t", "u", fit, node, "fw,version", NULL};
        if (ota_run(getversion, value, sizeof(value))) continue;
        value[strcspn(value, "\r\n")] = 0;
        unsigned long candidate = strtoul(value, &end, 10);
        if (*end || candidate != version) continue;
        char hash[65]; unsigned long long size;
        if (ota_hash(fit, hash, &size) || strcmp(hash, r.hash[1]) || size != r.size[1]) continue;
        /* Ambiguous same-version rebuilds cannot be repaired safely. */
        if (*selected_hash && strcmp(hash, selected_hash)) { rc = -1; break; }
        strcpy(selected_hash, hash);
        strcpy(directory, path);
        rc = 0;
    }
    closedir(d);
    return rc;
}

/* Exact written rootfs payload, including its verity tree; tags and FIT wrapper
 * timestamps do not select recovery content. Never substitute latest. */
int ota_rootfs_recovery_path(const char *root, const char *request, char directory[OTA_PATH])
{
    char product[64], layout[64], hash[65], extra, canonical[320], releases[OTA_PATH];
    if (sscanf(request, "/recovery/%63[^/]/%63[^/]/rootfs/%64[^/]/rootfs.itb%c",
        product, layout, hash, &extra) != 3 || !ota_identifier(product) || !ota_identifier(layout) ||
        strlen(hash) != 64 || strspn(hash, "0123456789abcdef") != 64) return -1;
    snprintf(canonical, sizeof(canonical), "/recovery/%s/%s/rootfs/%s/rootfs.itb", product, layout, hash);
    if (strcmp(canonical, request) || ota_path(releases, root, "releases")) return -1;
    DIR *d = opendir(releases);
    if (!d) return -1;
    struct dirent *entry; int rc = -1;
    while ((entry = readdir(d))) {
        char *end, path[OTA_PATH], manifest[OTA_PATH], body[512], root_hash[65], kernel_hash[65];
        unsigned long long n = strtoull(entry->d_name, &end, 10), publication, kernel_size, root_size;
        unsigned version; struct release release;
        if (!n || *end || n > 2147483647 || ota_path(path, releases, entry->d_name) ||
            ota_path(manifest, path, "manifest") || ota_read(manifest, &release) ||
            strcmp(release.product, product) || strcmp(release.layout, layout)) continue;
        struct stat st;
        if (lstat(path, &st) || !S_ISDIR(st.st_mode) || ota_image_identity(path, n, body, sizeof(body))) continue;
        if (sscanf(body, "BSPOTAIMAGE1 %llu %u %llu %64s %llu %64s %c", &publication, &version,
            &kernel_size, kernel_hash, &root_size, root_hash, &extra) != 6 || strcmp(root_hash, hash)) continue;
        strcpy(directory, path); rc = 0; break;
    }
    closedir(d); return rc;
}
