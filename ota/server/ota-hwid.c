/* Hardware declarations are FIT metadata, never guessed from product names. */
#define _POSIX_C_SOURCE 200809L
#include "ota-admin.h"
#include "dm/hwid-value.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

int ota_hwid_list_valid(const char *values)
{
    size_t length = strlen(values);
    if (!length) return 1; /* Historical packages did not declare hardware. */
    if (length >= OTA_HWIDS_SIZE || (length + 1) % 17) return 0;
    for (size_t offset = 0; offset < length; offset += 17) {
        char text[17], canonical[17]; unsigned char raw[8];
        memcpy(text, values + offset, 16); text[16] = 0;
        if (!dm_hwid_parse(text, raw)) return 0;
        dm_hwid_format(raw, canonical);
        if (strcmp(text, canonical) || (offset + 16 < length && values[offset + 16] != ' ')) return 0;
        for (size_t previous = 0; previous < offset; previous += 17)
            if (!memcmp(values + previous, text, 16)) return 0;
    }
    return 1;
}

int ota_fit_file_hwids(const char *directory, const char *filename, char values[OTA_HWIDS_SIZE])
{
    char path[OTA_PATH], config[128], node[256], output[OTA_HWIDS_SIZE * 3 + 32];
    if (ota_path(path, directory, filename)) return -1;
    char *configuration[] = {"fdtget", "-t", "s", path, "/configurations", "default", NULL};
    if (ota_run(configuration, config, sizeof(config))) return -1;
    config[strcspn(config, "\r\n")] = 0;
    if (!ota_identifier(config)) return -1;
    snprintf(node, sizeof(node), "/configurations/%s", config);
    /* Read raw bytes: -t s would confuse one string containing spaces with a
     * real NUL-delimited allowlist. The extra capacity detects truncated output. */
    char *property[] = {"fdtget", "-t", "bx", "-d", "", path, node, "fw,hwids", NULL};
    if (ota_run(property, output, sizeof(output))) return -1;
    unsigned char bytes[OTA_HWIDS_SIZE]; size_t count = 0;
    char *cursor = output;
    while (*cursor) {
        while (isspace((unsigned char)*cursor)) cursor++;
        if (!*cursor) break;
        if (!isxdigit((unsigned char)*cursor)) return -1;
        char *end; unsigned long value = strtoul(cursor, &end, 16);
        if (end - cursor > 2 || value > 255 || (*end && !isspace((unsigned char)*end)) || count == sizeof(bytes)) return -1;
        bytes[count++] = (unsigned char)value;
        cursor = end;
    }
    values[0] = 0;
    if (!count) return 0;
    if (count % 17) return -1;
    for (size_t offset = 0; offset < count; offset += 17) {
        if (bytes[offset + 16] || memchr(bytes + offset, 0, 16)) return -1;
        memcpy(values + offset, bytes + offset, 16);
        values[offset + 16] = offset + 17 < count ? ' ' : 0;
    }
    return ota_hwid_list_valid(values) ? 0 : -1;
}

int ota_fit_hwids(const char *directory, char values[OTA_HWIDS_SIZE])
{
    return ota_fit_file_hwids(directory, "release.itb", values);
}
