// SPDX-License-Identifier: GPL-2.0+
#define _GNU_SOURCE
#include "dm_fw_storage_priv.h"

#include "dm_fw_metadata_priv.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#define FW_IO_CHUNK (64U * 1024U)

int fw_storage_pread_full(struct fw_storage *storage, void *data, size_t size,
			  uint64_t offset)
{
	uint8_t *p = data;
	size_t done = 0;

	while (done < size) {
		ssize_t ret = pread(storage->fd, p + done, size - done,
				    (off_t)(offset + done));

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (!ret)
			return -EIO;
		done += (size_t)ret;
	}
	return 0;
}

int fw_storage_pwrite_full(struct fw_storage *storage, const void *data,
			   size_t size, uint64_t offset)
{
	const uint8_t *p = data;
	size_t done = 0;

	while (done < size) {
		ssize_t ret = pwrite(storage->fd, p + done, size - done,
				     (off_t)(offset + done));

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (!ret)
			return -EIO;
		done += (size_t)ret;
	}
	return 0;
}

int fw_storage_open(struct fw_storage *storage,
			    const struct fw_layout *layout, bool writable)
{
	if (!storage || !layout)
		return -EINVAL;
	memset(storage, 0, sizeof(*storage));
	storage->fd = -1;
	storage->read_fd = -1;
	storage->layout = layout;

	switch (layout->storage_type) {
	case FW_STORAGE_BLOCK:
		return fw_storage_block_open(storage, layout, writable);
	case FW_STORAGE_MTD_NOR:
		return fw_storage_mtd_open(storage, layout, writable);
	default:
		return -EINVAL;
	}
}

void fw_storage_close(struct fw_storage *storage)
{
	if (storage && storage->read_direct) {
		close(storage->read_fd);
		storage->read_fd = -1;
		storage->read_direct = false;
	}
	if (storage && storage->fd >= 0) {
		close(storage->fd);
		storage->fd = -1;
	}
}

int fw_storage_lock(struct fw_storage *storage, bool exclusive)
{
	int operation = exclusive ? LOCK_EX : LOCK_SH;

	if (!storage || storage->fd < 0)
		return -EINVAL;
	if (flock(storage->fd, operation | LOCK_NB) < 0)
		return -errno;
	return 0;
}

int fw_storage_read(struct fw_storage *storage, uint64_t offset,
			void *data, size_t size)
{
	if (!storage || !storage->ops || (!data && size))
		return -EINVAL;
	if (offset > storage->info.capacity ||
	    size > storage->info.capacity - offset)
		return -EFBIG;
	return storage->ops->read(storage, offset, data, size);
}

int fw_storage_write(struct fw_storage *storage, uint64_t offset,
			 const void *data, size_t size)
{
	if (!storage || !storage->ops || (!data && size))
		return -EINVAL;
	if (offset > storage->info.capacity ||
	    size > storage->info.capacity - offset)
		return -EFBIG;
	return storage->ops->write(storage, offset, data, size);
}

int fw_storage_sync(struct fw_storage *storage)
{
	if (!storage || !storage->ops)
		return -EINVAL;
	return storage->ops->sync(storage);
}

int fw_storage_erase(struct fw_storage *storage, uint64_t offset,
			     uint64_t size)
{
	if (!storage || !storage->ops || !storage->ops->erase)
		return -EINVAL;
	if (offset > storage->info.capacity ||
	    size > storage->info.capacity - offset)
		return -EFBIG;
	return storage->ops->erase(storage, offset, size);
}

int fw_storage_read_metadata(struct fw_storage *storage, unsigned int copy,
				     void *data, size_t size)
{
	enum fw_region_id id;

	if (!storage || copy >= FW_METADATA_COPIES || size > FW_METADATA_SIZE)
		return -EINVAL;
	id = copy ? FW_REGION_META1 : FW_REGION_META0;
	return fw_storage_read(storage, storage->layout->region[id].offset,
			       data, size);
}

int fw_storage_write_metadata(struct fw_storage *storage, unsigned int copy,
				      const void *data, size_t size)
{
	enum fw_region_id id;
	int ret;

	if (!storage || copy >= FW_METADATA_COPIES || size > FW_METADATA_SIZE)
		return -EINVAL;
	id = copy ? FW_REGION_META1 : FW_REGION_META0;
	if (storage->info.erase_size) {
		uint64_t offset = storage->layout->region[id].offset;

		if (offset % storage->info.erase_size ||
		    storage->layout->region[id].size < storage->info.erase_size)
			return -EINVAL;
		ret = fw_storage_erase(storage, offset, storage->info.erase_size);
		if (ret)
			return ret;
	}
	ret = fw_storage_write(storage, storage->layout->region[id].offset,
			       data, size);
	return ret ? ret : fw_storage_sync(storage);
}

int fw_storage_write_region(struct fw_storage *storage,
				    const struct fw_region *region,
				    const void *data, size_t size)
{
	const uint8_t *source = data;
	uint8_t *verify;
	size_t offset = 0;
	int ret;

	if (!storage || !region || (!data && size))
		return -EINVAL;
	if (size > region->size)
		return -EFBIG;
	if (storage->info.erase_size) {
		if (region->offset % storage->info.erase_size ||
		    region->size % storage->info.erase_size)
			return -EINVAL;
		ret = fw_storage_erase(storage, region->offset, region->size);
		if (ret)
			return ret;
	}
	while (offset < size) {
		size_t count = size - offset;

		if (count > FW_IO_CHUNK)
			count = FW_IO_CHUNK;
		ret = fw_storage_write(storage, region->offset + offset,
				       source + offset, count);
		if (ret)
			return ret;
		offset += count;
	}
	ret = fw_storage_sync(storage);
	if (ret)
		return ret;
	verify = malloc(FW_IO_CHUNK);
	if (!verify)
		return -ENOMEM;
	offset = 0;
	while (offset < size) {
		size_t count = size - offset;

		if (count > FW_IO_CHUNK)
			count = FW_IO_CHUNK;
		ret = fw_storage_read(storage, region->offset + offset, verify,
				      count);
		if (ret || memcmp(verify, source + offset, count)) {
			if (!ret)
				ret = -EIO;
			free(verify);
			return ret;
		}
		offset += count;
	}
	free(verify);
	return 0;
}
