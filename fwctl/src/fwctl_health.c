/* SPDX-License-Identifier: GPL-2.0+ */

#include "fwctl.h"
#include "peripheral_ipc.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct health_check {
	const char *name;
	int (*run)(const struct fwctl_options *options);
};

static int health_report(const char *message, const char *value)
{
	fprintf(stderr, "[fwctl] [ERROR] health-check: ");
	fprintf(stderr, message, value);
	fputc('\n', stderr);
	return -EIO;
}

static int health_pending(const char *message, const char *value)
{
	fprintf(stderr, "[fwctl] [WARN] health-check: ");
	fprintf(stderr, message, value);
	fputc('\n', stderr);
	return -EAGAIN;
}

static int health_check_boot(const struct fwctl_options *options)
{
	struct stat st;

	(void)options;
	if (stat("/run/rcS.critical", &st) == 0 && st.st_size > 0)
		return health_report("a critical boot service failed: %s", "/run/rcS.critical");
	return 0;
}

static int health_check_markers(const struct fwctl_options *options)
{
	(void)options;
	if (access("/run/fw-rollback-test.active", F_OK) == 0)
		return health_report("failure marker is active: %s",
				     "/run/fw-rollback-test.active");
	return 0;
}

static int health_check_pid(const char *name, const char *path)
{
	FILE *file;
	char buffer[32];
	char *end;
	long pid;

	file = fopen(path, "r");
	if (!file)
		return errno == ENOENT ? health_pending("%s pid file is not ready", name) :
		       health_report("%s pid file is unreadable", name);
	if (!fgets(buffer, sizeof(buffer), file)) {
		fclose(file);
		return health_report("%s pid file is invalid", name);
	}
	fclose(file);
	errno = 0;
	pid = strtol(buffer, &end, 10);
	if (errno || end == buffer || pid <= 0 || pid > INT_MAX)
		return health_report("%s pid file is invalid", name);
	if (kill((pid_t)pid, 0) < 0 && errno != EPERM)
		return health_pending("%s is not running yet", name);
	return 0;
}

static int health_check_pids(const struct fwctl_options *options)
{
	(void)options;
	return health_check_pid("system_manager", "/run/system_manager.pid");
}

static const char *service_name(uint16_t service_id)
{
	if (service_id == OSA_IPC_SERVICE_PTELEMETRYD)
		return "ptelemetryd";
	if (service_id == OSA_IPC_SERVICE_PSATELLITED)
		return "psatellited";
	return "unknown";
}

static int health_check_manager(const struct fwctl_options *options)
{
	struct osa_ipc_message message;
	struct osa_ipc_health_status status;
	size_t offset;
	int critical_seen = 0;
	int fd;
	int ret;

	(void)options;
	fd = osa_ipc_connect(OSA_IPC_DEFAULT_SOCKET, 1000);
	if (fd < 0)
		return health_pending("system_manager IPC is not ready: %s",
				     strerror(-fd));
	ret = osa_ipc_send(fd, OSA_IPC_MESSAGE_HEALTH_QUERY,
			   OSA_IPC_SERVICE_SYSTEM_MANAGER, 0U, 0U, 0U, 0,
			   NULL, 0U);
	if (!ret)
		ret = osa_ipc_receive(fd, &message, 1000);
	close(fd);
	if (ret < 0)
		return health_pending("system_manager health query not ready: %s",
				     strerror(-ret));
	if (message.header.message_type != OSA_IPC_MESSAGE_HEALTH_SNAPSHOT ||
	    message.header.service_id != OSA_IPC_SERVICE_SYSTEM_MANAGER ||
	    message.header.payload_length > sizeof(message.payload) ||
	    message.header.payload_length % sizeof(status))
		return health_report("system_manager returned invalid health data: %s",
				     "protocol error");
	for (offset = 0; offset < message.header.payload_length;
	     offset += sizeof(status)) {
        memcpy(&status, message.payload + offset, sizeof(status));
        if (status.state > OSA_IPC_HEALTH_FAILED)
            return health_report("invalid service health state: %s", "protocol error");
        if (status.service_id == OSA_IPC_SERVICE_PSATELLITED) {
            if (critical_seen++ || !(status.flags & OSA_IPC_HEALTH_FLAG_REQUIRED))
                return health_report("critical service health is inconsistent: %s", "psatellited");
        }
		if ((status.flags & OSA_IPC_HEALTH_FLAG_REQUIRED) &&
		    (status.state == OSA_IPC_HEALTH_PENDING ||
		     status.state == OSA_IPC_HEALTH_RESTARTING))
			return health_pending("required service is not ready: %s",
					     service_name(status.service_id));
		if ((status.flags & OSA_IPC_HEALTH_FLAG_REQUIRED) &&
		    (status.state == OSA_IPC_HEALTH_EXPIRED ||
		     status.state == OSA_IPC_HEALTH_FAILED))
			return health_report("service is unhealthy: %s",
					     service_name(status.service_id));
	}
	return critical_seen ? 0 : health_report("critical service missing from health snapshot: %s", "psatellited");
}

static int health_check_executables(const struct fwctl_options *options)
{
	static const char *const paths[] = {
		"/usr/sbin/system_manager", "/usr/sbin/fwctl",
	};
	size_t i;

	(void)options;
	for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
		if (access(paths[i], X_OK) < 0)
			return health_report("%s is not executable", paths[i]);
	return 0;
}

static int health_check_sysfs(const struct fwctl_options *options)
{
	static const char *const paths[] = {
		"/sys/module/pdm", "/sys/class/net/can0", "/sys/class/net/can1",
	};
	struct stat st;
	size_t i;

	(void)options;
	for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		int ret = stat(paths[i], &st);
		if (ret < 0 && errno == ENOENT)
			return health_pending("%s is not ready", paths[i]);
		else if (ret < 0 || !S_ISDIR(st.st_mode))
			return health_report("%s is not available", paths[i]);
	}
	return 0;
}

static int health_check_devices(const struct fwctl_options *options)
{
	static const char *const paths[] = {
		"/dev/watchdog0", "/dev/pdm_manager",
	};
	struct stat st;
	size_t i;

	(void)options;
	for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		int ret = stat(paths[i], &st);
		if (ret < 0 && errno == ENOENT)
			return health_pending("%s is not ready", paths[i]);
		else if (ret < 0 || !S_ISCHR(st.st_mode))
			return health_report("%s is not a character device", paths[i]);
	}
	return 0;
}

static int health_check_fw_status(const struct fwctl_options *options)
{
	struct dm_fw_open_params open_params;
	struct dm_fw_status status;
	dm_fw_handle_t handle;
	int ret;

	ret = fwctl_build_open_params(options, &open_params);
	if (ret)
		return health_report("invalid firmware configuration: %s",
				     strerror(-ret));
	handle = dm_fw_open_ext(&open_params);
	if (handle < 0)
		return health_report("firmware status is unavailable: %s",
				     strerror(-handle));
	ret = dm_fw_get_status(handle, &status);
	dm_fw_close(handle);
	if (ret)
		return health_report("firmware status failed: %s", strerror(-ret));
	return 0;
}

static const struct health_check health_checks[] = {
	{ "boot-failures", health_check_boot },
	{ "markers", health_check_markers },
	{ "pidfiles", health_check_pids },
	{ "system-manager", health_check_manager },
	{ "executables", health_check_executables },
	{ "sysfs", health_check_sysfs },
	{ "char-devices", health_check_devices },
	{ "fwctl", health_check_fw_status },
};

int fwctl_run_health_checks(const struct fwctl_options *options)
{
	size_t i;
	int pending = 0;

	for (i = 0; i < sizeof(health_checks) / sizeof(health_checks[0]); i++) {
		int ret = health_checks[i].run(options);
		if (ret == -EAGAIN)
			pending = 1;
		else if (ret)
			return ret;
	}
	return pending ? -EAGAIN : 0;
}
