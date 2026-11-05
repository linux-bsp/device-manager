// SPDX-License-Identifier: GPL-2.0+
#define _GNU_SOURCE
#include "dm_fw_storage_priv.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int fw_storage_claim_region(struct fw_storage *storage,
                            const struct fw_region *region, int *claim_fd)
{
    struct stat backing;
    *claim_fd = -1;
    if (fstat(storage->fd, &backing)) return -errno;
    /* Regular files are isolated simulation media, never mounted partitions. */
    if (S_ISREG(backing.st_mode)) return 0;
    if (!S_ISBLK(backing.st_mode)) return -EOPNOTSUPP;
    char base[128];
    snprintf(base, sizeof(base), "/sys/dev/block/%u:%u",
             major(backing.st_rdev), minor(backing.st_rdev));
    DIR *dir = opendir(base);
    if (!dir) return -errno;
    int ret = -ENODEV;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        char path[512];
        unsigned long long start, sectors;
        unsigned maj, min;
        if (entry->d_name[0] == '.' || strchr(entry->d_name, '/')) continue;
        snprintf(path, sizeof(path), "%s/%s/partition", base, entry->d_name);
        if (access(path, F_OK)) continue;
        snprintf(path, sizeof(path), "%s/%s/start", base, entry->d_name);
        FILE *file = fopen(path, "r");
        if (!file) { ret = -errno; break; }
        int parsed = fscanf(file, "%llu", &start); fclose(file);
        if (parsed != 1 || start > UINT64_MAX / 512) { ret = -EINVAL; break; }
        if (start * 512 != region->offset) continue;
        snprintf(path, sizeof(path), "%s/%s/size", base, entry->d_name);
        file = fopen(path, "r");
        if (!file) { ret = -errno; break; }
        parsed = fscanf(file, "%llu", &sectors); fclose(file);
        if (parsed != 1 || sectors > UINT64_MAX / 512 || sectors * 512 != region->size) {
            ret = -EINVAL; break;
        }
        snprintf(path, sizeof(path), "%s/%s/dev", base, entry->d_name);
        file = fopen(path, "r");
        if (!file) { ret = -errno; break; }
        parsed = fscanf(file, "%u:%u", &maj, &min); fclose(file);
        if (parsed != 2) { ret = -EINVAL; break; }
        snprintf(path, sizeof(path), "/dev/%s", entry->d_name);
        /* Kernel block claiming rejects mounted partitions and dm holders and
         * prevents new mounts until the write and readback have completed. */
        int fd = open(path, O_RDONLY|O_EXCL|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
        if (fd < 0) { ret = -errno; break; }
        struct stat partition;
        if (fstat(fd, &partition) || !S_ISBLK(partition.st_mode) ||
            partition.st_rdev != makedev(maj, min)) {
            close(fd); ret = -EINVAL; break;
        }
        *claim_fd = fd; ret = 0; break;
    }
    closedir(dir);
    return ret;
}

static int block_read(struct fw_storage *storage, uint64_t offset,
		      void *data, size_t size)
{
    if (!storage->read_direct || !size)
        return fw_storage_pread_full(storage, data, size, offset);
    size_t sector = storage->info.read_size;
    size_t prefix = offset & (sector - 1);
    if (size > SIZE_MAX - prefix - (sector - 1)) return -EOVERFLOW;
    size_t aligned = (prefix + size + sector - 1) & ~(sector - 1);
    void *buffer;
    int ret = posix_memalign(&buffer, sector, aligned);
    if (ret) return -ret;
    struct fw_storage reader = *storage;
    reader.fd = storage->read_fd;
    ret = fw_storage_pread_full(&reader, buffer, aligned, offset - prefix);
    if (!ret) memcpy(data, (const char *)buffer + prefix, size);
    free(buffer);
    return ret;
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
	unsigned sector = 512;
	if (S_ISBLK(st.st_mode)) {
		if (ioctl(storage->fd, BLKSSZGET, &sector) < 0) {
			ret = -errno;
			goto error;
		}
		if (sector < 512 || sector > 65536 || (sector & (sector - 1)) ||
		    capacity % sector) {
			ret = -EINVAL;
			goto error;
		}
		/* Whole-device and partition page caches are separate. A buffered
		 * read can otherwise report stale bytes after partition-side writes,
		 * and buffered readback can merely re-read the writer's own cache. */
		storage->read_fd = open(layout->device, O_RDONLY|O_CLOEXEC|O_DIRECT);
		if (storage->read_fd < 0) {
			ret = -errno;
			goto error;
		}
		storage->read_direct = true;
	}
	storage->ops = &block_ops;
	storage->info.capacity = capacity;
	storage->info.read_size = sector;
	storage->info.write_size = sector;
	storage->info.erase_size = 0;
	return 0;

error:
	close(storage->fd);
	storage->fd = -1;
	return ret;
}
