/* SPDX-License-Identifier: GPL-2.0+ */

#include "fwctl.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage: %s [--config path] <command>\n"
		"Commands:\n"
		"  init a|b\n"
		"  status\n"
		"  version\n"
		"  ota-status\n"
        "  identity [rootfs|initramfs]\n"
        "  recovery-status | recovery-fallback\n"
        "  recovery-restore ROOTFS_ITB PAYLOAD_BYTES PAYLOAD_SHA256 (initramfs only)\n"
        "  ota-check VERSION KERNEL_BYTES KERNEL_SHA256 ROOTFS_BYTES ROOTFS_SHA256\n"
		"  mark-good [deployment]\n"
		"  mark-bad deployment\n"
		"  rollback\n"
		"  health-check\n"
		"  update [all|bootloader|kernel|rootfs] [release.itb|URL]\n",
		program);
}

static int parse_options(int argc, char **argv, struct fwctl_options *options)
{
	int arg = 1;

	memset(options, 0, sizeof(*options));
	options->config = DM_FW_DEFAULT_CONFIG;
	while (arg < argc && !strncmp(argv[arg], "--", 2)) {
		if (arg + 1 >= argc)
			return -EINVAL;
		if (!strcmp(argv[arg], "--config")) {
			options->config = argv[arg + 1];
		} else
			return -EINVAL;
		arg += 2;
	}
	if (arg >= argc)
		return -EINVAL;
	options->command_index = arg;
	return 0;
}

int main(int argc, char **argv)
{
	struct fwctl_options options;
	const char *command;
	int command_argc;
	int ret;

	ret = parse_options(argc, argv, &options);
	if (ret) {
		usage(argv[0]);
		return 2;
	}
	command = argv[options.command_index];
	command_argc = argc - options.command_index - 1;
	if (!strcmp(command, "health-check")) {
		if (command_argc) {
			usage(argv[0]);
			return 2;
		}
		return fwctl_run_health_checks(&options) ? 1 : 0;
	}
	ret = fwctl_run_firmware(&options, command, command_argc,
				 argv + options.command_index + 1);
	if (ret) {
		fprintf(stderr, "[fwctl] [ERROR] %s failed: %s (%d)\n",
			command, strerror(-ret), -ret);
		if (ret == -EINVAL)
			usage(argv[0]);
	}
    /* Preserve update errno for the OTA daemon; other CLI commands retain their status. */
    if (!strcmp(command, "update") && ret < 0 && -ret <= 125) return -ret;
	return ret ? 1 : 0;
}
