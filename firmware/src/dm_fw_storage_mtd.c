// SPDX-License-Identifier: GPL-2.0+
#include "dm_fw_storage_priv.h"

#include <errno.h>
#include <fcntl.h>
#include <mtd/mtd-user.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int mtd_read(struct fw_storage *storage, uint64_t offset,
		    void *data, size_t size)
{
	return fw_storage_pread_full(storage, data, size, offset);
}

static int mtd_write(struct fw_storage *storage, uint64_t offset,
		     const void *data, size_t size)
{
	return fw_storage_pwrite_full(storage, data, size, offset);
}

static int mtd_sync(struct fw_storage *storage)
{
	return fdatasync(storage->fd) < 0 ? -errno : 0;
}

static int mtd_erase(struct fw_storage *storage, uint64_t offset,
		     uint64_t size)
{
	struct erase_info_user64 erase = {
		.start = offset,
		.length = size,
	};

	if (ioctl(storage->fd, MEMERASE64, &erase) < 0)
		return -errno;
	return 0;
}

static const struct fw_storage_ops mtd_ops = {
	.read = mtd_read,
	.write = mtd_write,
	.sync = mtd_sync,
	.erase = mtd_erase,
};

int fw_storage_mtd_open(struct fw_storage *storage,
			const struct fw_layout *layout, bool writable)
{
	struct mtd_info_user info;
	int flags = writable ? O_RDWR : O_RDONLY;
	int ret;

	storage->fd = open(layout->device, flags | O_CLOEXEC);
	if (storage->fd < 0)
		return -errno;
	if (ioctl(storage->fd, MEMGETINFO, &info) < 0) {
		ret = -errno;
		goto error;
	}
	if (info.type != MTD_NORFLASH || !info.size || !info.writesize ||
	    !info.erasesize || info.erasesize < info.writesize) {
		ret = -ENODEV;
		goto error;
	}
	storage->ops = &mtd_ops;
	storage->info.capacity = info.size;
	storage->info.read_size = info.writesize;
	storage->info.write_size = info.writesize;
	storage->info.erase_size = info.erasesize;
	return 0;

error:
	close(storage->fd);
	storage->fd = -1;
	return ret;
}
