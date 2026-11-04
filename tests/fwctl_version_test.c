/* SPDX-License-Identifier: GPL-2.0+ */
#define _GNU_SOURCE
#include "fwctl.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct dm_fw_status mock_status;
static uint32_t booted;
static int boot_error, status_error, opens, closes, mutations;
static const char *image_info;
static int identity_available, dt_available, dt_mismatch, dt_hash_mismatch;
static const char identity_id[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static const char *identity_tag = "v1.0.0";
static const char *expected_source;
static uint32_t expected_component;
static const char update_config[] = "update-protocol=http\nupdate-server=192.168.10.246:8090\nupdate-path=firmware\nupdate-release=release.itb\n";


FILE *__real_fopen(const char *path, const char *mode);
FILE *__wrap_fopen(const char *path, const char *mode)
{
    if (!strcmp(path, "/test/firmware.conf"))
        return fmemopen((void *)update_config, strlen(update_config), "r");
    if (!strcmp(path, "/etc/ota-release")) {
        if (!image_info) {
            errno = ENOENT;
            return NULL;
        }
        return fmemopen((void *)image_info, strlen(image_info), "r");
    }
    if (!strncmp(path, "/sys/firmware/devicetree/base/chosen/fw,", 40)) {
        if (!dt_available) { errno = ENOENT; return NULL; }
        const char *key = strrchr(path, ',') + 1;
        if (!strcmp(key, "tag")) return fmemopen((void *)identity_tag, strlen(identity_tag) + 1, "r");
        if (!strcmp(key, "id")) {
            static char value[65];
            memcpy(value, identity_id, sizeof(value));
            if (dt_hash_mismatch) value[63] = '0';
            return fmemopen(value, sizeof(value), "r");
        }
        static unsigned char selected[4];
        selected[3] = booted ^ dt_mismatch;
        return fmemopen(selected, sizeof(selected), "r");
    }
    return __real_fopen(path, mode);
}

dm_fw_handle_t dm_fw_open_ext(const struct dm_fw_open_params *params)
{
    assert(!strcmp(params->config_path, "/test/firmware.conf"));
    assert(!params->flags && !params->reserved);
    opens++;
    return 7;
}
int32_t dm_fw_close(dm_fw_handle_t handle)
{
    assert(handle == 7);
    closes++;
    return 0;
}
int32_t dm_fw_check_image(dm_fw_handle_t h, uint32_t d, uint32_t v,
                           const uint64_t sizes[2], const uint8_t hashes[2][32], int *matches)
{
    (void)h; (void)d; (void)v; (void)sizes; (void)hashes; (void)matches;
    return -ENOSYS;
}
int32_t dm_fw_get_identity(dm_fw_handle_t h, uint32_t d, char tag[32], char id[65])
{
    assert(h == 7 && d < DM_FW_DEPLOYMENT_COUNT);
    if (!identity_available || d != booted) return -ENODATA;
    strcpy(tag, "v1.0.0"); strcpy(id, identity_id);
    return 0;
}
int32_t dm_fw_get_rootfs_recovery_identity(dm_fw_handle_t h, uint32_t d, uint32_t *slot,
    uint32_t *version, char id[65], uint64_t *bytes, uint8_t digest[32])
{ (void)h; (void)d; (void)slot; (void)version; (void)id; (void)bytes; (void)digest; return -ENOSYS; }
int32_t dm_fw_check_rootfs(dm_fw_handle_t h, uint32_t d, int *matches)
{ (void)h; (void)d; (void)matches; return -ENOSYS; }
int32_t dm_fw_get_status(dm_fw_handle_t handle, struct dm_fw_status *status)
{
    assert(handle == 7);
    if (!status_error)
        *status = mock_status;
    return status_error;
}
int32_t dm_fw_get_booted_deployment(uint32_t *deployment)
{
    if (!boot_error)
        *deployment = booted;
    return boot_error;
}
const char *dm_fw_component_name(enum dm_fw_component component)
{
    static const char *names[] = {"bootloader", "kernel", "rootfs"};
    assert(component < DM_FW_COMPONENT_COUNT);
    return names[component];
}
const char *dm_fw_state_name(enum dm_fw_deployment_state state)
{
    static const char *names[] = {"empty", "writing", "ready", "boot-testing", "confirmed", "failed"};
    assert(state <= DM_FW_STATE_FAILED);
    return names[state];
}
int32_t dm_fw_initialize(dm_fw_handle_t h, uint32_t slot)
{
    (void)h; (void)slot; mutations++; return -ENOSYS;
}
int32_t dm_fw_update(dm_fw_handle_t h, const struct dm_fw_update_params *params)
{
    assert(h == 7 && expected_source && !strcmp(params->source, expected_source));
    assert(params->component == expected_component && params->progress);
    mutations++; return -ENOSYS;
}
int32_t dm_fw_mark_good(dm_fw_handle_t h, int32_t deployment)
{
    (void)h; (void)deployment; mutations++; return -ENOSYS;
}
int32_t dm_fw_mark_bad(dm_fw_handle_t h, uint32_t deployment)
{
    (void)h; (void)deployment; mutations++; return -ENOSYS;
}
int32_t dm_fw_recovery_fallback(dm_fw_handle_t h, uint32_t d)
{ (void)h; (void)d; return -ENOSYS; }
int32_t dm_fw_restore_rootfs(dm_fw_handle_t h, uint32_t d, const char *p, uint64_t n, const uint8_t hash[32])
{ (void)h; (void)d; (void)p; (void)n; (void)hash; return -ENOSYS; }
int32_t dm_fw_restore_rootfs_slot(dm_fw_handle_t h, uint32_t d, const char *p, uint64_t n, const uint8_t hash[32])
{ (void)h; (void)d; (void)p; (void)n; (void)hash; return -ENOSYS; }
int32_t dm_fw_rollback(dm_fw_handle_t h)
{
    (void)h; mutations++; return -ENOSYS;
}

static const char *tested_command = "version";

static int capture(char text[4096], int argc)
{
    struct fwctl_options options = {.config = "/test/firmware.conf"};
    FILE *file = tmpfile();
    assert(file);
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    assert(saved >= 0 && dup2(fileno(file), STDOUT_FILENO) >= 0);
    int ret = fwctl_run_firmware(&options, tested_command, argc, NULL);
    fflush(stdout);
    assert(lseek(fileno(file), 0, SEEK_SET) == 0);
    ssize_t count = read(fileno(file), text, 4095);
    assert(count >= 0);
    text[count] = 0;
    assert(dup2(saved, STDOUT_FILENO) >= 0);
    close(saved);
    fclose(file);
    assert(!mutations && opens == closes);
    return ret;
}

int main(void)
{
    char text[4096];
    mock_status.pending_deployment = 1;
    mock_status.selected_deployment = 1;
    mock_status.active_deployment = 0;
    for (unsigned i = 0; i < 2; i++) {
        struct dm_fw_deployment_status *d = &mock_status.deployment[i];
        d->release_version = 0x10000 + i;
        d->state = i ? DM_FW_STATE_BOOT_TESTING : DM_FW_STATE_CONFIRMED;
        d->successful = !i;
        for (unsigned j = 0; j < DM_FW_COMPONENT_COUNT; j++) {
            d->component[j].component = j;
            d->component[j].slot = i;
            d->component[j].valid = 1;
            d->component[j].version = d->release_version;
        }
    }
    tested_command = "ota-status";
    assert(!capture(text, 0));
    assert(!strcmp(text, "FWOTA2 0 0 1 65536 1 4 - 65537 0 3 -\n"));
    booted = 1;
    assert(!capture(text, 0));
    assert(!strcmp(text, "FWOTA2 1 0 1 65536 1 4 - 65537 0 3 -\n"));
    boot_error = -ENOENT;
    assert(capture(text, 0) == -ENOENT && !*text);
    boot_error = 0;
    booted = 0;
    assert(capture(text, 1) == -EINVAL);
    tested_command = "version";
    /* Staging new metadata must not report the pending/selected image as running. */
    assert(!capture(text, 0));
    assert(strstr(text, "[fwctl] [INFO] running release="));
    assert(!strstr(text, "===="));
    assert(strstr(text, "running release=1.0.0 hash=unknown deployment=0"));
    assert(strstr(text, "kernel slot=a version=1.0.0"));
    assert(!strstr(text, "1.0.1"));

    /* Small packed values are patch/minor versions, not legacy major versions. */
    struct dm_fw_deployment_status saved = mock_status.deployment[0];
    mock_status.deployment[0].release_version = 1;
    mock_status.deployment[0].component[DM_FW_COMPONENT_KERNEL].version = 1;
    mock_status.deployment[0].component[DM_FW_COMPONENT_ROOTFS].version = 1;
    assert(!capture(text, 0));
    assert(strstr(text, "running release=0.0.1 hash=unknown deployment=0"));
    assert(strstr(text, "kernel slot=a version=0.0.1"));
    assert(strstr(text, "rootfs slot=a version=0.0.1"));
    mock_status.deployment[0].release_version = 0x100;
    assert(!capture(text, 0));
    assert(strstr(text, "running release=0.1.0 hash=unknown deployment=0"));
    mock_status.deployment[0] = saved;

    /* The next boot is already running the new deployment before confirmation. */
    booted = 1;
    assert(!capture(text, 0));
    assert(strstr(text, "release=1.0.1 hash=unknown deployment=1 state=boot-testing successful=0"));
    assert(strstr(text, "rootfs slot=b version=1.0.1"));

    /* Booting the old deployment again reports its old version. */
    booted = 0;
    assert(!capture(text, 0));
    assert(strstr(text, "release=1.0.0") && !strstr(text, "1.0.1"));

    /* Blank metadata is unknown; the optional image fallback is explicitly labelled. */
    memset(&mock_status.deployment[0], 0, sizeof(mock_status.deployment[0]));
    image_info = "VERSION=1.0.0\nFIT_VERSION=65536\n";
    assert(!capture(text, 0));
    assert(strstr(text, "running release=unknown"));
    assert(strstr(text, "rootfs image=1.0.0 (from /etc/ota-release)"));
    assert(strstr(text, "kernel slot=? version=unknown"));

    /* Network boot must not guess a deployment from active or pending metadata. */
    boot_error = -ENOENT;
    int old_opens = opens;
    assert(!capture(text, 0));
    assert(opens == old_opens);
    assert(strstr(text, "booted deployment unavailable") && strstr(text, "rootfs image=1.0.0"));
    assert(!strstr(text, "running release="));
    image_info = NULL;
    assert(capture(text, 0) == -ENOENT);
    image_info = "FIT_VERSION=4294967296\n";
    assert(capture(text, 0) == -ENOENT && !strstr(text, "rootfs image="));
    image_info = "FIT_VERSION=65536\nFIT_VERSION=65537\n";
    assert(capture(text, 0) == -ENOENT && !strstr(text, "rootfs image="));

    boot_error = 0;
    booted = DM_FW_DEPLOYMENT_COUNT;
    assert(capture(text, 0) == -ERANGE);
    booted = 0;
    status_error = -EIO;
    image_info = "FIT_VERSION=65538\n";
    assert(capture(text, 0) == -EIO);
    assert(strstr(text, "rootfs image=1.0.2") && !strstr(text, "running release="));
    assert(capture(text, 1) == -EINVAL);
    status_error = 0;
    tested_command = "identity";
    identity_available = 1;
    assert(!capture(text, 0));
    assert(strstr(text, "id=01234567 deployment=") && !strstr(text, identity_id) && strstr(text, "tag=v1.0.0"));
    tested_command = "version";
    mock_status.deployment[0] = saved;
    assert(!capture(text, 0));
    assert(strstr(text, "release=1.0.0 hash=01234567 deployment=0"));
    assert(strstr(text, "kernel slot=a version=1.0.0 hash=01234567"));
    assert(strstr(text, "rootfs slot=a version=1.0.0 hash=01234567"));
    assert(!strstr(text, identity_id));
    tested_command = "identity";
    dt_available = 1;
    assert(!capture(text, 0)); /* 64 hex characters plus NUL must fit. */
    dt_hash_mismatch = 1;
    assert(capture(text, 0) == -EBADMSG); /* Same prefix must not pass full-ID comparison. */
    dt_hash_mismatch = 0;
    dt_mismatch = 1;
    assert(capture(text, 0) == -EBADMSG);
    tested_command = "version";
    assert(capture(text, 0) == -EBADMSG);
    tested_command = "identity";
    dt_mismatch = 0;
    identity_tag = "v1.0.1";
    assert(capture(text, 0) == -EBADMSG);
    identity_tag = "v1.0.0";
    dt_available = identity_available = 0;
    assert(!capture(text, 0) && strstr(text, "identity=unknown") && strstr(text, "legacy metadata"));
    status_error = -ENODATA;
    assert(capture(text, 0) == -ENODATA && !*text);
    status_error = -EIO;
    assert(capture(text, 0) == -EIO && !*text);
    struct fwctl_options options = {.config = "/test/firmware.conf"};
    char *component_source[] = {"kernel", "/cache/release.itb"};
    char *all_source[] = {"all", "https://example.test/release.itb"};
    char *source_only[] = {"./release.itb"};
    char *component_only[] = {"rootfs"};
    struct { int argc; char **argv; const char *source; uint32_t component; } cases[] = {
        {2, component_source, "/cache/release.itb", DM_FW_COMPONENT_KERNEL + 1},
        {2, all_source, "https://example.test/release.itb", DM_FW_UPDATE_COMPONENT_ALL},
        {1, source_only, "./release.itb", DM_FW_UPDATE_COMPONENT_ALL},
        {1, component_only, "http://192.168.10.246:8090/firmware/release.itb", DM_FW_COMPONENT_ROOTFS + 1},
        {0, NULL, "http://192.168.10.246:8090/firmware/release.itb", DM_FW_UPDATE_COMPONENT_ALL},
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        expected_source = cases[i].source; expected_component = cases[i].component;
        int old_mutations = mutations;
        assert(fwctl_run_firmware(&options, "update", cases[i].argc, cases[i].argv) == -ENOSYS);
        assert(mutations == old_mutations + 1 && opens == closes);
    }
    old_opens = opens;
    int old_mutations = mutations;
    char *unknown[] = {"kernal", "/cache/release.itb"};
    char *empty[] = {"kernel", ""};
    char *three[] = {"all", "release.itb", "ignored"};
    assert(fwctl_run_firmware(&options, "update", 2, unknown) == -EINVAL);
    assert(fwctl_run_firmware(&options, "update", 2, empty) == -EINVAL);
    assert(fwctl_run_firmware(&options, "update", 3, three) == -EINVAL);
    assert(opens == old_opens && mutations == old_mutations);
    char *factory[] = {"b", "--force"};
    char *no_force[] = {"b"};
    assert(fwctl_run_firmware(&options, "factory-init", 1, no_force) == -EINVAL);
    assert(fwctl_run_firmware(&options, "init", 1, no_force) == -EINVAL);
    assert(opens == old_opens && mutations == old_mutations);
    assert(fwctl_run_firmware(&options, "factory-init", 2, factory) == -ENOSYS);
    assert(mutations == old_mutations + 1 && opens == closes);
    puts("fwctl: running identity and update argument/routing checks passed");
    return 0;
}
