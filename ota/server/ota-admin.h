#ifndef OTA_ADMIN_H
#define OTA_ADMIN_H
#include "ota-common.h"
#include <stddef.h>
#include <stdio.h>
#define OTA_UPLOAD_MAX (512ULL * 1024 * 1024)
struct ota_report;
struct ota_network_context {
    const char *ip, *mac, *hostname, *boot;
    char peer_ip[46];
};
int ota_network_valid(const struct ota_network_context *network);
int ota_catalog_request(const char *root, const char *device, unsigned long long publication,
                        const char *transaction, const char *phase, const struct ota_network_context *network);
int ota_catalog_network_json(const char *root, FILE *out, const struct ota_report *report);

struct ota_catalog_identity {
    unsigned long version;
    long long uploaded_at;
    unsigned long long kernel_bytes, rootfs_bytes;
    char tag[40], hash[65], kernel[65], rootfs[65];
};
int ota_catalog_download(const char *root, unsigned long long publication, const char *device);
int ota_catalog_downloads_json(const char *root, FILE *out);
int ota_catalog_get(const char *root, const char *directory, struct ota_catalog_identity *identity);
int ota_content_identity(const char *body, char hash[65], unsigned long *version);
int ota_quarantine_hashes_blocked(const char *root, const char *kernel, const char *rootfs);
int ota_quarantine_blocked(const char *root, const char *directory);
int ota_quarantine_report(const char *root, const struct ota_report *report);
int ota_quarantine_json(const char *root, void *stream);
int ota_firmware_identity(const char *directory, char hash[65], unsigned long *version);
int ota_image_identity(const char *directory, unsigned long long publication, char *body, size_t capacity);
int ota_fit_version(const char *directory, unsigned long *version);
int ota_inspect(const char *directory, struct release *r, unsigned long *version);
int ota_draft_begin(const char *root, char temp[OTA_PATH]);
void ota_draft_cleanup(const char *temp);
int ota_draft_finish(const char *root, const char *temp, char id[32]);
int ota_admin_state(const char *root, char **json);
int ota_admin_bundle(const char *root, const char *kind, const char *id, char filename[128], unsigned long long *size);
int ota_admin_publish(const char *root, const char *id, unsigned long long *number);
#endif
