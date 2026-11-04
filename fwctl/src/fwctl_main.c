/* SPDX-License-Identifier: GPL-2.0+ */

#include "fwctl.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

int fwctl_recovery_main(int argc, char **argv);
int fwctl_run_hwid(int argc, char **argv);

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage: %s [--config path] <command>\n"
		"Commands:\n"
        "  hwid [--config path] [show|value|set HEX16]\n"
		"  init [a|b] (no argument: initramfs PID 1)\n"
		"  recovery [status|fallback|restore|info-slot|check-slot|restore-slot|check-root|table|maintain] ...\n"
		"  status\n"
		"  version\n"
		"  ota-status\n"
        "  identity [rootfs|initramfs]\n"
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
    if (!strcmp(command, "hwid")) {
        ret = fwctl_run_hwid(command_argc, argv + options.command_index + 1);
        if (ret) fprintf(stderr, "[fwctl] [ERROR] hwid failed: %s (%d)\n", strerror(-ret), -ret);
        return ret ? 1 : 0;
    }
    if (!strcmp(command, "init") && !command_argc)
        return fwctl_recovery_main(0, NULL);
    if (!strcmp(command, "recovery")) {
        if (!command_argc) { usage(argv[0]); return 2; }
        const char *action = argv[options.command_index + 1];
        if (!strcmp(action, "maintain")) {
            if (command_argc != 1) return 2;
            return fwctl_recovery_main(1, argv + options.command_index + 1);
        }
        char *args[3];
        if (!strcmp(action, "check-root") || !strcmp(action, "table")) {
            if (command_argc != 3) { usage(argv[0]); return 2; }
            args[0] = (char *)action;
            args[1] = argv[options.command_index + 2];
            args[2] = argv[options.command_index + 3];
            return fwctl_recovery_main(3, args);
        }
        if (strcmp(action, "status") && strcmp(action, "fallback") && strcmp(action, "restore") &&
            strcmp(action, "info-slot") && strcmp(action, "check-slot") &&
            strcmp(action, "restore-slot")) {
            usage(argv[0]); return 2;
        }
        char recovery_command[32];
        snprintf(recovery_command, sizeof(recovery_command), "recovery-%s", action);
        ret = fwctl_run_firmware(&options, recovery_command, command_argc - 1,
                                 argv + options.command_index + 2);
        if (ret) fprintf(stderr, "[fwctl] [ERROR] %s failed: %s (%d)\n",
                         recovery_command, strerror(-ret), -ret);
        return ret ? 1 : 0;
    }
	if (!strcmp(command, "health-check")) {
		if (command_argc) {
			usage(argv[0]);
			return 2;
		}
		ret = fwctl_run_health_checks(&options);
		return ret == -EAGAIN ? 75 : ret ? 1 : 0;
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
