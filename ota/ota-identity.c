#define _POSIX_C_SOURCE 200809L
#include "ota-admin.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The kernel writer stores fdt_totalsize bytes; rootfs stores the firmware
 * image referenced by the default FIT configuration, not its FIT wrapper. */
int ota_image_identity(const char *directory, unsigned long long publication,
                       char *body, size_t capacity)
{
    char path[OTA_PATH], manifest[OTA_PATH], temp[] = "/tmp/ota-identity-XXXXXX";
    char conf[128], node[256], image[128], names[2048], index[32];
    char hashes[2][65]; unsigned long long sizes[2]; unsigned long version;
    struct release release;
    if (ota_path(manifest, directory, "manifest") || ota_read(manifest, &release) ||
        ota_verify(directory, &release) || ota_fit_version(directory, &version)) return -1;
    int fd = mkstemp(temp), rc = -1;
    if (fd < 0) return -1;
    FILE *out = fdopen(fd, "wb"), *in = NULL;
    if (!out) { close(fd); unlink(temp); return -1; }
    if (ota_path(path, directory, "kernel.itb") || !(in = fopen(path, "rb"))) goto done;
    uint32_t header[2];
    if (fread(header, 1, sizeof(header), in) != sizeof(header) || ntohl(header[0]) != 0xd00dfeed ||
        ntohl(header[1]) < 40 || ntohl(header[1]) > release.size[1]) goto done;
    rewind(in);
    unsigned long long remaining = ntohl(header[1]);
    while (remaining) {
        char buffer[65536]; size_t n = remaining > sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
        if (fread(buffer, 1, n, in) != n || fwrite(buffer, 1, n, out) != n) goto done;
        remaining -= n;
    }
    fclose(in); in = NULL;
    if (fclose(out)) { out = NULL; goto done; } out = NULL;
    if (ota_hash(temp, hashes[0], &sizes[0]) || ota_path(path, directory, "rootfs.itb")) goto done;
    char *getconf[] = {"fdtget", "-t", "s", path, "/configurations", "default", NULL};
    if (ota_run(getconf, conf, sizeof(conf))) goto done;
    conf[strcspn(conf, "\r\n")] = 0;
    if (!ota_identifier(conf)) goto done;
    snprintf(node, sizeof(node), "/configurations/%s", conf);
    char *getimage[] = {"fdtget", "-t", "s", path, node, "firmware", NULL};
    if (ota_run(getimage, image, sizeof(image))) goto done;
    image[strcspn(image, "\r\n")] = 0;
    if (!ota_identifier(image)) goto done;
    char *list[] = {"fdtget", "-l", path, "/images", NULL};
    if (ota_run(list, names, sizeof(names))) goto done;
    unsigned position = 0; char *save = NULL, *name = strtok_r(names, "\n", &save);
    for (; name && strcmp(name, image); position++) name = strtok_r(NULL, "\n", &save);
    if (!name) goto done;
    snprintf(index, sizeof(index), "%u", position);
    char *extract[] = {"dumpimage", "-T", "flat_dt", "-p", index, "-o", temp, path, NULL};
    char ignored[2048];
    if (ota_run(extract, ignored, sizeof(ignored)) || ota_hash(temp, hashes[1], &sizes[1]) || !sizes[1]) goto done;
    int length = snprintf(body, capacity, "BSPOTAIMAGE1 %llu %lu %llu %s %llu %s\n",
        publication, version, sizes[0], hashes[0], sizes[1], hashes[1]);
    if (length > 0 && (size_t)length < capacity) rc = 0;
done:
    if (in) fclose(in);
    if (out) fclose(out);
    unlink(temp);
    return rc;
}

/* Content identity excludes publication number; labels are derived from FIT version. */
int ota_content_identity(const char *body, char hash[65], unsigned long *version)
{
    char kernel[65], rootfs[65], extra, canonical[256];
    unsigned long long publication, sizes[2]; unsigned value;
    if (sscanf(body, "BSPOTAIMAGE1 %llu %u %llu %64s %llu %64s %c", &publication, &value,
            &sizes[0], kernel, &sizes[1], rootfs, &extra) != 6) return -1;
    int count = snprintf(canonical, sizeof(canonical), "BSPFWCONTENT1\nkernel %llu %s\nrootfs %llu %s\n",
        sizes[0], kernel, sizes[1], rootfs);
    if (count < 0 || count >= (int)sizeof(canonical)) return -1;
    char temporary[] = "/tmp/ota-content-XXXXXX";
    int fd = mkstemp(temporary); if (fd < 0) return -1;
    int rc = write(fd, canonical, (size_t)count) == count ? 0 : -1;
    if (close(fd)) rc = -1;
    unsigned long long bytes;
    if (!rc) rc = ota_hash(temporary, hash, &bytes);
    unlink(temporary); if (!rc) *version = value; return rc;
}

int ota_firmware_identity(const char *directory, char hash[65], unsigned long *version)
{
    char body[512];
    if (ota_image_identity(directory, 1, body, sizeof(body))) return -1;
    return ota_content_identity(body, hash, version);
}
