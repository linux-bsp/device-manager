// SPDX-License-Identifier: GPL-2.0+
#ifndef DM_FW_STORAGE_PRIV_H
#define DM_FW_STORAGE_PRIV_H

#include "dm_fw_layout_priv.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct fw_storage;

struct fw_storage_ops {
	int (*read)(struct fw_storage *storage, uint64_t offset,
		    void *data, size_t size);
	int (*write)(struct fw_storage *storage, uint64_t offset,
		     const void *data, size_t size);
	int (*sync)(struct fw_storage *storage);
	int (*erase)(struct fw_storage *storage, uint64_t offset,
		     uint64_t size);
};

struct fw_storage_info {
	uint64_t capacity;
	uint32_t read_size;
	uint32_t write_size;
	uint32_t erase_size;
};

struct fw_storage {
	int fd;
	const struct fw_layout *layout;
	const struct fw_storage_ops *ops;
	struct fw_storage_info info;
};

DM_FW_INTERNAL int fw_storage_pread_full(struct fw_storage *storage,
					  void *data, size_t size,
					  uint64_t offset);
DM_FW_INTERNAL int fw_storage_pwrite_full(struct fw_storage *storage,
					   const void *data, size_t size,
					   uint64_t offset);
DM_FW_INTERNAL int fw_storage_block_open(struct fw_storage *storage,
					  const struct fw_layout *layout,
					  bool writable);
DM_FW_INTERNAL int fw_storage_mtd_open(struct fw_storage *storage,
					const struct fw_layout *layout,
					bool writable);

DM_FW_INTERNAL int fw_storage_open(struct fw_storage *storage,
				    const struct fw_layout *layout,
				    bool writable);
DM_FW_INTERNAL void fw_storage_close(struct fw_storage *storage);
DM_FW_INTERNAL int fw_storage_lock(struct fw_storage *storage,
				    bool exclusive);
DM_FW_INTERNAL int fw_storage_read(struct fw_storage *storage,
				   uint64_t offset, void *data, size_t size);
DM_FW_INTERNAL int fw_storage_write(struct fw_storage *storage,
				    uint64_t offset, const void *data, size_t size);
DM_FW_INTERNAL int fw_storage_sync(struct fw_storage *storage);
DM_FW_INTERNAL int fw_storage_erase(struct fw_storage *storage,
				     uint64_t offset, uint64_t size);
DM_FW_INTERNAL int fw_storage_read_metadata(struct fw_storage *storage,
					     unsigned int copy, void *data,
					     size_t size);
DM_FW_INTERNAL int fw_storage_write_metadata(struct fw_storage *storage,
					      unsigned int copy,
					      const void *data, size_t size);
DM_FW_INTERNAL int fw_storage_write_region(struct fw_storage *storage,
					    const struct fw_region *region,
					    const void *data, size_t size);

#endif
