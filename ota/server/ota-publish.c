#define _XOPEN_SOURCE 700
#include "ota-admin.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Read the existing fwctl FIT manifest using the Buildroot host dtc tool. */
static int property(const char *fit, const char *node, const char *key, char *out, unsigned size)
{
    char *args[] = {"fdtget", "-t", "s", (char *)fit, (char *)node, (char *)key, NULL};
    if (ota_run(args, out, size))
        return -1;
    out[strcspn(out, "\r\n")] = 0;
    return 0;
}

static int validate_fit(const char *directory, const struct release *r)
{
    char fit[OTA_PATH], config[128], node[256], value[256];
    char hwids[OTA_HWIDS_SIZE];
    if (ota_fit_hwids(directory, hwids) || !ota_hwid_supported(hwids, r->hwid)) return -1;
    for (unsigned i = 1; i < 3; i++) {
        char component[OTA_HWIDS_SIZE];
        if (ota_fit_file_hwids(directory, ota_files[i], component)) return -1;
        for (size_t offset = 0; offset < strlen(hwids); offset += 17) {
            char hwid[17]; memcpy(hwid, hwids + offset, 16); hwid[16] = 0;
            if (!ota_hwid_supported(component, hwid)) return -1;
        }
    }
    if (ota_path(fit, directory, "release.itb") ||
        property(fit, "/configurations", "default", config, sizeof(config)) ||
        !ota_identifier(config))
        return -1;
    snprintf(node, sizeof(node), "/configurations/%s", config);
    if (property(fit, node, "fw,type", value, sizeof(value)) || strcmp(value, "release") ||
        property(fit, node, "fw,components", value, sizeof(value)) || strcmp(value, "kernel rootfs"))
        return -1;
    const char *file_keys[] = {"fw,kernel-file", "fw,rootfs-file"};
    const char *hash_keys[] = {"fw,kernel-sha256", "fw,rootfs-sha256"};
    for (int i = 0; i < 2; i++)
        if (property(fit, node, file_keys[i], value, sizeof(value)) || strcmp(value, ota_files[i + 1]) ||
            property(fit, node, hash_keys[i], value, sizeof(value)) || strcmp(value, r->hash[i + 1]))
            return -1;
    return 0;
}

int ota_fit_version(const char *directory, unsigned long *version)
{
    char fit[OTA_PATH], config[128], node[256], value[64], *end;
    if (ota_path(fit, directory, "release.itb") ||
        property(fit, "/configurations", "default", config, sizeof(config)) ||
        !ota_identifier(config))
        return -1;
    snprintf(node, sizeof(node), "/configurations/%s", config);
    char *args[] = {"fdtget", "-t", "x", fit, node, "fw,version", NULL};
    if (ota_run(args, value, sizeof(value)))
        return -1;
    value[strcspn(value, "\r\n")] = 0;
    errno = 0;
    *version = strtoul(value, &end, 16);
    return errno || !*value || *end || !*version || *version > 0xffffffffUL ? -1 : 0;
}

int ota_inspect(const char *directory, struct release *r, unsigned long *version)
{
    char path[OTA_PATH], hwids[OTA_HWIDS_SIZE];
    memset(r, 0, sizeof(*r));
    r->version = 1;
    if (ota_fit_hwids(directory, hwids) || !*hwids) return -1;
    memcpy(r->hwid, hwids, 16); r->hwid[16] = 0;
    for (int i = 0; i < 3; i++)
        if (ota_path(path, directory, ota_files[i]) || ota_hash(path, r->hash[i], &r->size[i]))
            return -1;
    return validate_fit(directory, r) || ota_fit_version(directory, version) ? -1 : 0;
}

int ota_publish(int argc, char **argv)
{
    const char *root_arg = NULL, *source_arg = NULL;
    unsigned long long number = 0;
    for (int i = 2; i < argc; i += 2) {
        if (i + 1 == argc) goto usage;
        if (!strcmp(argv[i], "--root")) root_arg = argv[i + 1];
        else if (!strcmp(argv[i], "--source")) source_arg = argv[i + 1];
        else if (!strcmp(argv[i], "--version")) {
            char *end; errno = 0;
            number = strtoull(argv[i + 1], &end, 10);
            if (errno || *end || !number || number > 2147483647) goto usage;
        } else goto usage;
    }
    if (!root_arg || !source_arg || !number) goto usage;
    char root[OTA_PATH], source[OTA_PATH], releases[OTA_PATH], hwids[OTA_HWIDS_SIZE];
    if (ota_mkdir(root_arg) || !realpath(root_arg, root) || !realpath(source_arg, source) ||
        ota_path(releases, root, "releases") || ota_mkdir(releases) || ota_fit_hwids(source, hwids)) {
        fprintf(stderr, "Invalid publication directory or firmware HWID declaration\n"); return 1;
    }
    size_t count = (strlen(hwids) + 1) / 17;
    if (!count || number + count - 1 > 2147483647) return 1;
    struct release *batch = calloc(count, sizeof(*batch));
    if (!batch) return 1;
    int lock = ota_lock(root);
    if (lock < 0) { free(batch); fprintf(stderr, "Another publisher is active or lock unavailable\n"); return 1; }
    int rc = 1;
    char temp[OTA_PATH] = "", target[OTA_PATH], first[OTA_PATH] = "", name[64], from[OTA_PATH], to[OTA_PATH];
    for (size_t i = 0; i < count; i++) {
        struct release *r = &batch[i];
        if (i) *r = batch[0];
        r->version = number + i;
        memcpy(r->hwid, hwids + i * 17, 16); r->hwid[16] = 0;
        snprintf(name, sizeof(name), "%llu", r->version);
        if (ota_path(target, releases, name) || !access(target, F_OK) ||
            ota_path(temp, releases, ".publish-XXXXXX") || !mkdtemp(temp)) { temp[0] = 0; goto done; }
        for (int j = 0; j < 3; j++) {
            if (ota_path(from, i ? first : source, ota_files[j]) || ota_path(to, temp, ota_files[j])) goto done;
            /* New hardware snapshots share immutable payloads, but never link
             * existing releases: their catalog fingerprints remain unchanged. */
            if (i ? link(from, to) : (ota_copy(from, to) || ota_hash(to, r->hash[j], &r->size[j]))) goto done;
        }
        if (!i && validate_fit(temp, r)) goto done;
        if (ota_path(to, temp, "manifest") || ota_write(to, r) ||
            (!i && ota_quarantine_blocked(root, temp)) || ota_sync_dir(temp) ||
            rename(temp, target) || ota_sync_dir(releases)) goto done;
        temp[0] = 0;
        if (!i) strcpy(first, target);
    }
    /* All files, manifests and database identities must be durable before any
     * device sees the new package. Orphan snapshots are safe after interruption. */
    for (size_t i = 0; i < count; i++) {
        struct ota_catalog_identity identity;
        snprintf(name, sizeof(name), "%llu", batch[i].version);
        if (ota_path(target, releases, name) || ota_catalog_get(root, target, &identity)) goto done;
    }
    if (ota_channels_set(root, batch, count)) goto done;
    printf("Published firmware to %zu declared HWID channel(s), starting at #%llu\n", count, number);
    rc = 0;
done:
    if (temp[0]) {
        for (unsigned i = 0; i < 3; i++) if (!ota_path(to, temp, ota_files[i])) unlink(to);
        if (!ota_path(to, temp, "manifest")) unlink(to);
        rmdir(temp);
    }
    if (rc) fprintf(stderr, "Publication failed; inspect retained snapshots and channel state\n");
    close(lock); free(batch); return rc;
usage:
    fprintf(stderr, "Usage: device-server publish --root DIR --source RELEASE_DIR --version NUMBER\n");
    return 1;
}
