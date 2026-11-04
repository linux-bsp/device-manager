/* Each exact HWID owns one atomic, independently published channel. */
#define _POSIX_C_SOURCE 200809L
#include "ota-admin.h"
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int ota_hwid_supported(const char *values, const char *hwid)
{
    if (!ota_hwid_valid(hwid) || !ota_hwid_list_valid(values)) return 0;
    for (size_t offset = 0; offset < strlen(values); offset += 17)
        if (!memcmp(values + offset, hwid, 16)) return 1;
    return 0;
}

int ota_channel_path(const char *root, const char *hwid, char path[OTA_PATH])
{
    char channels[OTA_PATH];
    return !ota_hwid_valid(hwid) || ota_path(channels, root, "channels") || ota_path(path, channels, hwid) ? -1 : 0;
}

int ota_channel_read(const char *root, const char *hwid, struct release *release)
{
    char path[OTA_PATH];
    return ota_channel_path(root, hwid, path) || ota_read(path, release) || strcmp(release->hwid, hwid) ? -1 : 0;
}

int ota_channel_active(const char *root, const struct release *release)
{
    struct release active;
    return !ota_channel_read(root, release->hwid, &active) && active.version == release->version;
}

int ota_channel_set(const char *root, const struct release *release)
{
    char channels[OTA_PATH], path[OTA_PATH], temporary[OTA_PATH], name[64];
    if (ota_path(channels, root, "channels") || ota_mkdir(channels) || ota_sync_dir(root) ||
        ota_channel_path(root, release->hwid, path)) return -1;
    snprintf(name, sizeof(name), ".%s-%ld", release->hwid, (long)getpid());
    if (ota_path(temporary, channels, name) || ota_write(temporary, release)) return -1;
    int rc = rename(temporary, path) || ota_sync_dir(channels) ? -1 : 0;
    unlink(temporary);
    return rc;
}

unsigned long long ota_publication_next(const char *root)
{
    char path[OTA_PATH];
    if (ota_path(path, root, "releases")) return 0;
    DIR *dir = opendir(path);
    if (!dir) return errno == ENOENT ? 1 : 0;
    unsigned long long next = 1; struct dirent *entry;
    while ((entry = readdir(dir))) {
        char *end; unsigned long long value = strtoull(entry->d_name, &end, 10);
        if (value && !*end && value <= 2147483647 && value >= next) next = value + 1;
    }
    closedir(dir);
    return next <= 2147483647 ? next : 0;
}

static int channels(const char *root, FILE *out, unsigned long long *maximum)
{
    char path[OTA_PATH];
    if (ota_path(path, root, "channels")) return -1;
    DIR *dir = opendir(path);
    if (!dir) return errno == ENOENT ? 0 : -1;
    struct dirent *entry; int count = 0, rc = 0;
    while ((entry = readdir(dir))) {
        if (!ota_hwid_valid(entry->d_name)) continue;
        struct release release;
        if (ota_channel_read(root, entry->d_name, &release)) { rc = -1; break; }
        if (maximum && release.version > *maximum) *maximum = release.version;
        if (out) fprintf(out, "%s{\"hwid\":\"%s\",\"publication\":%llu}", count++ ? "," : "", release.hwid, release.version);
    }
    closedir(dir);
    return rc;
}

unsigned long long ota_channel_max(const char *root)
{
    unsigned long long maximum = 0;
    return channels(root, NULL, &maximum) ? 0 : maximum;
}

int ota_channels_json(const char *root, FILE *out)
{
    return channels(root, out, NULL);
}
