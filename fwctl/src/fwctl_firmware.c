/* SPDX-License-Identifier: GPL-2.0+ */
#define _GNU_SOURCE

#include "fwctl.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/vfs.h>
#include <linux/magic.h>

static int component_from_name(const char *name, uint32_t *component)
{
	unsigned int i;
	for (i = 0; i < DM_FW_COMPONENT_COUNT; i++)
		if (!strcmp(name, dm_fw_component_name(i))) {
			*component = i + 1U;
			return 0;
		}
	return -EINVAL;
}

int fwctl_resolve_update(const struct fwctl_options *options,
                         int argc, char **argv, char *source,
                         size_t source_size, uint32_t *component)
{
	FILE *file;
	char line[512];
	char protocol[16] = "";
	char server[128] = "";
	char path[256] = "";
	char release[128] = "release.itb";
	const char *value;
	const char *argument = NULL;

	if (!options || !source || !source_size || !component || argc < 0 || argc > 2 ||
        (argc && (!argv || !argv[0] || !argv[0][0])) ||
        (argc == 2 && (!argv[1] || !argv[1][0])))
		return -EINVAL;
	*component = DM_FW_UPDATE_COMPONENT_ALL;
	if (argc) {
        int named = !strcmp(argv[0], "all") || !component_from_name(argv[0], component);
        if (argc == 2 && !named) return -EINVAL;
        argument = argc == 2 ? argv[1] : named ? NULL : argv[0];
    }
	if (argument && (strstr(argument, "://") || strchr(argument, '/') ||
	                 strstr(argument, ".itb"))) {
		if (snprintf(source, source_size, "%s", argument) >= (int)source_size)
			return -ENAMETOOLONG;
		return 0;
	}
	if (argument && *argument) {
		if (component_from_name(argument, component))
			return -EINVAL;
	}
	file = fopen(options->config, "r");
	if (!file)
		return -errno;
	while (fgets(line, sizeof(line), file)) {
		char *eq = strchr(line, '=');
		char *v;
		if (!eq) continue;
		*eq = '\0'; v = eq + 1;
		v[strcspn(v, "\r\n")] = '\0';
		if (!strcmp(line, "update-protocol")) snprintf(protocol, sizeof(protocol), "%s", v);
		else if (!strcmp(line, "update-server")) snprintf(server, sizeof(server), "%s", v);
		else if (!strcmp(line, "update-path")) snprintf(path, sizeof(path), "%s", v);
		else if (!strcmp(line, "update-release")) snprintf(release, sizeof(release), "%s", v);
	}
	fclose(file);
	if (!protocol[0] || !server[0])
		return -EINVAL;
	if (strcmp(protocol, "tftp") && strcmp(protocol, "http") && strcmp(protocol, "https"))
		return -EINVAL;
	value = path[0] == '/' ? path + 1 : path;
	if (!strcmp(value, "."))
		value = "";
	if (snprintf(source, source_size, "%s://%s/%s%s%s", protocol, server,
	             value, value[0] ? "/" : "", release) >= (int)source_size)
		return -ENAMETOOLONG;
	return 0;
}

static int parse_u64(const char *value, uint64_t *result)
{
	unsigned long long parsed;
	char *end;

	errno = 0;
	parsed = strtoull(value, &end, 0);
	if (errno || !*value || *end)
		return -EINVAL;
	*result = parsed;
	return 0;
}

static int parse_deployment(const char *value)
{
	uint64_t deployment;

	if (parse_u64(value, &deployment) || deployment >= DM_FW_DEPLOYMENT_COUNT)
		return -EINVAL;
	return (int)deployment;
}

int fwctl_build_open_params(const struct fwctl_options *options,
			    struct dm_fw_open_params *params)
{
	memset(params, 0, sizeof(*params));
	params->config_path = options->config;
	if (!options->config || !options->config[0])
		return -EINVAL;
	return 0;
}

static void print_version(uint32_t version)
{
    printf("%u.%u.%u", version >> 16, (version >> 8) & 0xff,
           version & 0xff);
}

static void print_build_time(uint32_t minutes)
{
	time_t value;
	struct tm tm;

	if (!minutes) {
		printf("unknown");
		return;
	}
	value = (time_t)minutes * 60 + 8 * 60 * 60;
	if (!gmtime_r(&value, &tm)) {
		printf("unknown");
		return;
	}
	printf("%04d-%02d-%02d %02d:%02d +08:00", tm.tm_year + 1900,
	       tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

static void print_status(const struct dm_fw_status *status)
{
	unsigned int i;
	unsigned int j;

	printf("sequence: %" PRIu32 "\n", status->sequence);
	printf("active: %" PRIu32 "  pending: ", status->active_deployment);
	if (status->pending_deployment == DM_FW_SLOT_NONE)
		printf("none");
	else
		printf("%" PRIu32, status->pending_deployment);
	printf("  last-good: %" PRIu32 "  selected: %" PRIu32 "\n",
	       status->last_good_deployment, status->selected_deployment);
	for (i = 0; i < DM_FW_DEPLOYMENT_COUNT; i++) {
		const struct dm_fw_deployment_status *deployment = &status->deployment[i];

		printf("deployment %u: %s successful=%" PRIu32
		       " tries=%" PRIu32 " release=", i,
		       dm_fw_state_name(deployment->state), deployment->successful,
		       deployment->tries_remaining);
		print_version(deployment->release_version);
		printf("\n");
		for (j = 0; j < DM_FW_COMPONENT_COUNT; j++) {
			const struct dm_fw_component_status *component = &deployment->component[j];

			printf("  %-10s slot=%c version=",
			       dm_fw_component_name(component->component),
			       component->slot <= 1 ? 'a' + component->slot : '?');
			print_version(component->version);
			printf(" build-time=");
			print_build_time(component->build_time_minutes);
			printf(" rollback=%" PRIu32 " valid=%" PRIu32 "\n",
			       component->rollback_index, component->valid);
		}
	}
}

/* This immutable rootfs marker is a fallback for blank factory metadata.
 * It identifies the running rootfs image, not a network-loaded kernel.
 */
static int print_rootfs_image_version(void)
{
	FILE *file = fopen("/etc/ota-release", "r");
	char line[256];
	uint64_t version = 0;
	int found = 0;
	int ret = -ENOENT;

	if (!file)
		return -errno;
	while (fgets(line, sizeof(line), file)) {
		const char prefix[] = "FIT_VERSION=";

		if (strncmp(line, prefix, sizeof(prefix) - 1U))
			continue;
		line[strcspn(line, "\r\n")] = '\0';
		if (found || parse_u64(line + sizeof(prefix) - 1U, &version) ||
		    !version || version > UINT32_MAX) {
			ret = -EINVAL;
			goto out;
		}
		found = 1;
	}
	if (ferror(file)) {
		ret = -EIO;
		goto out;
	}
	if (found) {
		printf("[fwctl] [INFO] rootfs image=");
		print_version((uint32_t)version);
		printf(" (from /etc/ota-release)\n");
		ret = 0;
	}
out:
	fclose(file);
	return ret;
}

static int boot_identity_property(const char *key, char *value, size_t capacity)
{
    char path[160];
    snprintf(path, sizeof(path), "/sys/firmware/devicetree/base/chosen/fw,%s", key);
    FILE *f = fopen(path, "rb");
    if (!f) return -errno;
    size_t n = fread(value, 1, capacity, f); int extra = fgetc(f);
    fclose(f);
    if (!n || extra != EOF || value[n-1] || strlen(value) != n-1) return -EINVAL;
    return 0;
}

static int print_boot_identity(const struct fwctl_options *options, const char *stage, char *firmware_hash)
{
    uint32_t booted; char tag[32], id[65], stored_tag[32], stored_id[65];
    struct dm_fw_open_params params;
    int ret = dm_fw_get_booted_deployment(&booted);
    if (ret || booted > 1) return ret ? ret : -EINVAL;
    ret = fwctl_build_open_params(options, &params); if (ret) return ret;
    int handle = dm_fw_open_ext(&params); if (handle < 0) return handle;
    ret = dm_fw_get_identity(handle, booted, stored_tag, stored_id);
    bool legacy_identity = false;
    if (ret == -ENODATA) {
        /* Distinguish a valid legacy record from unreadable/missing metadata. */
        struct dm_fw_status status;
        int status_ret = dm_fw_get_status(handle, &status);
        if (!status_ret)
            legacy_identity = true;
        else
            ret = status_ret;
    }
    dm_fw_close(handle);
    if (ret && !legacy_identity)
        return ret;
    int tag_rc = boot_identity_property("tag", tag, sizeof(tag));
    int id_rc = boot_identity_property("id", id, sizeof(id));
    if (!tag_rc && !id_rc) {
        unsigned char deployment[4];
        FILE *file = fopen("/sys/firmware/devicetree/base/chosen/fw,deployment", "rb");
        if (!file) return -errno;
        size_t count = fread(deployment, 1, sizeof(deployment), file);
        int extra = fgetc(file);
        fclose(file);
        if (count != sizeof(deployment) || extra != EOF) return -EINVAL;
        uint32_t selected = ((uint32_t)deployment[0] << 24) |
                            ((uint32_t)deployment[1] << 16) |
                            ((uint32_t)deployment[2] << 8) | deployment[3];
        if (selected != booted) return -EBADMSG;
        if (tag[0] != 'v' || strspn(tag+1, "0123456789.") != strlen(tag+1) ||
            strlen(id) != 64 || strspn(id, "0123456789abcdef") != 64) return -EINVAL;
        if (!ret && (strcmp(tag, stored_tag) || strcmp(id, stored_id))) {
            fprintf(stderr, "[fwctl] [ERROR] boot identity differs from deployment metadata\n"); return -EBADMSG;
        }
    } else {
        if (tag_rc != -ENOENT || id_rc != -ENOENT) return -EINVAL;
        if (legacy_identity) {
            printf("[fwctl] [INFO] stage=%s identity=unknown deployment=%u (legacy metadata)\n",
                   stage, booted);
            return 0;
        }
        strcpy(tag, stored_tag); strcpy(id, stored_id);
    }
    if (firmware_hash) strcpy(firmware_hash, id);
    printf("[fwctl] [INFO] stage=%s tag=%s id=%.8s deployment=%u\n", stage, tag, id, booted);
    return 0;
}

static int print_running_version(const struct fwctl_options *options)
{
    char firmware_hash[65] = "unknown";
    int identity_ret = print_boot_identity(options, "rootfs", firmware_hash);
    if (identity_ret == -EBADMSG) {
        fprintf(stderr, "[fwctl] [ERROR] inconsistent running firmware identity\n");
        return identity_ret;
    }
	struct dm_fw_open_params params;
	struct dm_fw_status status;
	const struct dm_fw_deployment_status *deployment;
	dm_fw_handle_t handle;
	uint32_t booted;
	unsigned int i;
	int ret;

	/* pending/selected can change after staging an update during this boot. */
	ret = dm_fw_get_booted_deployment(&booted);
	if (ret) {
		printf("[fwctl] [INFO] booted deployment unavailable\n");
		return print_rootfs_image_version() ? ret : 0;
	}
	if (booted >= DM_FW_DEPLOYMENT_COUNT)
		return -ERANGE;
	ret = fwctl_build_open_params(options, &params);
	if (ret)
		return ret;
	handle = dm_fw_open_ext(&params);
	if (handle < 0)
		return handle;
	ret = dm_fw_get_status(handle, &status);
	dm_fw_close(handle);
	if (ret) {
		(void)print_rootfs_image_version();
		return ret;
	}
	deployment = &status.deployment[booted];
	printf("[fwctl] [INFO] running release=");
	if (deployment->release_version)
		print_version(deployment->release_version);
	else
		printf("unknown");
	printf(" hash=%.8s deployment=%" PRIu32 " state=%s successful=%" PRIu32 "\n",
	       firmware_hash, booted, dm_fw_state_name(deployment->state), deployment->successful);
	for (i = DM_FW_COMPONENT_KERNEL; i <= DM_FW_COMPONENT_ROOTFS; i++) {
		const struct dm_fw_component_status *component = &deployment->component[i];

		printf("[fwctl] [INFO] %s slot=%c version=",
		       dm_fw_component_name(i),
		       component->valid && component->slot <= 1 ? 'a' + component->slot : '?');
		if (component->valid && component->version)
			print_version(component->version);
		else
			printf("unknown");
		printf(" hash=%.8s\n", firmware_hash);
	}
	if (!deployment->release_version)
		(void)print_rootfs_image_version();
	return 0;
}

/* Versioned machine interface for the OTA agent. No metadata mutations. */
static int print_ota_status(const struct fwctl_options *options)
{
	struct dm_fw_open_params params;
	struct dm_fw_status status;
	dm_fw_handle_t handle;
	uint32_t booted;
	int ret = dm_fw_get_booted_deployment(&booted);

	if (ret || booted >= DM_FW_DEPLOYMENT_COUNT)
		return ret ? ret : -ERANGE;
	ret = fwctl_build_open_params(options, &params);
	if (ret)
		return ret;
	handle = dm_fw_open_ext(&params);
	if (handle < 0)
		return handle;
	ret = dm_fw_get_status(handle, &status);
	if (ret) {
		dm_fw_close(handle);
		return ret;
	}
	printf("FWOTA2 %u %u %u", booted, status.active_deployment,
	       status.pending_deployment);
	for (unsigned int i = 0; i < DM_FW_DEPLOYMENT_COUNT; i++) {
		printf(" %u %u %u", status.deployment[i].release_version,
		       status.deployment[i].successful, status.deployment[i].state);
		char tag[32], id[65] = "-";
		int identity_ret = dm_fw_get_identity(handle, i, tag, id);
		printf(" %s", identity_ret ? "-" : id);
	}
	printf("\n");
	dm_fw_close(handle);
	return 0;
}

static int check_ota_image(dm_fw_handle_t handle, int argc, char **argv)
{
    uint64_t version, sizes[2];
    uint8_t hashes[2][32];
    const char *digits = "0123456789abcdef";
    uint32_t booted;
    int matches, ret;
    if (argc != 5 || parse_u64(argv[0], &version) || !version || version > UINT32_MAX)
        return -EINVAL;
    for (unsigned i = 0; i < 2; i++) {
        const char *hex = argv[2 + i * 2];
        if (parse_u64(argv[1 + i * 2], &sizes[i]) || !sizes[i] || strlen(hex) != 64)
            return -EINVAL;
        for (unsigned j = 0; j < 32; j++) {
            const char *a = strchr(digits, hex[j * 2]);
            const char *b = strchr(digits, hex[j * 2 + 1]);
            if (!a || !b) return -EINVAL;
            hashes[i][j] = (uint8_t)((a - digits) * 16 + b - digits);
        }
    }
    ret = dm_fw_get_booted_deployment(&booted);
    if (ret) return ret;
    ret = dm_fw_check_image(handle, booted, (uint32_t)version, sizes, hashes, &matches);
    if (!ret) printf("FWCHECK1 %s\n", matches ? "match" : "different");
    return ret;
}

static int recovery_command(dm_fw_handle_t handle, const char *command, int argc, char **argv)
{
    uint32_t booted;
    struct dm_fw_status status;
    struct statfs fs;
    int ret = dm_fw_get_booted_deployment(&booted);
    if (ret || booted > 1) return ret ? ret : -EINVAL;
    if (!strcmp(command, "recovery-status")) {
        if (argc) return -EINVAL;
        ret = dm_fw_get_status(handle, &status);
        if (ret) return ret;
        const struct dm_fw_component_status *root = &status.deployment[booted].component[DM_FW_COMPONENT_ROOTFS];
        if (!root->valid || root->slot > 1) return -EINVAL;
        printf("FWRECOVERY1 %u %u %u\n", booted, root->slot, root->version);
        return 0;
    }
    if (!strcmp(command, "recovery-info-slot") || !strcmp(command, "recovery-check-slot")) {
        uint64_t requested;
        if (argc != 1 || parse_u64(argv[0], &requested) || requested > 1) return -EINVAL;
        uint32_t slot, version;
        char id[65], hex[65];
        uint64_t bytes;
        uint8_t digest[32];
        ret = dm_fw_get_rootfs_recovery_identity(handle, (uint32_t)requested,
            &slot, &version, id, &bytes, digest);
        if (ret) return ret;
        if (!strcmp(command, "recovery-info-slot")) {
            for (unsigned i = 0; i < sizeof(digest); i++)
                snprintf(hex + i * 2, 3, "%02x", digest[i]);
            printf("FWROOTFS1 %llu %u %u %s %llu %s\n",
                (unsigned long long)requested, slot, version, id,
                (unsigned long long)bytes, hex);
            return 0;
        }
        int matches;
        ret = dm_fw_check_rootfs(handle, (uint32_t)requested, &matches);
        if (!ret) printf("FWROOTFSCHECK1 %s\n", matches ? "match" : "different");
        return ret;
    }
    /* Selected-slot repair requires RAM root. Standby repair obtains an
     * exclusive, unmounted partition claim and validates identity in the engine. */
    if (strcmp(command, "recovery-restore-slot") &&
        (statfs("/", &fs) || (fs.f_type != RAMFS_MAGIC && fs.f_type != TMPFS_MAGIC))) return -EPERM;
    if (!strcmp(command, "recovery-fallback"))
        return argc ? -EINVAL : dm_fw_recovery_fallback(handle, booted);
    if (!strcmp(command, "recovery-restore")) {
        uint64_t bytes;
        uint8_t digest[32];
        const char *digits = "0123456789abcdef";
        if (argc != 3 || parse_u64(argv[1], &bytes) || !bytes || strlen(argv[2]) != 64) return -EINVAL;
        for (unsigned i = 0; i < 32; i++) {
            const char *a = strchr(digits, argv[2][2*i]), *b = strchr(digits, argv[2][2*i+1]);
            if (!a || !b) return -EINVAL;
            digest[i] = (uint8_t)((a-digits)*16 + b-digits);
        }
        return dm_fw_restore_rootfs(handle, booted, argv[0], bytes, digest);
    }
    if (!strcmp(command, "recovery-restore-slot")) {
        uint64_t requested, bytes;
        uint8_t digest[32];
        const char *digits = "0123456789abcdef";
        if (argc != 4 || parse_u64(argv[0], &requested) || requested > 1 ||
            parse_u64(argv[2], &bytes) || !bytes || strlen(argv[3]) != 64) return -EINVAL;
        for (unsigned i = 0; i < 32; i++) {
            const char *a = strchr(digits, argv[3][2*i]), *b = strchr(digits, argv[3][2*i+1]);
            if (!a || !b) return -EINVAL;
            digest[i] = (uint8_t)((a-digits)*16 + (b-digits));
        }
        return dm_fw_restore_rootfs_slot(handle, (uint32_t)requested, argv[1], bytes, digest);
    }
    return -EINVAL;
}

static void update_progress(const struct dm_fw_progress *progress, void *userdata)
{
	(void)userdata;
	switch (progress->event) {
	case DM_FW_PROGRESS_STARTED:
        printf("[fwctl] [INFO] Writing release=v");
        print_version(progress->release_version);
        printf(" deployment=%" PRIu32 "; do not power off\n", progress->deployment);
        break;
    case DM_FW_PROGRESS_COMPONENT_STAGED:
        printf("[fwctl] [INFO] Staged component=%s version=v",
               dm_fw_component_name(progress->component));
        print_version(progress->component_version);
        putchar('\n');
        break;
    case DM_FW_PROGRESS_READY:
        printf("[fwctl] [INFO] Ready release=v");
        print_version(progress->release_version);
        printf(" deployment=%" PRIu32 " boot-attempts=%" PRIu32 "\n",
               progress->deployment, progress->attempts);
        puts("[fwctl] [INFO] Reboot and health confirmation are still required");
        break;
	default:
		break;
	}
	/* Progress must be visible while OTA captures stdout through a pipe. */
	fflush(stdout);
}

int fwctl_run_firmware(const struct fwctl_options *options,
		       const char *command, int argc, char **argv)
{
	struct dm_fw_open_params open_params;
	struct dm_fw_update_params update_params;
	struct dm_fw_status status;
	int deployment = 0;
	dm_fw_handle_t handle;
	int ret;

    if (!strcmp(command, "identity")) {
        const char *stage = argc ? argv[0] : "rootfs";
        if (argc > 1 || (strcmp(stage, "rootfs") && strcmp(stage, "initramfs"))) return -EINVAL;
        return print_boot_identity(options, stage, NULL);
    }
	if (!strcmp(command, "ota-status"))
		return argc ? -EINVAL : print_ota_status(options);

	if (!strcmp(command, "version"))
		return argc ? -EINVAL : print_running_version(options);

    char source[512];
    if (!strcmp(command, "update")) {
        memset(&update_params, 0, sizeof(update_params));
        ret = fwctl_resolve_update(options, argc, argv, source, sizeof(source),
                                    &update_params.component);
        if (ret) return ret;
        update_params.source = source;
        update_params.progress = update_progress;
    }

	ret = fwctl_build_open_params(options, &open_params);
	if (ret)
		return ret;
	handle = dm_fw_open_ext(&open_params);
	if (handle < 0)
		return handle;
	if (!strncmp(command, "recovery-", 9)) {
        ret = recovery_command(handle, command, argc, argv);
    } else if (!strcmp(command, "ota-check")) {
        ret = check_ota_image(handle, argc, argv);
    } else if (!strcmp(command, "init")) {
		ret = argc == 1 && strlen(argv[0]) == 1 &&
		      (argv[0][0] == 'a' || argv[0][0] == 'b') ?
		      dm_fw_initialize(handle, argv[0][0] - 'a') : -EINVAL;
	} else if (!strcmp(command, "status")) {
		ret = argc ? -EINVAL : dm_fw_get_status(handle, &status);
		if (!ret)
			print_status(&status);
	} else if (!strcmp(command, "update")) {
        ret = dm_fw_update(handle, &update_params);
	} else if (!strcmp(command, "mark-good")) {
		if (argc > 1)
			ret = -EINVAL;
		else if (argc) {
			deployment = parse_deployment(argv[0]);
			ret = deployment < 0 ? deployment : 0;
		} else {
			uint32_t booted;
			ret = dm_fw_get_booted_deployment(&booted);
			deployment = (int)booted;
		}
		if (!ret)
			ret = dm_fw_mark_good(handle, (uint32_t)deployment);
	} else if (!strcmp(command, "mark-bad")) {
		deployment = argc == 1 ? parse_deployment(argv[0]) : -EINVAL;
		ret = deployment < 0 ? deployment :
			dm_fw_mark_bad(handle, (uint32_t)deployment);
	} else if (!strcmp(command, "rollback")) {
		ret = argc ? -EINVAL : dm_fw_rollback(handle);
	} else {
		ret = -EINVAL;
	}
	dm_fw_close(handle);
	return ret;
}
