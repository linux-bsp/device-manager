#include <assert.h>
#include <errno.h>
#include <string.h>
#include "dm_fw_update_priv.h"
#include "dm_fw_fit_priv.h"
#include "dm_fw_source_priv.h"

static unsigned char media[FW_REGION_COUNT * FW_METADATA_SIZE];
static struct fw_layout layout;
static int fail_component;
static int corrupt_component;
static int artifacts;
static enum fw_component selected_component = FW_COMPONENT_KERNEL;
static const unsigned char image[] = {1, 2, 3, 4};
int fw_source_get_release(const struct fw_layout *value, const char *source,
                           struct fw_artifact *artifact)
{
    (void)value; (void)source;
    strcpy(artifact->path, "release");
    ++artifacts;
    return 0;
}
int fw_source_get_component(const struct fw_layout *value, const char *release,
                             const char *component, struct fw_artifact *artifact)
{
    (void)value; (void)release; (void)component;
    strcpy(artifact->path, "kernel");
    ++artifacts;
    return 0;
}
void fw_source_put(struct fw_artifact *artifact) { (void)artifact; --artifacts; }
int fw_fit_open(struct fw_fit *fit, const char *path, const struct fw_layout *value)
{
    (void)fit; (void)path; (void)value;
    return 0;
}
void fw_fit_close(struct fw_fit *fit) { (void)fit; }
int fw_fit_verify_release(const struct fw_layout *value, const struct fw_fit *fit,
                           struct fw_release *release)
{
    (void)value; (void)fit;
    memset(release, 0, sizeof(*release));
    release->version = 2;
    release->count = 1;
    release->component[0].component = selected_component;
    strcpy(release->component[0].file, "kernel");
    return 0;
}
int fw_fit_verify_package(const struct fw_layout *value, const struct fw_fit *fit,
                           struct fw_package *package)
{
    (void)value; (void)fit;
    memset(package, 0, sizeof(*package));
    package->component = selected_component;
    package->version = 2;
    return 0;
}
int fw_fit_check_identity(const struct fw_fit *fit, enum fw_component component,
                          const struct fw_identity *identity)
{
    (void)fit; (void)component; (void)identity;
    return 0;
}
int fw_sha256_file(const char *path, uint8_t digest[FW_SHA256_SIZE])
{
    (void)path;
    memset(digest, 0, FW_SHA256_SIZE);
    return 0;
}
int fw_fit_get_component_data(const struct fw_fit *fit, enum fw_component component,
    const char *name, const void **data, size_t *size)
{
    (void)fit; (void)component; (void)name;
    *data = image; *size = sizeof(image);
    return 0;
}
static int read_media(struct fw_storage *storage, uint64_t offset, void *data, size_t size)
{
    (void)storage;
    memcpy(data, media + offset, size);
    if (corrupt_component && offset == layout.region[FW_REGION_KERNEL_B].offset)
        ((unsigned char *)data)[0] ^= 1;
    return 0;
}
static int write_media(struct fw_storage *storage, uint64_t offset, const void *data, size_t size)
{
    struct fw_metadata persisted;
    if (offset == layout.region[FW_REGION_KERNEL_B].offset ||
        offset == layout.region[FW_REGION_ROOTFS_B].offset) {
        assert(!fw_metadata_load(storage, &persisted));
        assert(persisted.deployment[1].state == FW_STATE_WRITING);
        assert(persisted.pending_deployment == FW_SLOT_NONE);
        if (fail_component) return -EIO;
    }
    memcpy(media + offset, data, size);
    return 0;
}
static int sync_media(struct fw_storage *storage) { (void)storage; return 0; }
static int erase_media(struct fw_storage *storage, uint64_t offset, uint64_t size)
{
    (void)storage; (void)offset; (void)size;
    return 0;
}
static const struct fw_storage_ops memory_ops = {
    .read = read_media, .write = write_media, .sync = sync_media, .erase = erase_media,
};
int main(void)
{
    struct fw_storage storage = {.fd = -1, .layout = &layout, .ops = &memory_ops,
        .info = {.capacity = sizeof(media), .write_size = 1}};
    struct fw_metadata metadata;
    fw_layout_defaults(&layout);
    for (unsigned int i = 0; i < FW_REGION_COUNT; ++i) {
        layout.region[i].offset = i * FW_METADATA_SIZE;
        layout.region[i].size = FW_METADATA_SIZE;
    }
    for (unsigned int scenario = 0; scenario < 3; ++scenario) {
        memset(media, 0, sizeof(media));
        fw_metadata_init(&metadata, 0);
        assert(!fw_metadata_save(&storage, &metadata));
        assert(!fw_metadata_save(&storage, &metadata));
        fail_component = scenario == 1;
        corrupt_component = scenario == 2;
        assert(fw_update_release(&layout, &storage, "release", 0, NULL, NULL) ==
               (scenario ? -EIO : 0));
        assert(!artifacts);
        assert(!fw_metadata_load(&storage, &metadata));
        assert(metadata.active_deployment == 0 && metadata.last_good_deployment == 0);
        assert(metadata.deployment[0].component[FW_COMPONENT_KERNEL].slot == 0);
        if (scenario) {
            assert(metadata.pending_deployment == FW_SLOT_NONE);
            assert(metadata.deployment[1].state == FW_STATE_WRITING);
        } else {
            assert(metadata.pending_deployment == 1);
            assert(metadata.deployment[1].state == FW_STATE_READY);
            assert(metadata.deployment[1].component[FW_COMPONENT_KERNEL].slot == 1);
        }
    }
    fail_component = corrupt_component = 0;
    memset(media, 0, sizeof(media));fw_metadata_init(&metadata, 0);
    assert(!fw_metadata_save(&storage, &metadata));
    assert(!fw_metadata_save(&storage, &metadata));
    assert(!fw_metadata_check_update_source(&metadata, 0));
    assert(fw_metadata_check_update_source(&metadata, 1) == -EBUSY);
    metadata.deployment[0].state = FW_STATE_BOOT_TESTING;
    assert(fw_metadata_check_update_source(&metadata, 0) == -EBUSY);
    metadata.deployment[0].state = FW_STATE_CONFIRMED;
    unsigned char saved[sizeof(media)];memcpy(saved, media, sizeof(media));
    assert(fw_update_release(&layout, &storage, "release", DM_FW_COMPONENT_ROOTFS + 1, NULL, NULL) == -ENOENT);
    assert(!memcmp(saved, media, sizeof(media))); /* Absent component must not mutate metadata. */
    selected_component = FW_COMPONENT_BOOTLOADER;
    assert(fw_update_release(&layout, &storage, "release", DM_FW_COMPONENT_BOOTLOADER + 1, NULL, NULL) == -EPERM);
    assert(!memcmp(saved, media, sizeof(media)));
    selected_component = FW_COMPONENT_KERNEL;
    assert(!fw_update_release(&layout, &storage, "release", DM_FW_COMPONENT_KERNEL + 1, NULL, NULL));
    assert(!fw_metadata_load(&storage, &metadata));
    assert(metadata.deployment[1].component[FW_COMPONENT_KERNEL].slot == 1);
    assert(metadata.deployment[1].component[FW_COMPONENT_ROOTFS].slot == 0);
    /* Repeating a staged component must keep writing B, never confirmed A. */
    memset(media + layout.region[FW_REGION_KERNEL_A].offset, 0xa5, sizeof(image));
    assert(!fw_update_release(&layout, &storage, "release", DM_FW_COMPONENT_KERNEL + 1, NULL, NULL));
    for (unsigned i = 0; i < sizeof(image); i++) assert(media[layout.region[FW_REGION_KERNEL_A].offset + i] == 0xa5);
    selected_component = FW_COMPONENT_ROOTFS;
    assert(!fw_update_release(&layout, &storage, "release", DM_FW_COMPONENT_ROOTFS + 1, NULL, NULL));
    assert(!fw_metadata_load(&storage, &metadata));
    assert(metadata.pending_deployment == 1 && metadata.deployment[1].state == FW_STATE_READY);
    assert(metadata.deployment[1].component[FW_COMPONENT_KERNEL].slot == 1);
    assert(metadata.deployment[1].component[FW_COMPONENT_ROOTFS].slot == 1);
    /* A writing deployment cannot be manually confirmed. */
    uint8_t target;
    assert(!fw_metadata_begin_component(&metadata, FW_COMPONENT_ROOTFS, &target));
    assert(fw_metadata_mark_good(&metadata, target) == -EINVAL);
    assert(!artifacts);
    return 0;
}
