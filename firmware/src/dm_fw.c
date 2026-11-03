// SPDX-License-Identifier: GPL-2.0+

#define _GNU_SOURCE

#include "dm_fw_priv.h"
#include "dm_handle.h"

#include "dm_fw_update_priv.h"

#include <errno.h>
#include <openssl/evp.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DM_FW_OPEN_F_VALID 0U

static struct dm_fw_handle dm_fw_handles[DM_FW_MAX_HANDLES];
static pthread_mutex_t dm_fw_handles_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t dm_fw_handles_once = PTHREAD_ONCE_INIT;

static void dm_fw_handles_init(void)
{
	unsigned int i;

	for (i = 0; i < DM_FW_MAX_HANDLES; i++)
		pthread_cond_init(&dm_fw_handles[i].idle, NULL);
	for (i = 0; i < DM_FW_MAX_HANDLES; i++)
		pthread_mutex_init(&dm_fw_handles[i].request_lock, NULL);
}

static struct dm_fw_handle *dm_fw_get_handle_locked(int handle)
{
    uint32_t slot = dm_handle_slot(handle);
    if (slot >= DM_FW_MAX_HANDLES || !dm_fw_handles[slot].used ||
        dm_fw_handles[slot].closing ||
        !dm_handle_matches(handle, DM_HANDLE_FW, slot,
                            dm_fw_handles[slot].generation))
        return NULL;
    return &dm_fw_handles[slot];
}

static int dm_fw_handle_request_lock(int handle, struct dm_fw_handle **out)
{
	struct dm_fw_handle *value;

	if (!out)
		return -EINVAL;
	pthread_once(&dm_fw_handles_once, dm_fw_handles_init);
	pthread_mutex_lock(&dm_fw_handles_lock);
	value = dm_fw_get_handle_locked(handle);
	if (!value) {
		pthread_mutex_unlock(&dm_fw_handles_lock);
		return -EBADF;
	}
	value->inflight++;
	pthread_mutex_unlock(&dm_fw_handles_lock);
	pthread_mutex_lock(&value->request_lock);
	*out = value;
	return 0;
}

static void dm_fw_handle_request_unlock(struct dm_fw_handle *handle)
{
	if (handle) {
		pthread_mutex_unlock(&handle->request_lock);
		pthread_mutex_lock(&dm_fw_handles_lock);
		handle->inflight--;
		if (handle->closing && !handle->inflight)
			pthread_cond_signal(&handle->idle);
		pthread_mutex_unlock(&dm_fw_handles_lock);
	}
}

static int dm_fw_init_layout(struct fw_layout *layout,
			      const struct dm_fw_open_params *params)
{
	const char *config_path;
	int ret;

	if (!layout || !params)
		return -EINVAL;
	if (params->reserved || (params->flags & ~DM_FW_OPEN_F_VALID) ||
	    (params->config_path && !params->config_path[0]))
		return -EINVAL;
	fw_layout_defaults(layout);
	config_path = params->config_path ? params->config_path :
		DM_FW_DEFAULT_CONFIG;
	ret = fw_layout_load(layout, config_path);
	if (ret)
		return ret;
	return fw_layout_validate(layout, UINT64_MAX, 1, 0);
}

int dm_fw_open_ext(const struct dm_fw_open_params *params)
{
	struct fw_layout layout;
	int handle = -EMFILE;
	int ret;

	if (!params)
		return -EINVAL;
	ret = dm_fw_init_layout(&layout, params);
	if (ret)
		return ret;
	pthread_once(&dm_fw_handles_once, dm_fw_handles_init);
	pthread_mutex_lock(&dm_fw_handles_lock);
	for (int i = 0; i < DM_FW_MAX_HANDLES; i++) {
		if (dm_fw_handles[i].used ||
            dm_fw_handles[i].generation == DM_HANDLE_GENERATION_MAX)
			continue;
		dm_fw_handles[i].layout = layout;
		dm_fw_handles[i].used = true;
		dm_fw_handles[i].closing = false;
		dm_fw_handles[i].inflight = 0;
		handle = dm_handle_allocate(DM_HANDLE_FW, i,
            &dm_fw_handles[i].generation);
		break;
	}
	pthread_mutex_unlock(&dm_fw_handles_lock);
	return handle;
}

static void dm_fw_update_progress(const struct dm_fw_progress *progress,
					   void *userdata)
{
	struct dm_fw_update_params *params = userdata;

	if (params->progress)
		params->progress(progress, params->userdata);
}

int dm_fw_open(const char *config_path)
{
	struct dm_fw_open_params params = {
		.config_path = config_path,
	};

	return dm_fw_open_ext(&params);
}

int32_t dm_fw_close(int handle)
{
	struct dm_fw_handle *value;

	pthread_once(&dm_fw_handles_once, dm_fw_handles_init);
	pthread_mutex_lock(&dm_fw_handles_lock);
	value = dm_fw_get_handle_locked(handle);
	if (!value) {
		pthread_mutex_unlock(&dm_fw_handles_lock);
		return -EBADF;
	}
	value->closing = true;
	while (value->inflight)
		pthread_cond_wait(&value->idle, &dm_fw_handles_lock);
	value->used = false;
	value->closing = false;
	pthread_mutex_unlock(&dm_fw_handles_lock);
	return 0;
}

static int dm_fw_storage_open(struct dm_fw_handle *handle,
			       bool writable, struct fw_storage *storage)
{
	int ret;

	ret = fw_storage_open(storage, &handle->layout, writable);
	if (ret)
		return ret;
	ret = fw_layout_validate(&handle->layout, storage->info.capacity,
				 storage->info.write_size,
				 storage->info.erase_size);
	if (!ret)
		ret = fw_storage_lock(storage, writable);
	if (ret) {
		fw_storage_close(storage);
		return ret;
	}
	return 0;
}

static void dm_fw_status_from_metadata(const struct fw_metadata *metadata,
					struct dm_fw_status *status)
{
	unsigned int i;
	unsigned int j;

	memset(status, 0, sizeof(*status));
	status->sequence = metadata->sequence;
	status->active_deployment = metadata->active_deployment;
	status->pending_deployment = metadata->pending_deployment;
	status->last_good_deployment = metadata->last_good_deployment;
	status->boot_once_deployment = metadata->boot_once_deployment;
	status->selected_deployment = metadata->selected_deployment;
	status->update_state = metadata->update_state;
	status->source_copy = metadata->source_copy;
	for (i = 0; i < DM_FW_DEPLOYMENT_COUNT; i++) {
		status->deployment[i].state = metadata->deployment[i].state;
		status->deployment[i].tries_remaining =
			metadata->deployment[i].tries_remaining;
		status->deployment[i].successful = metadata->deployment[i].successful;
		status->deployment[i].release_version =
			metadata->deployment[i].release_version;
		for (j = 0; j < DM_FW_COMPONENT_COUNT; j++) {
			status->deployment[i].component[j].component = j;
			status->deployment[i].component[j].slot =
				metadata->deployment[i].component[j].slot;
			status->deployment[i].component[j].valid =
				metadata->deployment[i].component[j].valid;
			status->deployment[i].component[j].version =
				metadata->deployment[i].component[j].version;
			status->deployment[i].component[j].rollback_index =
				metadata->deployment[i].component[j].rollback_index;
			status->deployment[i].component[j].build_time_minutes =
				metadata->deployment[i].build_time_minutes[j];
		}
	}
}

int32_t dm_fw_get_status(int handle, struct dm_fw_status *status)
{
	struct dm_fw_handle *value;
	struct fw_storage storage = { .fd = -1 };
	struct fw_metadata metadata;
	int ret;

	if (!status)
		return -EINVAL;
	ret = dm_fw_handle_request_lock(handle, &value);
	if (ret)
		return ret;
	ret = dm_fw_storage_open(value, false, &storage);
	if (!ret)
		ret = fw_metadata_load(&storage, &metadata);
	if (!ret)
		dm_fw_status_from_metadata(&metadata, status);
	fw_storage_close(&storage);
	dm_fw_handle_request_unlock(value);
	return ret;
}

int32_t dm_fw_get_identity(int handle, uint32_t deployment, char tag[32], char id[65])
{
    struct dm_fw_handle *value;
    struct fw_storage storage = {.fd = -1}; struct fw_metadata metadata;
    if (deployment > 1 || !tag || !id) return -EINVAL;
    tag[0] = id[0] = 0;
    int ret = dm_fw_handle_request_lock(handle, &value);
    if (ret) return ret;
    ret = dm_fw_storage_open(value, false, &storage);
    if (!ret) ret = fw_metadata_load(&storage, &metadata);
    if (!ret) {
        struct fw_identity derived;
        const struct fw_identity *identity = &metadata.deployment[deployment].identity;
        if (!identity->valid) {
            ret = fw_identity_from_storage(&value->layout, &storage, &metadata, deployment, &derived);
            identity = &derived;
        }
        if (!ret) {
            memcpy(tag, identity->tag, 32);
            for (unsigned i = 0; i < 32; i++) snprintf(id+i*2, 3, "%02x", identity->id[i]);
        }
    }
    fw_storage_close(&storage); dm_fw_handle_request_unlock(value); return ret;
}

int32_t dm_fw_get_rootfs_recovery_identity(int handle, uint32_t deployment,
						   uint32_t *slot, uint32_t *version,
						   char firmware_id[65], uint64_t *bytes,
						   uint8_t payload_sha256[32])
{
	struct dm_fw_handle *value;
	struct fw_storage storage = { .fd = -1 };
	struct fw_metadata metadata;
	int ret;

	if (deployment > 1 || !slot || !version || !firmware_id || !bytes || !payload_sha256)
		return -EINVAL;
	ret = dm_fw_handle_request_lock(handle, &value);
	if (ret)
		return ret;
	ret = dm_fw_storage_open(value, false, &storage);
	if (!ret)
		ret = fw_metadata_load(&storage, &metadata);
	if (!ret) {
		const struct fw_deployment *d = &metadata.deployment[deployment];
		const struct fw_component_slot *root = &d->component[FW_COMPONENT_ROOTFS];
		const struct fw_identity *identity = &d->identity;
		const struct fw_region *region = fw_layout_component_region(
			&value->layout, FW_COMPONENT_ROOTFS, root->slot);
		if (!identity->valid || !root->valid || !region || root->slot > 1 ||
		    !d->release_version || root->version != d->release_version ||
		    !identity->size[1] || identity->size[1] > region->size)
			ret = -ENODATA;
		else {
			*slot = root->slot;
			*version = d->release_version;
			*bytes = identity->size[1];
			memcpy(payload_sha256, identity->hash[1], 32);
			for (unsigned i = 0; i < 32; i++)
				snprintf(firmware_id + i * 2, 3, "%02x", identity->id[i]);
		}
	}
	fw_storage_close(&storage);
	dm_fw_handle_request_unlock(value);
	return ret;
}

int32_t dm_fw_check_rootfs(int handle, uint32_t deployment, int *matches)
{
	struct dm_fw_handle *value;
	struct fw_storage storage = { .fd = -1 };
	struct fw_metadata metadata;
	EVP_MD_CTX *ctx = NULL;
	unsigned char buffer[65536], digest[32];
	int ret;

	if (deployment > 1 || !matches)
		return -EINVAL;
	*matches = 0;
	ret = dm_fw_handle_request_lock(handle, &value);
	if (ret)
		return ret;
	ret = dm_fw_storage_open(value, false, &storage);
	if (!ret)
		ret = fw_metadata_load(&storage, &metadata);
	if (ret)
		goto out;
	const struct fw_deployment *d = &metadata.deployment[deployment];
	const struct fw_component_slot *root = &d->component[FW_COMPONENT_ROOTFS];
	const struct fw_identity *identity = &d->identity;
	const struct fw_region *region = fw_layout_component_region(
		&value->layout, FW_COMPONENT_ROOTFS, root->slot);
	if (!identity->valid || !root->valid || !region || root->slot > 1 ||
	    root->version != d->release_version || !identity->size[1] ||
	    identity->size[1] > region->size) {
		ret = -ENODATA;
		goto out;
	}
	ctx = EVP_MD_CTX_new();
	if (!ctx) { ret = -ENOMEM; goto out; }
	if (!EVP_DigestInit_ex(ctx, EVP_sha256(), NULL)) { ret = -EIO; goto out; }
	for (uint64_t offset = 0; offset < identity->size[1];) {
		size_t count = identity->size[1] - offset > sizeof(buffer) ? sizeof(buffer) :
			(size_t)(identity->size[1] - offset);
		ret = fw_storage_read(&storage, region->offset + offset, buffer, count);
		if (ret)
			goto out;
		if (!EVP_DigestUpdate(ctx, buffer, count)) { ret = -EIO; goto out; }
		offset += count;
	}
	unsigned length;
	if (!EVP_DigestFinal_ex(ctx, digest, &length) || length != 32) { ret = -EIO; goto out; }
	*matches = !memcmp(digest, identity->hash[1], 32);
	ret = 0;
out:
	EVP_MD_CTX_free(ctx);
	fw_storage_close(&storage);
	dm_fw_handle_request_unlock(value);
	return ret;
}

/* Compare the bytes actually installed in the selected deployment. The server
 * supplies payload lengths, never partition capacities or cached-package sizes. */
int32_t dm_fw_check_image(int handle, uint32_t deployment, uint32_t version,
                           const uint64_t sizes[2], const uint8_t hashes[2][32],
                           int *matches)
{
    struct dm_fw_handle *value;
    struct fw_storage storage = { .fd = -1 };
    struct fw_metadata metadata;
    EVP_MD_CTX *ctx = NULL;
    unsigned char buffer[65536], digest[32];
    int ret;

    if (deployment > 1 || !version || !sizes || !hashes || !matches ||
        !sizes[0] || !sizes[1])
        return -EINVAL;
    *matches = 0;
    ret = dm_fw_handle_request_lock(handle, &value);
    if (ret)
        return ret;
    ret = dm_fw_storage_open(value, false, &storage);
    if (!ret)
        ret = fw_metadata_load(&storage, &metadata);
    if (ret || metadata.deployment[deployment].release_version != version)
        goto out;
    ctx = EVP_MD_CTX_new();
    if (!ctx) { ret = -ENOMEM; goto out; }
    for (unsigned i = 0; i < 2; i++) {
        unsigned component = FW_COMPONENT_KERNEL + i;
        const struct fw_component_slot *slot = &metadata.deployment[deployment].component[component];
        const struct fw_region *region = fw_layout_component_region(&value->layout, component, slot->slot);
        if (!slot->valid || !region) { ret = -EINVAL; goto out; }
        if (sizes[i] > region->size) { ret = -EFBIG; goto out; }
        if (!EVP_DigestInit_ex(ctx, EVP_sha256(), NULL)) { ret = -EIO; goto out; }
        for (uint64_t offset = 0; offset < sizes[i];) {
            size_t count = sizes[i] - offset > sizeof(buffer) ? sizeof(buffer) : (size_t)(sizes[i] - offset);
            ret = fw_storage_read(&storage, region->offset + offset, buffer, count);
            if (ret) goto out;
            if (!EVP_DigestUpdate(ctx, buffer, count)) { ret = -EIO; goto out; }
            offset += count;
        }
        unsigned length;
        if (!EVP_DigestFinal_ex(ctx, digest, &length) || length != 32) { ret = -EIO; goto out; }
        if (memcmp(digest, hashes[i], 32)) goto out;
    }
    *matches = 1;
out:
    EVP_MD_CTX_free(ctx);
    fw_storage_close(&storage);
    dm_fw_handle_request_unlock(value);
    return ret;
}

static int32_t dm_fw_mutate(int handle,
			     int (*mutate)(struct fw_metadata *, int32_t,
					   bool *changed),
			     int32_t argument)
{
	struct dm_fw_handle *value;
	struct fw_storage storage = { .fd = -1 };
	struct fw_metadata metadata;
	bool changed = true;
	int ret;

	ret = dm_fw_handle_request_lock(handle, &value);
	if (ret)
		return ret;
	ret = dm_fw_storage_open(value, true, &storage);
	if (!ret)
		ret = fw_metadata_load(&storage, &metadata);
	if (!ret)
		ret = mutate(&metadata, argument, &changed);
	if (!ret && changed)
		ret = fw_metadata_save(&storage, &metadata);
	fw_storage_close(&storage);
	dm_fw_handle_request_unlock(value);
	return ret;
}

static int dm_fw_mark_good_mutate(struct fw_metadata *metadata, int32_t arg,
				   bool *changed)
{
	if (arg >= 0 && arg < (int32_t)DM_FW_DEPLOYMENT_COUNT &&
	    metadata->active_deployment == arg &&
	    metadata->last_good_deployment == arg &&
	    metadata->pending_deployment == FW_SLOT_NONE &&
	    metadata->deployment[arg].state == FW_STATE_CONFIRMED &&
	    metadata->deployment[arg].successful) {
		*changed = false;
		return 0;
	}
	return fw_metadata_mark_good(metadata, arg);
}

static int dm_fw_mark_bad_mutate(struct fw_metadata *metadata, int32_t arg,
				  bool *changed)
{
	(void)changed;
	return fw_metadata_mark_bad(metadata, (uint8_t)arg);
}

static int dm_fw_rollback_mutate(struct fw_metadata *metadata, int32_t arg,
				  bool *changed)
{
	(void)arg;
	(void)changed;
	return fw_metadata_rollback(metadata);
}

int32_t dm_fw_recovery_fallback(int handle, uint32_t failed)
{
    struct dm_fw_handle *value;
    struct fw_storage storage = { .fd = -1 };
    struct fw_metadata metadata;
    if (failed > 1) return -EINVAL;
    int ret = dm_fw_handle_request_lock(handle, &value);
    if (ret) return ret;
    ret = dm_fw_storage_open(value, true, &storage);
    if (!ret) ret = fw_metadata_load(&storage, &metadata);
    if (!ret && metadata.selected_deployment != failed) ret = -EPERM;
    if (!ret) ret = fw_metadata_fallback(&metadata, failed);
    if (!ret) ret = fw_validate_recovery_kernel(&value->layout, &storage, &metadata, 1U-failed);
    if (!ret) ret = fw_metadata_save(&storage, &metadata);
    fw_storage_close(&storage);
    dm_fw_handle_request_unlock(value);
    return ret;
}

int32_t dm_fw_restore_rootfs(int handle, uint32_t deployment,
    const char *path, uint64_t bytes, const uint8_t sha256[32])
{
    struct dm_fw_handle *value;
    struct fw_storage storage = { .fd = -1 };
    int ret = dm_fw_handle_request_lock(handle, &value);
    if (ret) return ret;
    ret = dm_fw_storage_open(value, true, &storage);
    if (!ret) ret = fw_restore_rootfs(&value->layout, &storage, deployment, path, bytes, sha256, false);
    fw_storage_close(&storage);
    dm_fw_handle_request_unlock(value);
    return ret;
}

int32_t dm_fw_restore_rootfs_slot(int handle, uint32_t deployment,
				  const char *path, uint64_t bytes,
				  const uint8_t sha256[32])
{
	struct dm_fw_handle *value;
	struct fw_storage storage = { .fd = -1 };
	int ret;
	if (deployment > 1)
		return -EINVAL;
	ret = dm_fw_handle_request_lock(handle, &value);
	if (ret)
		return ret;
	ret = dm_fw_storage_open(value, true, &storage);
	if (!ret)
		ret = fw_restore_rootfs(&value->layout, &storage, deployment, path, bytes, sha256, true);
	fw_storage_close(&storage);
	dm_fw_handle_request_unlock(value);
	return ret;
}

int32_t dm_fw_initialize(int handle, uint32_t slot)
{
	struct dm_fw_handle *value;
	struct fw_storage storage = { .fd = -1 };
	struct fw_metadata metadata;
	int ret;

	if (slot > 1)
		return -EINVAL;
	ret = dm_fw_handle_request_lock(handle, &value);
	if (ret)
		return ret;
	ret = dm_fw_storage_open(value, true, &storage);
	if (!ret) {
		fw_metadata_init(&metadata, (uint8_t)slot);
		ret = fw_metadata_save(&storage, &metadata);
		if (!ret)
			ret = fw_metadata_save(&storage, &metadata);
	}
	fw_storage_close(&storage);
	dm_fw_handle_request_unlock(value);
	return ret;
}

int32_t dm_fw_mark_good(int handle, int32_t deployment)
{
	return dm_fw_mutate(handle, dm_fw_mark_good_mutate, deployment);
}

int32_t dm_fw_mark_bad(int handle, uint32_t deployment)
{
	if (deployment > 1)
		return -EINVAL;
	return dm_fw_mutate(handle, dm_fw_mark_bad_mutate, deployment);
}

int32_t dm_fw_rollback(int handle)
{
	return dm_fw_mutate(handle, dm_fw_rollback_mutate, 0);
}

int32_t dm_fw_update(int handle,
		      const struct dm_fw_update_params *params)
{
	struct dm_fw_handle *value;
	struct fw_storage storage = { .fd = -1 };
	int ret;

	if (!params || !params->source || !params->source[0] || params->flags ||
	    params->reserved || params->component > DM_FW_COMPONENT_COUNT)
		return -EINVAL;
	ret = dm_fw_handle_request_lock(handle, &value);
	if (ret)
		return ret;
	ret = dm_fw_storage_open(value, true, &storage);
	if (!ret)
		ret = fw_update_release(&value->layout, &storage,
					params->source, params->component,
					dm_fw_update_progress,
					(void *)params);
	fw_storage_close(&storage);
	dm_fw_handle_request_unlock(value);
	return ret;
}

int32_t dm_fw_get_booted_deployment(uint32_t *deployment)
{
	char buffer[2048];
	char *saveptr;
	char *token;
	ssize_t length;
	int fd;

	if (!deployment)
		return -EINVAL;
	fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	length = read(fd, buffer, sizeof(buffer) - 1U);
	close(fd);
	if (length < 0)
		return -errno;
	buffer[length] = '\0';
	for (token = strtok_r(buffer, " \n", &saveptr); token;
	     token = strtok_r(NULL, " \n", &saveptr)) {
		const char prefix[] = "fw.deployment=";
		char *end;
		unsigned long value;

		if (strncmp(token, prefix, sizeof(prefix) - 1U))
			continue;
		errno = 0;
		value = strtoul(token + sizeof(prefix) - 1U, &end, 10);
		if (!errno && *end == '\0' &&
		    value < DM_FW_DEPLOYMENT_COUNT) {
			*deployment = (uint32_t)value;
			return 0;
		}
	}
	return -ENOENT;
}

const char *dm_fw_component_name(enum dm_fw_component component)
{
	return fw_component_name((enum fw_component)component);
}

const char *dm_fw_state_name(enum dm_fw_deployment_state state)
{
	return fw_state_name((enum fw_deployment_state)state);
}
