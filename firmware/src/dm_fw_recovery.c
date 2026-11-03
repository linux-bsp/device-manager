// SPDX-License-Identifier: GPL-2.0+
#define _POSIX_C_SOURCE 200809L
#include "dm_fw_update_priv.h"
#include "dm_fw_fit_priv.h"
#include <errno.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <openssl/evp.h>
#include <string.h>

/* Offline repair of the booted rootfs only. Its identity comes from the verified
 * kernel DTB, not from the server's latest publication or the package wrapper. */
int fw_restore_rootfs(const struct fw_layout *layout, struct fw_storage *storage,
    uint32_t deployment, const char *path, uint64_t bytes, const uint8_t sha256[32],
    bool allow_unselected)
{
    struct fw_metadata metadata;
    struct fw_fit fit;
    struct fw_package package;
    const void *data;
    size_t size;
    unsigned char digest[32]; unsigned length;
    if (deployment > 1 || !bytes || !sha256 || !path || strstr(path, "://")) return -EINVAL;
    int ret = fw_metadata_load(storage, &metadata);
    if (ret) return ret;
    if ((!allow_unselected && metadata.selected_deployment != deployment) ||
        (allow_unselected && metadata.selected_deployment == deployment)) return -EPERM;
    const struct fw_component_slot *slot = &metadata.deployment[deployment].component[FW_COMPONENT_ROOTFS];
    const struct fw_region *region = fw_layout_component_region(layout, FW_COMPONENT_ROOTFS, slot->slot);
    if (!slot->valid || !region || bytes > region->size) return -EINVAL;
    ret = fw_fit_open(&fit, path, layout);
    if (ret) return ret;
    ret = fw_fit_verify_package(layout, &fit, &package);
    if (!ret && (package.component != FW_COMPONENT_ROOTFS || package.version != slot->version ||
        package.rollback_index < slot->rollback_index)) ret = -EPERM;
    if (!ret) ret = fw_fit_get_component_data(&fit, FW_COMPONENT_ROOTFS, NULL, &data, &size);
    if (!ret && size != bytes) ret = -EBADMSG;
    if (!ret && (!EVP_Digest(data, size, digest, &length, EVP_sha256(), NULL) || length != 32)) ret = -EIO;
    if (!ret && memcmp(digest, sha256, 32)) ret = -EBADMSG;
    if (!ret) ret = fw_storage_write_region(storage, region, data, size);
    fw_fit_close(&fit);
    /* Do not mark good: real rootfs services still own health confirmation. */
    return ret;
}

int fw_validate_recovery_kernel(const struct fw_layout *layout, struct fw_storage *storage,
    const struct fw_metadata *metadata, uint32_t deployment)
{
    const struct fw_component_slot *slot = &metadata->deployment[deployment].component[FW_COMPONENT_KERNEL];
    const struct fw_region *region = fw_layout_component_region(layout, FW_COMPONENT_KERNEL, slot->slot);
    uint32_t header[2];
    char path[FW_PATH_SIZE];
    if (!slot->valid || !region || fw_storage_read(storage, region->offset, header, sizeof(header)) ||
        ntohl(header[0]) != 0xd00dfeed || ntohl(header[1]) < 40 || ntohl(header[1]) > region->size) return -EINVAL;
    if (snprintf(path, sizeof(path), "%s/fw-fallback-XXXXXX", layout->work_dir) >= (int)sizeof(path)) return -ENAMETOOLONG;
    int fd = mkstemp(path), ret = -EIO;
    if (fd < 0) return -errno;
    FILE *out = fdopen(fd, "wb");
    if (!out) { close(fd); unlink(path); return -errno; }
    unsigned char buffer[65536];
    uint64_t size = ntohl(header[1]), offset = 0;
    while (offset < size) {
        size_t count = size-offset > sizeof(buffer) ? sizeof(buffer) : (size_t)(size-offset);
        if (fw_storage_read(storage, region->offset+offset, buffer, count) || fwrite(buffer,1,count,out) != count) break;
        offset += count;
    }
    if (fclose(out) || offset != size) goto done;
    struct fw_fit fit; struct fw_package package;
    ret = fw_fit_open(&fit, path, layout);
    if (ret) goto done;
    ret = fw_fit_verify_package(layout, &fit, &package);
    if (!ret && (package.component != FW_COMPONENT_KERNEL ||
        (slot->version && package.version != slot->version) || package.rollback_index < slot->rollback_index)) ret = -EPERM;
    if (!ret) ret = fw_fit_recovery_capable(&fit);
    fw_fit_close(&fit);
done:
    unlink(path); return ret;
}
