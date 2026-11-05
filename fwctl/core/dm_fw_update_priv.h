// SPDX-License-Identifier: GPL-2.0+
#ifndef DM_FW_UPDATE_PRIV_H
#define DM_FW_UPDATE_PRIV_H

#include "dm_fw_layout_priv.h"
#include "dm_fw_metadata_priv.h"
#include "dm_fw_storage_priv.h"
#include "dm/firmware.h"

DM_FW_INTERNAL int fw_identity_from_storage(const struct fw_layout *layout, struct fw_storage *storage,
    const struct fw_metadata *metadata, uint32_t deployment, struct fw_identity *identity);
DM_FW_INTERNAL int fw_validate_recovery_kernel(const struct fw_layout *layout,
    struct fw_storage *storage, const struct fw_metadata *metadata, uint32_t deployment);
DM_FW_INTERNAL int fw_restore_rootfs(const struct fw_layout *layout,
    struct fw_storage *storage, uint32_t deployment, const char *path,
    uint64_t bytes, const uint8_t sha256[32], bool allow_unselected);
DM_FW_INTERNAL int fw_update_release(const struct fw_layout *layout,
                                      struct fw_storage *storage,
                                      const char *release_source,
                                      uint32_t component,
                                      dm_fw_progress_fn progress,
                                      void *userdata);

#endif
