// SPDX-License-Identifier: GPL-2.0+
#define _GNU_SOURCE
#include "dm_fw_source_priv.h"
#include "dm/transfer.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FW_RELEASE_DOWNLOAD_MAX (4ULL * 1024ULL * 1024ULL)
#define FW_COMPONENT_DOWNLOAD_MAX (512ULL * 1024ULL * 1024ULL)

static int is_url(const char *source)
{
    return !strncmp(source, "https://", 8) ||
           !strncmp(source, "http://", 7) ||
           !strncmp(source, "tftp://", 7);
}

static int download_url(const struct fw_layout *layout, const char *url,
                        uint64_t maximum, struct fw_artifact *artifact)
{
    char pattern[FW_PATH_SIZE];
    if (snprintf(pattern, sizeof(pattern), "%s/fwctl-XXXXXX", layout->work_dir) >= (int)sizeof(pattern)) return -ENAMETOOLONG;
    int fd = mkstemp(pattern); if (fd < 0) return -errno;
    if (close(fd)) { int error = -errno; unlink(pattern); return error; }
    struct dm_transfer_download options = {0}; struct dm_transfer_result result;
    dm_transfer_request_init(&options.request);
    options.request.url = url; options.request.allow_tftp = 1;
    options.request.follow_redirects = strncmp(url, "tftp://", 7) != 0;
    options.request.connect_timeout_ms = 30000; options.request.timeout_ms = 1800000;
    options.request.low_speed_bytes = 1024; options.request.low_speed_seconds = 60;
    options.path = pattern; options.maximum = maximum;
    int rc = dm_transfer_download_file(&options, &result);
    if (rc) { unlink(pattern); return rc; }
    memset(artifact, 0, sizeof(*artifact));
    strcpy(artifact->path, pattern); artifact->temporary = 1; return 0;
}

static int use_local_file(const char *path, struct fw_artifact *artifact)
{
    if (!path || !*path)
        return -EINVAL;
    if (strlen(path) >= sizeof(artifact->path))
        return -ENAMETOOLONG;
    memset(artifact, 0, sizeof(*artifact));
    strcpy(artifact->path, path);
    return access(path, R_OK) < 0 ? -errno : 0;
}

int fw_source_get_release(const struct fw_layout *layout, const char *source,
                          struct fw_artifact *artifact)
{
    if (!layout || !source || !artifact)
        return -EINVAL;
    if (is_url(source))
        return download_url(layout, source, FW_RELEASE_DOWNLOAD_MAX,
                            artifact);
    return use_local_file(source, artifact);
}

int fw_source_build_component(const char *release_source,
                              const char *component_file,
                              char *result, size_t result_size)
{
    const char *slash;
    size_t base_len;
    int ret;

    if (!release_source || !component_file || !*component_file ||
        component_file[0] == '/' || strstr(component_file, ".."))
        return -EINVAL;
    if (is_url(release_source) &&
        (strchr(release_source, '?') || strchr(release_source, '#')))
        return -EINVAL;
    slash = strrchr(release_source, '/');
    if (!slash) {
        ret = snprintf(result, result_size, "%s", component_file);
    } else {
        base_len = (size_t)(slash - release_source + 1);
        ret = snprintf(result, result_size, "%.*s%s", (int)base_len,
                       release_source, component_file);
    }
    if (ret < 0 || (size_t)ret >= result_size)
        return -ENAMETOOLONG;
    return 0;
}

int fw_source_get_component(const struct fw_layout *layout,
                            const char *release_source,
                            const char *component_file,
                            struct fw_artifact *artifact)
{
    char source[FW_PATH_SIZE * 2U];
    int ret;

    if (!layout || !release_source || !component_file || !artifact)
        return -EINVAL;
    ret = fw_source_build_component(release_source, component_file, source,
                                    sizeof(source));
    if (ret)
        return ret;
    if (is_url(source))
        return download_url(layout, source, FW_COMPONENT_DOWNLOAD_MAX,
                            artifact);
    return use_local_file(source, artifact);
}

void fw_source_put(struct fw_artifact *artifact)
{
    if (!artifact)
        return;
    if (artifact->temporary && artifact->path[0])
        unlink(artifact->path);
    memset(artifact, 0, sizeof(*artifact));
}
