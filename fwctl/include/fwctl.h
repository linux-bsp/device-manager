/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef FWCTL_H
#define FWCTL_H

#include "dm/firmware.h"

#include <stddef.h>
#include <stdint.h>

struct fwctl_options {
	const char *config;
	int command_index;
};

int fwctl_resolve_update(const struct fwctl_options *options,
                         int argc, char **argv, char *source,
                         size_t source_size, uint32_t *component);

int fwctl_build_open_params(const struct fwctl_options *options,
			    struct dm_fw_open_params *params);
int fwctl_run_firmware(const struct fwctl_options *options,
		       const char *command, int argc, char **argv);
int fwctl_run_health_checks(const struct fwctl_options *options);

#endif /* FWCTL_H */
