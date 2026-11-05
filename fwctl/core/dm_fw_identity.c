// SPDX-License-Identifier: GPL-2.0+
#define _POSIX_C_SOURCE 200809L
#include "dm_fw_update_priv.h"
#include "dm_fw_fit_priv.h"
#include <arpa/inet.h>
#include <errno.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Old bootloaders can discard identity extensions when saving boot attempts.
 * Reconstruct the label from this deployment's verified bytes, never /latest.
 * No metadata is changed, and this is not a boot-health confirmation. */
int fw_identity_from_storage(const struct fw_layout *layout, struct fw_storage *storage,
    const struct fw_metadata *metadata, uint32_t deployment, struct fw_identity *identity)
{
    if (deployment > 1 || !identity) return -EINVAL;
    memset(identity, 0, sizeof(*identity));
    const struct fw_deployment *d = &metadata->deployment[deployment];
    const struct fw_component_slot *kernel = &d->component[FW_COMPONENT_KERNEL];
    const struct fw_component_slot *rootfs = &d->component[FW_COMPONENT_ROOTFS];
    const struct fw_region *kr = fw_layout_component_region(layout, FW_COMPONENT_KERNEL, kernel->slot);
    const struct fw_region *rr = fw_layout_component_region(layout, FW_COMPONENT_ROOTFS, rootfs->slot);
    if (!d->release_version || !kernel->valid || !rootfs->valid || !kr || !rr) return -ENODATA;
    if (kernel->version != d->release_version || rootfs->version != d->release_version) return -EBADMSG;
    uint32_t header[2];
    int ret = fw_storage_read(storage, kr->offset, header, sizeof(header));
    if (ret) return ret;
    uint64_t bytes = ntohl(header[1]);
    if (ntohl(header[0]) != 0xd00dfeed || bytes < 40 || bytes > kr->size) return -EBADMSG;
    char path[FW_PATH_SIZE];
    if (snprintf(path, sizeof(path), "%s/fw-identity-XXXXXX", layout->work_dir) >= (int)sizeof(path)) return -ENAMETOOLONG;
    int fd = mkstemp(path);
    if (fd < 0) return -errno;
    FILE *file = fdopen(fd, "wb");
    if (!file) { ret = -errno; close(fd); unlink(path); return ret; }
    unsigned char buffer[65536];
    for (uint64_t offset = 0; offset < bytes;) {
        size_t count = bytes - offset > sizeof(buffer) ? sizeof(buffer) : (size_t)(bytes - offset);
        ret = fw_storage_read(storage, kr->offset + offset, buffer, count);
        if (ret || fwrite(buffer, 1, count, file) != count) { if (!ret) ret = -EIO; break; }
        offset += count;
    }
    if (fclose(file) && !ret) ret = -EIO;
    if (ret) goto done;
    struct fw_fit fit;
    ret = fw_fit_open(&fit, path, layout);
    if (ret) goto done;
    struct fw_package package;
    ret = fw_fit_verify_package(layout, &fit, &package);
    if (!ret && (package.component != FW_COMPONENT_KERNEL || package.version != d->release_version ||
                 package.rollback_index < kernel->rollback_index)) ret = -EBADMSG;
    if (!ret) ret = fw_fit_rootfs_identity(&fit, &identity->size[1], identity->hash[1]);
    if (!ret && identity->size[1] > rr->size) ret = -EFBIG;
    unsigned length;
    identity->size[0] = bytes;
    if (!ret && (!EVP_Digest(fit.data, fit.fit_size, identity->hash[0], &length, EVP_sha256(), NULL) || length != 32)) ret = -EIO;
    fw_fit_close(&fit);
    if (ret) goto done;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) { ret = -ENOMEM; goto done; }
    if (!EVP_DigestInit_ex(ctx, EVP_sha256(), NULL)) ret = -EIO;
    for (uint64_t offset = 0; !ret && offset < identity->size[1];) {
        size_t count = identity->size[1] - offset > sizeof(buffer) ? sizeof(buffer) : (size_t)(identity->size[1] - offset);
        ret = fw_storage_read(storage, rr->offset + offset, buffer, count);
        if (!ret && !EVP_DigestUpdate(ctx, buffer, count)) ret = -EIO;
        offset += count;
    }
    unsigned char actual[32];
    if (!ret && (!EVP_DigestFinal_ex(ctx, actual, &length) || length != 32)) ret = -EIO;
    EVP_MD_CTX_free(ctx);
    if (!ret && memcmp(actual, identity->hash[1], 32)) ret = -EBADMSG;
    if (ret) goto done;
    char hashes[2][65], canonical[256];
    for (unsigned i = 0; i < 2; i++)
        for (unsigned j = 0; j < 32; j++) snprintf(hashes[i] + j * 2, 3, "%02x", identity->hash[i][j]);
    int count = snprintf(canonical, sizeof(canonical), "BSPFWCONTENT1\nkernel %llu %s\nrootfs %llu %s\n",
        (unsigned long long)identity->size[0], hashes[0], (unsigned long long)identity->size[1], hashes[1]);
    if (count < 0 || count >= (int)sizeof(canonical) ||
        !EVP_Digest(canonical, (size_t)count, identity->id, &length, EVP_sha256(), NULL) || length != 32) { ret = -EIO; goto done; }
    snprintf(identity->tag, sizeof(identity->tag), "v%u.%u.%u", d->release_version >> 16,
        (d->release_version >> 8) & 255, d->release_version & 255);
    identity->valid = 1;
done:
    unlink(path);
    if (ret) memset(identity, 0, sizeof(*identity));
    return ret;
}
