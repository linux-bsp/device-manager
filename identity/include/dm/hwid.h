/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef DM_HWID_H
#define DM_HWID_H
#include <stdint.h>
#include "hwid-value.h"

#define DM_HWID_DEFAULT_CONFIG "/etc/fw_env.config"
struct dm_hwid_status {
    char text[DM_HWID_TEXT_SIZE];
};

int dm_hwid_read(const char *config, struct dm_hwid_status *result);
/* Update only the hwid variable in an existing valid U-Boot environment. */
int dm_hwid_set(const char *config, const char *text, struct dm_hwid_status *result);
#endif
