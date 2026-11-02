// SPDX-License-Identifier: GPL-2.0+
#ifndef DM_FW_FIT_PRIV_H
#define DM_FW_FIT_PRIV_H

#include "dm_fw_layout_priv.h"
#include "dm_fw_metadata_priv.h"

#include <stddef.h>
#include <stdint.h>

#define FW_SHA256_SIZE 32U
#define FW_RELEASE_PATH_SIZE 128U

struct fw_fit {
    int fd;
    void *data;
    size_t mapped_size;
    size_t fit_size;
    int configuration;
    char configuration_name[FW_ID_SIZE];
    char path[FW_PATH_SIZE];
};

struct fw_package {
    enum fw_component component;
    uint32_t version;
    uint32_t rollback_index;
    uint32_t build_time_minutes;
};

struct fw_release_component {
    enum fw_component component;
    char file[FW_RELEASE_PATH_SIZE];
    uint8_t sha256[FW_SHA256_SIZE];
};

struct fw_release {
    struct fw_identity identity;
    uint32_t version;
    uint32_t rollback_index;
    int allow_bootloader;
    unsigned int count;
    struct fw_release_component component[FW_COMPONENT_COUNT];
};

DM_FW_INTERNAL int fw_fit_open(struct fw_fit *fit, const char *path,
                                const struct fw_layout *layout);
DM_FW_INTERNAL void fw_fit_close(struct fw_fit *fit);
DM_FW_INTERNAL int fw_fit_verify_package(const struct fw_layout *layout,
                                          const struct fw_fit *fit,
                                          struct fw_package *package);
DM_FW_INTERNAL int fw_fit_verify_release(const struct fw_layout *layout,
                                          const struct fw_fit *fit,
                                          struct fw_release *release);
DM_FW_INTERNAL int fw_fit_get_component_data(
    const struct fw_fit *fit, enum fw_component component,
    const char *image_name, const void **data, size_t *size);
DM_FW_INTERNAL int fw_fit_check_identity(const struct fw_fit *fit, enum fw_component component, const struct fw_identity *identity);
DM_FW_INTERNAL int fw_fit_rootfs_identity(const struct fw_fit *fit, uint64_t *bytes, uint8_t hash[32]);
DM_FW_INTERNAL int fw_fit_recovery_capable(const struct fw_fit *fit);
DM_FW_INTERNAL int fw_sha256_file(const char *path,
                                   uint8_t digest[FW_SHA256_SIZE]);

#endif
