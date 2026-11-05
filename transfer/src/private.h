// SPDX-License-Identifier: GPL-2.0+
#ifndef DM_TRANSFER_PRIVATE_H
#define DM_TRANSFER_PRIVATE_H
#include "dm/transfer.h"
#include <stdint.h>
#define DM_TRANSFER_PATH 4096
struct dm_transfer_checkpoint {
    char sha256[65], prefix[65];
    uint64_t size, offset;
};
int dm_transfer_checkpoint_read(const char *path, struct dm_transfer_checkpoint *checkpoint);
int dm_transfer_checkpoint_write(const char *path, const struct dm_transfer_checkpoint *checkpoint);
int dm_transfer_write_all(int fd, const void *data, size_t length);
int dm_transfer_cancelled(const struct dm_transfer_callbacks *callbacks);
struct evp_md_ctx_st;
int dm_transfer_hash_finish(struct evp_md_ctx_st *context,char hex[65]);
int dm_transfer_hash_prefix(int fd,uint64_t length,struct evp_md_ctx_st *context,
                              const struct dm_transfer_callbacks *callbacks);
#endif
