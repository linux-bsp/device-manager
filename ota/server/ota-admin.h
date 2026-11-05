#ifndef OTA_ADMIN_H
#define OTA_ADMIN_H
#include "ota-common.h"
#include <stddef.h>
#include <stdio.h>
#define OTA_UPLOAD_MAX (512ULL * 1024 * 1024)
#define OTA_HWID_LIMIT 256
#define OTA_HWIDS_SIZE (OTA_HWID_LIMIT * 17)
int ota_hwid_list_valid(const char *values);
int ota_fit_hwids(const char *directory, char values[OTA_HWIDS_SIZE]);
int ota_fit_file_hwids(const char *directory, const char *filename, char values[OTA_HWIDS_SIZE]);
int ota_hwid_supported(const char *values, const char *hwid);
int ota_channel_path(const char *root, const char *hwid, char path[OTA_PATH]);
int ota_channel_read(const char *root, const char *hwid, struct release *release);
int ota_channel_set(const char *root, const struct release *release);
int ota_channels_set(const char *root, const struct release *releases, size_t count);
int ota_channel_active(const char *root, const struct release *release);
unsigned long long ota_publication_next(const char *root);
unsigned long long ota_channel_max(const char *root);
int ota_channels_json(const char *root, FILE *out);
struct ota_report;
struct ota_network_context {
    const char *ip, *mac, *hostname, *boot, *hwid;
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
    char hwids[OTA_HWIDS_SIZE];
};
/* Private worker result, never accepted from the network. The parent consumes
 * it only after successful worker exit and rechecks the file fingerprint. */
struct ota_prepared_draft {
    struct ota_catalog_identity identity;
    char fingerprint[1536];
};
int ota_catalog_prepare(const char *directory, struct ota_prepared_draft *result);
int ota_catalog_store(const char *root, const char *directory, const struct ota_prepared_draft *result);
struct ota_removal { char path[OTA_PATH], id[32]; long long created; };
int ota_admin_delete(const char *root, const char *id, const char *peer);
int ota_admin_cleanup(const char *root);
int ota_catalog_removed(const char *root, const char *path, long long *created);
int ota_catalog_recorded(const char *root, const char *path, struct ota_catalog_identity *identity);
int ota_catalog_remove_begin(const char *root, const struct ota_removal *items, size_t count,
                             const struct ota_catalog_identity *identity, const char *peer);
int ota_catalog_remove_done(const char *root, const char *path);
int ota_catalog_deletion(const char *root, const char *id, const struct ota_catalog_identity *identity, const char *status);
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
int ota_draft_prepare(const char *temp, struct ota_prepared_draft *result);
int ota_draft_finish(const char *root, const char *temp, const struct ota_prepared_draft *result, char id[32]);
int ota_admin_state(const char *root, char **json);
int ota_admin_bundle(const char *root, const char *kind, const char *id, char filename[128], unsigned long long *size);
int ota_admin_publish(const char *root, const char *id, unsigned long long *number);
#endif
