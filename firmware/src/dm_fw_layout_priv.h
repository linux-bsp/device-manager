// SPDX-License-Identifier: GPL-2.0+
#ifndef DM_FW_LAYOUT_PRIV_H
#define DM_FW_LAYOUT_PRIV_H

#include <stddef.h>
#include <stdint.h>

#define DM_FW_INTERNAL __attribute__((visibility("hidden")))

#define FW_PATH_SIZE 256U
#define FW_ID_SIZE 64U

enum fw_region_id {
    FW_REGION_META0,
    FW_REGION_META1,
    FW_REGION_TIBOOT3_A,
    FW_REGION_TIBOOT3_B,
    FW_REGION_TISPL_A,
    FW_REGION_TISPL_B,
    FW_REGION_UBOOT_A,
    FW_REGION_UBOOT_B,
    FW_REGION_KERNEL_A,
    FW_REGION_KERNEL_B,
    FW_REGION_ROOTFS_A,
    FW_REGION_ROOTFS_B,
	FW_REGION_COUNT,
};

enum fw_storage_type {
	FW_STORAGE_BLOCK = 0,
	FW_STORAGE_MTD_NOR,
};

struct fw_region {
    const char *name;
    uint64_t offset;
    uint64_t size;
};

struct fw_layout {
    char device[FW_PATH_SIZE];
    char signature_key[FW_PATH_SIZE];
    char work_dir[FW_PATH_SIZE];
    char hwid_config[FW_PATH_SIZE];
	uint8_t attempts;
	int allow_bootloader_update;
	int require_signature;
	enum fw_storage_type storage_type;
	struct fw_region region[FW_REGION_COUNT];
};

DM_FW_INTERNAL void fw_layout_defaults(struct fw_layout *layout);
DM_FW_INTERNAL int fw_layout_load(struct fw_layout *layout, const char *path);
DM_FW_INTERNAL int fw_layout_validate(const struct fw_layout *layout,
	                                       uint64_t capacity,
	                                       uint32_t write_size,
	                                       uint32_t erase_size);
DM_FW_INTERNAL const struct fw_region *fw_layout_component_region(
    const struct fw_layout *layout, unsigned int component, uint8_t slot);

#endif
