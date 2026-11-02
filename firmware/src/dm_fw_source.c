// SPDX-License-Identifier: GPL-2.0+
#define _GNU_SOURCE
#include "dm_fw_source_priv.h"
#include "dm_http.h"

#include <curl/curl.h>
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

struct download_context {
    int fd;
    uint64_t received;
    uint64_t maximum;
    int error;
};

static int is_url(const char *source)
{
    return !strncmp(source, "https://", 8) ||
           !strncmp(source, "http://", 7) ||
           !strncmp(source, "tftp://", 7);
}

static int is_tftp_url(const char *source)
{
    return !strncmp(source, "tftp://", 7);
}

static size_t write_download(void *data, size_t size, size_t count, void *arg)
{
    struct download_context *ctx = arg;
    const uint8_t *p = data;
    size_t length;
    size_t done = 0;

    if (size && count > SIZE_MAX / size) {
        ctx->error = -EOVERFLOW;
        return 0;
    }
    length = size * count;
    if (ctx->received > ctx->maximum ||
        length > ctx->maximum - ctx->received) {
        ctx->error = -EFBIG;
        return 0;
    }
    while (done < length) {
        ssize_t ret = write(ctx->fd, p + done, length - done);

        if (ret < 0) {
            if (errno == EINTR)
                continue;
            ctx->error = -errno;
            return 0;
        }
        if (!ret) {
            ctx->error = -EIO;
            return 0;
        }
        done += (size_t)ret;
    }
    ctx->received += length;
    return length;
}

static int download_url(const struct fw_layout *layout, const char *url,
                        uint64_t maximum, struct fw_artifact *artifact)
{
    struct download_context ctx;
    char pattern[FW_PATH_SIZE];
    CURL *curl;
    CURLcode result;
    int tftp;
    int ret;

    ret = snprintf(pattern, sizeof(pattern), "%s/fwctl-XXXXXX",
                   layout->work_dir);
    if (ret < 0 || (size_t)ret >= sizeof(pattern))
        return -ENAMETOOLONG;
    memset(artifact, 0, sizeof(*artifact));
    ctx.fd = mkstemp(pattern);
    if (ctx.fd < 0)
        return -errno;
    ctx.received = 0;
    ctx.maximum = maximum;
    ctx.error = 0;

    ret = dm_http_global_init();
    if (ret) {
        goto error;
    }
    curl = curl_easy_init();
    if (!curl) {
        ret = -ENOMEM;
        goto error;
    }
    tftp = is_tftp_url(url);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR,
                     tftp ? "tftp" : "http,https");
    if (!tftp) {
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR,
                         !strncmp(url, "https://", 8) ? "https" :
                         "http,https");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    }
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "fwctl/2.0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_download);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    result = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK) {
        ret = ctx.error ? ctx.error : -EIO;
        goto error;
    }
    if (!ctx.received) {
        ret = -ENODATA;
        goto error;
    }
    if (fsync(ctx.fd) < 0) {
        ret = -errno;
        goto error;
    }
    close(ctx.fd);
    if (strlen(pattern) >= sizeof(artifact->path)) {
        unlink(pattern);
        return -ENAMETOOLONG;
    }
    strcpy(artifact->path, pattern);
    artifact->temporary = 1;
    return 0;

error:
    close(ctx.fd);
    unlink(pattern);
    return ret;
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
