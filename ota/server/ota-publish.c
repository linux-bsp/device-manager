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
    if (ota_fit_hwids(directory, hwids)) return -1;
    if (ota_path(fit, directory, "release.itb") ||
        property(fit, "/configurations", "default", config, sizeof(config)) ||
        !ota_identifier(config))
        return -1;
    snprintf(node, sizeof(node), "/configurations/%s", config);
    if (property(fit, node, "fw,type", value, sizeof(value)) || strcmp(value, "release") ||
        property(fit, node, "fw,product", value, sizeof(value)) || strcmp(value, r->product) ||
        property(fit, node, "fw,layout-id", value, sizeof(value)) || strcmp(value, r->layout) ||
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
    char fit[OTA_PATH], config[128], node[256], path[OTA_PATH];
    memset(r, 0, sizeof(*r));
    r->version = 1; /* Draft metadata uses a placeholder publication number. */
    if (ota_path(fit, directory, "release.itb") ||
        property(fit, "/configurations", "default", config, sizeof(config)) ||
        !ota_identifier(config))
        return -1;
    snprintf(node, sizeof(node), "/configurations/%s", config);
    if (property(fit, node, "fw,product", r->product, sizeof(r->product)) ||
        property(fit, node, "fw,layout-id", r->layout, sizeof(r->layout)) ||
        !ota_identifier(r->product) || !ota_identifier(r->layout))
        return -1;
    for (int i = 0; i < 3; i++)
        if (ota_path(path, directory, ota_files[i]) || ota_hash(path, r->hash[i], &r->size[i]))
            return -1;
    return validate_fit(directory, r) || ota_fit_version(directory, version) ? -1 : 0;
}

int ota_publish(int argc, char **argv)
{
    const char *root_arg = NULL, *source_arg = NULL;
    struct release r = {0}, old;
    for (int i = 2; i < argc; i += 2) {
        if (i + 1 == argc)
            goto usage;
        if (!strcmp(argv[i], "--root"))
            root_arg = argv[i + 1];
        else if (!strcmp(argv[i], "--source"))
            source_arg = argv[i + 1];
        else if (!strcmp(argv[i], "--version")) {
            char *end;
            errno = 0;
            r.version = strtoull(argv[i + 1], &end, 10);
            if (errno || *end || !r.version || r.version > 2147483647)
                goto usage;
        } else if (!strcmp(argv[i], "--product") && ota_identifier(argv[i + 1]))
            strcpy(r.product, argv[i + 1]);
        else if (!strcmp(argv[i], "--layout") && ota_identifier(argv[i + 1]))
            strcpy(r.layout, argv[i + 1]);
        else
            goto usage;
    }
    if (!root_arg || !source_arg || !r.version || !r.product[0] || !r.layout[0])
        goto usage;
    char root[OTA_PATH], source[OTA_PATH], releases[OTA_PATH], latest[OTA_PATH];
    if (ota_mkdir(root_arg) || !realpath(root_arg, root) || !realpath(source_arg, source) ||
        ota_path(releases, root, "releases") || ota_mkdir(releases) || ota_path(latest, root, "latest")) {
        perror("Publication directory");
        return 1;
    }
    int lock = ota_lock(root);
    if (lock < 0) {
        fprintf(stderr, "Another publisher is active or lock unavailable\n");
        return 1;
    }
    int rc = 1;
    if (!access(latest, F_OK) && (ota_read(latest, &old) || r.version <= old.version ||
        strcmp(r.product, old.product) || strcmp(r.layout, old.layout))) {
        fprintf(stderr, "Use a higher publication number and the same product/layout\n");
        goto done;
    }
    char temp[OTA_PATH], target[OTA_PATH], name[64], from[OTA_PATH], to[OTA_PATH];
    snprintf(name, sizeof(name), "%llu", r.version);
    if (ota_path(target, releases, name) || !access(target, F_OK) ||
        ota_path(temp, releases, ".publish-XXXXXX") || !mkdtemp(temp)) {
        fprintf(stderr, "Release already exists or staging failed\n");
        goto done;
    }
    for (int i = 0; i < 3; i++) {
        if (ota_path(from, source, ota_files[i]) || ota_path(to, temp, ota_files[i]) ||
            ota_copy(from, to) || ota_hash(to, r.hash[i], &r.size[i]))
            goto cleanup;
    }
    if (validate_fit(temp, &r)) {
        fprintf(stderr, "FIT product/layout, filenames or component SHA256 mismatch\n");
        goto cleanup;
    }
    if (ota_path(to, temp, "manifest") || ota_write(to, &r) || ota_quarantine_blocked(root, temp)) {
        fprintf(stderr, "Firmware is quarantined or quarantine validation failed\n");
        goto cleanup;
    }
    /* Snapshot first; the atomic latest switch happens only after all files exist. */
    if (ota_sync_dir(temp) ||
        rename(temp, target) || ota_sync_dir(releases))
        goto cleanup;
    struct ota_catalog_identity identity;
    if (ota_catalog_get(root, target, &identity)) {
        fprintf(stderr, "Cannot persist firmware identity; latest unchanged\n");
        goto done;
    }
    snprintf(name, sizeof(name), ".latest-%ld", (long)getpid());
    if (ota_path(to, root, name) || ota_write(to, &r) || rename(to, latest) || ota_sync_dir(root)) {
        fprintf(stderr, "Snapshot retained; latest publication failed\n");
        goto done;
    }
    printf("Published %s/%s #%llu: /releases/%llu/release.itb\n",
           r.product, r.layout, r.version, r.version);
    rc = 0;
    goto done;
cleanup:
    for (int i = 0; i < 3; i++)
        if (!ota_path(to, temp, ota_files[i]))
            unlink(to);
    if (!ota_path(to, temp, "manifest"))
        unlink(to);
    rmdir(temp);
    fprintf(stderr, "Publication failed; latest unchanged\n");
done:
    close(lock);
    return rc;
usage:
    fprintf(stderr, "Usage: device-server publish --root DIR --source RELEASE_DIR "
            "--version NUMBER --product ID --layout ID\n");
    return 1;
}
