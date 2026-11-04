// SPDX-License-Identifier: GPL-2.0+
#define _GNU_SOURCE
#include "dm_fw_layout_priv.h"

#include "dm_fw_metadata_priv.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const region_names[FW_REGION_COUNT] = {
    "meta0", "meta1", "tiboot3_a", "tiboot3_b", "tispl_a", "tispl_b",
    "uboot_a", "uboot_b", "kernel_a", "kernel_b", "rootfs_a",
    "rootfs_b",
};

static int copy_string(char *dst, size_t dst_size, const char *src)
{
    size_t len = strlen(src);

    if (!len || len >= dst_size)
        return -ENAMETOOLONG;
    memcpy(dst, src, len + 1);
    return 0;
}

void fw_layout_defaults(struct fw_layout *layout)
{
    unsigned int i;
    memset(layout, 0, sizeof(*layout));
    strcpy(layout->work_dir, "/tmp");
    layout->attempts = FW_DEFAULT_ATTEMPTS;
    for (i = 0; i < FW_REGION_COUNT; i++)
        layout->region[i].name = region_names[i];
}

static char *trim(char *text)
{
    char *end;

    while (isspace((unsigned char)*text))
        text++;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
        end--;
    *end = '\0';
    return text;
}

static int parse_bool(const char *value, int *result)
{
    if (!strcmp(value, "1") || !strcasecmp(value, "true") ||
        !strcasecmp(value, "yes")) {
        *result = 1;
        return 0;
    }
    if (!strcmp(value, "0") || !strcasecmp(value, "false") ||
        !strcasecmp(value, "no")) {
        *result = 0;
        return 0;
    }
    return -EINVAL;
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

static int set_region_value(struct fw_layout *layout, const char *key,
                            const char *value)
{
    unsigned int i;
    size_t name_len;
    uint64_t parsed;
    int ret;

    for (i = 0; i < FW_REGION_COUNT; i++) {
        name_len = strlen(layout->region[i].name);
        if (strncmp(key, layout->region[i].name, name_len))
            continue;
        if (!strcmp(key + name_len, "-offset")) {
            ret = parse_u64(value, &parsed);
            if (!ret)
                layout->region[i].offset = parsed;
            return ret;
        }
        if (!strcmp(key + name_len, "-size")) {
            ret = parse_u64(value, &parsed);
            if (!ret)
                layout->region[i].size = parsed;
            return ret;
        }
    }
    return -ENOENT;
}

static int set_config_value(struct fw_layout *layout, const char *key,
                            const char *value)
{
    uint64_t parsed;
    int ret;

    if (!strcmp(key, "device"))
        return copy_string(layout->device, sizeof(layout->device), value);
    if (!strcmp(key, "storage-type")) {
        if (!strcmp(value, "block"))
            layout->storage_type = FW_STORAGE_BLOCK;
        else if (!strcmp(value, "mtd-nor"))
            layout->storage_type = FW_STORAGE_MTD_NOR;
        else
            return -EINVAL;
        return 0;
    }
    if (!strcmp(key, "update-protocol") || !strcmp(key, "update-server") ||
        !strcmp(key, "update-path") || !strcmp(key, "update-release"))
        return *value ? 0 : -EINVAL;
    if (!strcmp(key, "product"))
        return copy_string(layout->product, sizeof(layout->product), value);
    if (!strcmp(key, "hwid-config"))
        return copy_string(layout->hwid_config, sizeof(layout->hwid_config), value);
    if (!strcmp(key, "layout-id"))
        return copy_string(layout->layout_id, sizeof(layout->layout_id), value);
    if (!strcmp(key, "signature-key")) {
        if (!*value) {
            layout->signature_key[0] = '\0';
            return 0;
        }
        return copy_string(layout->signature_key,
                           sizeof(layout->signature_key), value);
    }
    if (!strcmp(key, "work-dir"))
        return copy_string(layout->work_dir, sizeof(layout->work_dir), value);
    if (!strcmp(key, "allow-bootloader-update"))
        return parse_bool(value, &layout->allow_bootloader_update);
    if (!strcmp(key, "require-signature"))
        return parse_bool(value, &layout->require_signature);
    if (!strcmp(key, "boot-attempts")) {
        ret = parse_u64(value, &parsed);
        if (ret || !parsed || parsed > UINT8_MAX)
            return -EINVAL;
        layout->attempts = (uint8_t)parsed;
        return 0;
    }
    return set_region_value(layout, key, value);
}

int fw_layout_load(struct fw_layout *layout, const char *path)
{
    FILE *file;
    char *line = NULL;
    size_t capacity = 0;
    int ret = 0;

    if (!layout || !path)
        return -EINVAL;
    file = fopen(path, "r");
    if (!file)
        return -errno;
    while (getline(&line, &capacity, file) >= 0) {
        char *key;
        char *value;
        char *separator;

        key = trim(line);
        if (!*key || *key == '#')
            continue;
        separator = strchr(key, '=');
        if (!separator) {
            ret = -EINVAL;
            break;
        }
        *separator = '\0';
        value = trim(separator + 1);
        key = trim(key);
        ret = set_config_value(layout, key, value);
        if (ret)
            break;
    }
    if (ferror(file) && !ret)
        ret = -EIO;
    free(line);
    fclose(file);
    return ret;
}

int fw_layout_validate(const struct fw_layout *layout, uint64_t capacity,
                       uint32_t write_size, uint32_t erase_size)
{
    unsigned int i;
    unsigned int j;

    if (!layout || !layout->device[0] || !layout->product[0] ||
        !write_size ||
        !layout->layout_id[0] || !layout->attempts || !layout->work_dir[0])
        return -EINVAL;
    if (layout->require_signature && !layout->signature_key[0])
        return -EINVAL;
    for (i = 0; i < FW_REGION_COUNT; i++) {
        const struct fw_region *a = &layout->region[i];

        if (!a->size || a->offset > capacity ||
            a->size > capacity - a->offset ||
            a->offset % write_size || a->size % write_size ||
            (erase_size && (a->offset % erase_size ||
                            a->size % erase_size)))
            return -EINVAL;
        for (j = i + 1; j < FW_REGION_COUNT; j++) {
            const struct fw_region *b = &layout->region[j];

            if (a->offset < b->offset + b->size &&
                b->offset < a->offset + a->size)
                return -EINVAL;
        }
    }
    if (layout->region[FW_REGION_META0].size < FW_METADATA_SIZE ||
        layout->region[FW_REGION_META1].size < FW_METADATA_SIZE)
        return -ENOSPC;
    return 0;
}

const struct fw_region *fw_layout_component_region(
    const struct fw_layout *layout, unsigned int component, uint8_t slot)
{
    enum fw_region_id id;

    if (!layout || slot > 1)
        return NULL;
    if (component == FW_COMPONENT_KERNEL)
        id = FW_REGION_KERNEL_A + slot;
    else if (component == FW_COMPONENT_ROOTFS)
        id = FW_REGION_ROOTFS_A + slot;
    else
        return NULL;
    return &layout->region[id];
}
