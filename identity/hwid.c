/* SPDX-License-Identifier: GPL-2.0+ */
#define _GNU_SOURCE
#include "dm/hwid.h"
#include <errno.h>
#include <fcntl.h>
#include <libuboot.h>
#include <mtd/mtd-user.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

/* Use the same standard two-copy fw_env.config as fw_printenv/fw_setenv.
 * libubootenv owns CRC, active-copy selection, locking and atomic updates. */
static int open_environment(const char *config, struct uboot_ctx **ctx)
{
    char *line = NULL; size_t size = 0; unsigned int copies = 0;
    FILE *file = fopen(config, "r");
    if (!file) return -errno;
    int rc = 0;
    while (getline(&line, &size, file) >= 0) {
        char device[256], offset[64], envsize[64], sector[64], count[64], extra;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#' || *p == '\n') continue;
        int fields = sscanf(p, "%255s %63s %63s %63s %63s %c", device, offset, envsize, sector, count, &extra);
        if (fields < 3 || fields > 5 || ++copies > 2) { rc = -EINVAL; break; }
        struct stat st;
        if (stat(device, &st)) { rc = -errno; break; }
        if (S_ISCHR(st.st_mode)) {
            struct mtd_info_user info;
            int fd = open(device, O_RDONLY | O_CLOEXEC);
            if (fd < 0) { rc = -errno; break; }
            int ret = ioctl(fd, MEMGETINFO, &info); close(fd);
            if (ret || info.type != MTD_NORFLASH) { rc = -ENOTSUP; break; }
        } else if (!S_ISREG(st.st_mode) && !S_ISBLK(st.st_mode)) { rc = -ENOTSUP; break; }
    }
    if (ferror(file) && !rc) rc = -EIO;
    free(line); fclose(file);
    if (rc) return rc;
    if (copies != 2) return -EINVAL;
    rc = libuboot_initialize(ctx, NULL);
    if (rc) return rc;
    rc = libuboot_read_config(*ctx, config);
    if (!rc) rc = libuboot_open(*ctx);
    if (rc) { libuboot_close(*ctx); libuboot_exit(*ctx); *ctx = NULL; }
    return rc;
}

static int get_hwid(struct uboot_ctx *ctx, struct dm_hwid_status *result)
{
    char *value = libuboot_get_env(ctx, "hwid"); unsigned char raw[8];
    if (!value) return -ENODATA;
    int rc = strlen(value) == 16 && dm_hwid_parse(value, raw) ? 0 : -EINVAL;
    if (!rc) dm_hwid_format(raw, result->text);
    free(value); return rc;
}

int dm_hwid_read(const char *config, struct dm_hwid_status *result)
{
    struct uboot_ctx *ctx = NULL;
    if (!config || !result) return -EINVAL;
    memset(result, 0, sizeof(*result));
    int rc = open_environment(config, &ctx);
    if (rc) return rc;
    rc = get_hwid(ctx, result);
    libuboot_close(ctx); libuboot_exit(ctx); return rc;
}

int dm_hwid_set(const char *config, const char *text, struct dm_hwid_status *result)
{
    struct uboot_ctx *ctx = NULL; unsigned char value[8]; char canonical[17];
    if (!config || !text || !result || strlen(text) != 16 || !dm_hwid_parse(text, value)) return -EINVAL;
    dm_hwid_format(value, canonical);
    int rc = open_environment(config, &ctx);
    if (rc) return rc; /* Never replace a corrupt environment with guessed defaults. */
    char *old = libuboot_get_env(ctx, "hwid");
    if (!old || strcmp(old, canonical)) {
        rc = libuboot_set_env(ctx, "hwid", canonical);
        if (!rc) rc = libuboot_env_store(ctx);
    }
    free(old);
    libuboot_close(ctx); libuboot_exit(ctx);
    if (!rc) rc = dm_hwid_read(config, result);
    if (!rc && strcmp(result->text, canonical)) rc = -EIO;
    return rc;
}
