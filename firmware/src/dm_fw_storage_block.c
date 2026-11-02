// SPDX-License-Identifier: GPL-2.0+
#include "dm_fw_storage_priv.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

static int block_read(struct fw_storage *storage, uint64_t offset,
		      void *data, size_t size)
{
	return fw_storage_pread_full(storage, data, size, offset);
}

static int block_write(struct fw_storage *storage, uint64_t offset,
		       const void *data, size_t size)
{
	return fw_storage_pwrite_full(storage, data, size, offset);
}

static int block_sync(struct fw_storage *storage)
{
	return fdatasync(storage->fd) < 0 ? -errno : 0;
}

static int block_erase(struct fw_storage *storage, uint64_t offset,
			uint64_t size)
{
	(void)storage;
	(void)offset;
	(void)size;
	return 0;
}

static const struct fw_storage_ops block_ops = {
	.read = block_read,
	.write = block_write,
	.sync = block_sync,
	.erase = block_erase,
};

int fw_storage_block_open(struct fw_storage *storage,
			  const struct fw_layout *layout, bool writable)
{
	struct stat st;
	uint64_t capacity;
	int flags = writable ? O_RDWR : O_RDONLY;
	int ret;

	storage->fd = open(layout->device, flags | O_CLOEXEC);
	if (storage->fd < 0)
		return -errno;
	if (fstat(storage->fd, &st) < 0) {
		ret = -errno;
		goto error;
	}
	if (S_ISREG(st.st_mode)) {
		capacity = (uint64_t)st.st_size;
	} else if (ioctl(storage->fd, BLKGETSIZE64, &capacity) < 0) {
		ret = -errno;
		goto error;
	}
	if (!capacity) {
		ret = -ENOSPC;
		goto error;
	}
	storage->ops = &block_ops;
	storage->info.capacity = capacity;
	storage->info.read_size = 512U;
	storage->info.write_size = 512U;
	storage->info.erase_size = 0;
	return 0;

error:
	close(storage->fd);
	storage->fd = -1;
	return ret;
}
