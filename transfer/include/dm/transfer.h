// SPDX-License-Identifier: GPL-2.0+
#ifndef DM_TRANSFER_H
#define DM_TRANSFER_H
#include <stddef.h>
#include <stdint.h>

enum dm_transfer_error {
    DM_TRANSFER_OK, DM_TRANSFER_NETWORK, DM_TRANSFER_TIMEOUT,
    DM_TRANSFER_HTTP, DM_TRANSFER_STORAGE, DM_TRANSFER_INTEGRITY,
    DM_TRANSFER_PROTOCOL, DM_TRANSFER_CANCELLED
};
struct dm_transfer_callbacks {
    int (*cancelled)(void *context);
    void (*progress)(void *context, uint64_t done, uint64_t total);
    void *context;
};
/* Initialize with dm_transfer_request_init: descriptor -1 means unused.
 * perform returns transport/I/O errors; callers own application HTTP statuses.
 * A request is never automatically replayed (POST may have committed remotely). */
struct dm_transfer_request {
    const char *url, *method;
    const char *const *headers;
    size_t header_count;
    const void *body;
    size_t body_size;
    int upload_fd, output_fd;
    uint64_t upload_offset, upload_size, output_limit;
    char *response;
    size_t response_capacity;
    int (*sink)(void *context, const void *data, size_t length);
    void *sink_context;
    uint64_t range_offset, range_total;
    int ranged, allow_tftp, follow_redirects;
    long connect_timeout_ms, timeout_ms, low_speed_seconds, low_speed_bytes;
    uint64_t receive_rate, send_rate;
    struct dm_transfer_callbacks callbacks;
};
struct dm_transfer_result {
    enum dm_transfer_error kind;
    long http_status;
    uint64_t received, sent, resume_from, durable_bytes;
    size_t response_size;
    int backend_code, system_error, cached;
};
void dm_transfer_request_init(struct dm_transfer_request *request);
int dm_transfer_perform(const struct dm_transfer_request *request, struct dm_transfer_result *result);
const char *dm_transfer_error_name(enum dm_transfer_error error);

/* Only a fully verified, synced file is renamed to path. Callers own their task
 * namespace/lock. A resumable file requires immutable size and full SHA256.
 * .part.state commits only synced bytes and their prefix digest. */
struct dm_transfer_download {
    struct dm_transfer_request request;
    const char *path, *sha256;
    uint64_t size, maximum;
    int resume;
    unsigned attempts, retry_delay_ms;
};
int dm_transfer_download_file(const struct dm_transfer_download *download,
                               struct dm_transfer_result *result);
int dm_transfer_sha256_fd(int fd, uint64_t length, char digest[65],
                          const struct dm_transfer_callbacks *callbacks);
int dm_transfer_sync_parent(const char *path);
int dm_transfer_sync_file(const char *path);

/* Cursor protocol is supplied by the application. The library never interprets
 * boot IDs, firmware manifests, JSON acknowledgements or archive commit. */
typedef int (*dm_transfer_push_fn)(void *context, uint64_t offset,
    const void *data, size_t length, uint64_t *next);
struct dm_transfer_push {
    int fd;
    uint64_t size;
    size_t chunk_size;
    unsigned max_chunks;
    dm_transfer_push_fn exchange;
    void *context;
    struct dm_transfer_callbacks callbacks;
};
/* 0 caught up, 1 budget exhausted, negative errno on failure. */
int dm_transfer_push_file(const struct dm_transfer_push *push, uint64_t *next);
#endif
