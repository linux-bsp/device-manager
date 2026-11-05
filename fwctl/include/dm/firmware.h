// SPDX-License-Identifier: GPL-2.0+
/************************************************************************
 * FWMD RAW A/B firmware update API (two deployments, three components)
 ************************************************************************/

#ifndef DM_FW_API_H
#define DM_FW_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DM_FW_DEFAULT_CONFIG "/etc/device-manager/firmware.conf"
#define DM_FW_DEPLOYMENT_COUNT 2U
#define DM_FW_COMPONENT_COUNT 3U
#define DM_FW_SLOT_NONE 0xffU
#define DM_FW_DEPLOYMENT_CURRENT (-1)

#define DM_FW_UPDATE_COMPONENT_ALL 0U

enum dm_fw_component {
	DM_FW_COMPONENT_BOOTLOADER = 0,
	DM_FW_COMPONENT_KERNEL = 1,
	DM_FW_COMPONENT_ROOTFS = 2,
};

enum dm_fw_deployment_state {
	DM_FW_STATE_EMPTY = 0,
	DM_FW_STATE_WRITING = 1,
	DM_FW_STATE_READY = 2,
	DM_FW_STATE_BOOT_TESTING = 3,
	DM_FW_STATE_CONFIRMED = 4,
	DM_FW_STATE_FAILED = 5,
};

enum dm_fw_progress_event {
	DM_FW_PROGRESS_STARTED = 0,
	DM_FW_PROGRESS_COMPONENT_STAGED = 1,
	DM_FW_PROGRESS_READY = 2,
};

struct dm_fw_open_params {
	const char *config_path;
	uint32_t flags;
	uint32_t reserved;
};

struct dm_fw_component_status {
	uint32_t component;
	uint32_t slot;
	uint32_t valid;
	uint32_t version;
	uint32_t rollback_index;
	uint32_t build_time_minutes;
	uint32_t reserved[2];
};

struct dm_fw_deployment_status {
	uint32_t state;
	uint32_t tries_remaining;
	uint32_t successful;
	uint32_t release_version;
	struct dm_fw_component_status component[DM_FW_COMPONENT_COUNT];
	uint32_t reserved[4];
};

struct dm_fw_status {
	uint32_t sequence;
	uint32_t active_deployment;
	uint32_t pending_deployment;
	uint32_t last_good_deployment;
	uint32_t boot_once_deployment;
	uint32_t selected_deployment;
	uint32_t update_state;
	uint32_t source_copy;
	struct dm_fw_deployment_status deployment[DM_FW_DEPLOYMENT_COUNT];
	uint32_t reserved[8];
};

struct dm_fw_progress {
	enum dm_fw_progress_event event;
	enum dm_fw_component component;
	uint32_t deployment;
	uint32_t release_version;
	uint32_t component_version;
	uint32_t attempts;
	uint32_t reserved[4];
};

typedef void (*dm_fw_progress_fn)(const struct dm_fw_progress *progress,
				   void *userdata);

struct dm_fw_update_params {
	const char *source;
	/* Called synchronously; do not re-enter the same firmware handle. */
	dm_fw_progress_fn progress;
	void *userdata;
	uint32_t flags;
	uint32_t reserved;
	uint32_t component;
};

/* Positive library token, never a POSIX fd. Negative values are -errno.
 * Operations on a handle serialize; close waits for in-flight requests.
 * Callbacks must not re-enter or close the same handle.
 */
typedef int32_t dm_fw_handle_t;

dm_fw_handle_t dm_fw_open(const char *config_path);
dm_fw_handle_t dm_fw_open_ext(const struct dm_fw_open_params *params);
int32_t dm_fw_close(dm_fw_handle_t handle);
int32_t dm_fw_initialize(dm_fw_handle_t handle, uint32_t slot);
/* Compare release version and SHA256 of installed kernel/rootfs payloads.
 * sizes/hashes are ordered kernel, rootfs. Returns 0 for a valid comparison;
 * matches is 1 only when both version and actual storage contents match. */
int32_t dm_fw_check_image(dm_fw_handle_t handle, uint32_t deployment,
                           uint32_t version, const uint64_t sizes[2],
                           const uint8_t hashes[2][32], int *matches);

/* Read stored identity, or reconstruct it from verified kernel/rootfs bytes
 * when a legacy bootloader omitted the extension. No metadata writes; may read
 * complete payloads and use a temporary file in the configured work-dir. */
int32_t dm_fw_get_identity(dm_fw_handle_t handle, uint32_t deployment, char tag[32], char id[65]);
/* Read the metadata-bound rootfs payload identity for either deployment. */
int32_t dm_fw_get_rootfs_recovery_identity(dm_fw_handle_t handle, uint32_t deployment,
						   uint32_t *slot, uint32_t *version,
						   char firmware_id[65], uint64_t *bytes,
						   uint8_t payload_sha256[32]);
/* Compare a deployment's complete rootfs data/hash-tree payload to metadata. */
int32_t dm_fw_check_rootfs(dm_fw_handle_t handle, uint32_t deployment, int *matches);
int32_t dm_fw_get_status(dm_fw_handle_t handle, struct dm_fw_status *status);
int32_t dm_fw_update(dm_fw_handle_t handle,
		      const struct dm_fw_update_params *params);
int32_t dm_fw_mark_good(dm_fw_handle_t handle, int32_t deployment);
int32_t dm_fw_mark_bad(dm_fw_handle_t handle, uint32_t deployment);
/* Recovery environment only: select another confirmed deployment for reboot.
 * No eligible alternate returns -ENOENT without changing metadata. */
int32_t dm_fw_recovery_fallback(dm_fw_handle_t handle, uint32_t failed);
/* Caller must ensure rootfs is not mounted. Restore its exact payload in place,
 * verify readback, and preserve metadata and every other component/slot. */
int32_t dm_fw_restore_rootfs(dm_fw_handle_t handle, uint32_t deployment,
    const char *path, uint64_t bytes, const uint8_t sha256[32]);
/* Initramfs-only explicit repair of an unmounted, non-selected rootfs slot. */
int32_t dm_fw_restore_rootfs_slot(dm_fw_handle_t handle, uint32_t deployment,
				  const char *path, uint64_t bytes,
				  const uint8_t sha256[32]);
int32_t dm_fw_rollback(dm_fw_handle_t handle);
int32_t dm_fw_get_booted_deployment(uint32_t *deployment);

const char *dm_fw_component_name(enum dm_fw_component component);
const char *dm_fw_state_name(enum dm_fw_deployment_state state);

#ifdef __cplusplus
}
#endif

#endif /* DM_FW_API_H */
