// SPDX-License-Identifier: GPL-2.0+
#ifndef DM_FW_METADATA_PRIV_H
#define DM_FW_METADATA_PRIV_H

#include <stddef.h>
#include <stdint.h>

struct fw_storage;

#ifndef DM_FW_INTERNAL
#define DM_FW_INTERNAL __attribute__((visibility("hidden")))
#endif

#define FW_METADATA_SIZE 4096U
#define FW_METADATA_COPIES 2U
#define FW_SLOT_NONE 0xffU
#define FW_DEFAULT_ATTEMPTS 3U

enum fw_component {
    FW_COMPONENT_BOOTLOADER,
    FW_COMPONENT_KERNEL,
    FW_COMPONENT_ROOTFS,
    FW_COMPONENT_COUNT,
};

enum fw_deployment_state {
    FW_STATE_EMPTY,
    FW_STATE_WRITING,
    FW_STATE_READY,
    FW_STATE_BOOT_TESTING,
    FW_STATE_CONFIRMED,
    FW_STATE_FAILED,
};

struct fw_component_slot {
    uint8_t slot;
    uint8_t valid;
    uint32_t version;
    uint32_t rollback_index;
};

/* Identity of kernel FIT + rootfs payload; separate from boot selection state. */
struct fw_identity {
    uint32_t valid;
    char tag[32];
    uint8_t id[32];
    uint64_t size[2];
    uint8_t hash[2][32];
};

struct fw_deployment {
    struct fw_identity identity;
    struct fw_component_slot component[FW_COMPONENT_COUNT];
    uint8_t state;
    uint8_t tries_remaining;
    uint8_t successful;
    uint32_t release_version;
    uint32_t build_time_minutes[FW_COMPONENT_COUNT];
};

struct fw_metadata {
    uint32_t sequence;
    uint8_t active_deployment;
    uint8_t pending_deployment;
    uint8_t last_good_deployment;
    uint8_t boot_once_deployment;
    uint8_t selected_deployment;
    uint8_t update_state;
    struct fw_deployment deployment[2];
    uint8_t source_copy;
};

DM_FW_INTERNAL const char *fw_component_name(enum fw_component component);
DM_FW_INTERNAL const char *fw_state_name(enum fw_deployment_state state);
DM_FW_INTERNAL int fw_deployment_is_valid(
    const struct fw_deployment *deployment);
DM_FW_INTERNAL void fw_metadata_init(struct fw_metadata *metadata,
                                      uint8_t slot);
DM_FW_INTERNAL int fw_metadata_load(struct fw_storage *storage,
                                     struct fw_metadata *metadata);
DM_FW_INTERNAL int fw_metadata_save(struct fw_storage *storage,
                                     struct fw_metadata *metadata);
DM_FW_INTERNAL int fw_metadata_begin_release(struct fw_metadata *metadata,
                                              uint32_t release_version,
                                              uint8_t *deployment);
DM_FW_INTERNAL int fw_metadata_stage_component(
    struct fw_metadata *metadata, uint8_t deployment,
    enum fw_component component, uint8_t slot, uint32_t version,
    uint32_t rollback_index, uint32_t build_time_minutes);
DM_FW_INTERNAL int fw_metadata_commit_release(struct fw_metadata *metadata,
                                                uint8_t deployment,
                                                uint8_t attempts);
DM_FW_INTERNAL int fw_metadata_mark_good(struct fw_metadata *metadata,
                                          int deployment);
DM_FW_INTERNAL int fw_metadata_mark_bad(struct fw_metadata *metadata,
                                         uint8_t deployment);
DM_FW_INTERNAL int fw_metadata_fallback(struct fw_metadata *metadata, uint8_t failed);
DM_FW_INTERNAL int fw_metadata_rollback(struct fw_metadata *metadata);

#endif
