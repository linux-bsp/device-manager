// SPDX-License-Identifier: GPL-2.0+

#ifndef DM_FW_PRIV_H
#define DM_FW_PRIV_H

#include "dm_fw_layout_priv.h"
#include "dm_fw_metadata_priv.h"
#include "dm_fw_storage_priv.h"
#include "dm/firmware.h"

#include <pthread.h>
#include <stdbool.h>

#define DM_FW_MAX_HANDLES 8

struct dm_fw_handle {
	uint32_t generation;
	bool used;
	bool closing;
	unsigned int inflight;
	struct fw_layout layout;
	pthread_mutex_t request_lock;
	pthread_cond_t idle;
};

#endif
