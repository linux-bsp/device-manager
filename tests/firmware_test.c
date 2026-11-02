#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "dm/firmware.h"
#include "dm_fw_metadata_priv.h"
#include "dm_fw_storage_priv.h"
#include "dm_fw_update_priv.h"

/* The updater is a separate boundary. These tests exercise handles, metadata
 * durability and storage verification without FIT/HTTP/crypto dependencies. */
int fw_update_release(const struct fw_layout *layout, struct fw_storage *storage,
    const char *source, uint32_t component, dm_fw_progress_fn progress, void *data)
{
    (void)layout; (void)storage; (void)source; (void)component;
    (void)progress; (void)data;
    return -ENOTSUP;
}
int fw_restore_rootfs(const struct fw_layout *l, struct fw_storage *s, uint32_t d,
    const char *p, uint64_t n, const uint8_t h[32])
{
    (void)l; (void)s; (void)d; (void)p; (void)n; (void)h; return -ENOTSUP;
}
int fw_validate_recovery_kernel(const struct fw_layout *l, struct fw_storage *s,
    const struct fw_metadata *m, uint32_t d)
{ (void)l; (void)s; (void)m; (void)d; return 0; }
int fw_identity_from_storage(const struct fw_layout *l, struct fw_storage *s,
    const struct fw_metadata *m, uint32_t d, struct fw_identity *id)
{ (void)l; (void)s; (void)m; (void)d; (void)id; return -ENODATA; }
static unsigned char media[FW_METADATA_SIZE * 2];
static int fail_write;
static size_t partial_write;
static int media_read(struct fw_storage *storage, uint64_t offset, void *data, size_t size)
{
    (void)storage;
    memcpy(data, media + offset, size);
    return 0;
}
static int media_write(struct fw_storage *storage, uint64_t offset, const void *data, size_t size)
{
    (void)storage;
    if (fail_write) return -EIO;
    if (partial_write) {
        memcpy(media + offset, data, partial_write);
        return -EIO;
    }
    memcpy(media + offset, data, size);
    return 0;
}
static int media_sync(struct fw_storage *storage) { (void)storage; return 0; }
static const struct fw_storage_ops memory_ops = {
    .read = media_read, .write = media_write, .sync = media_sync,
};
static void check_identity_metadata(void)
{
    struct fw_layout layout;
    struct fw_storage storage = {.layout = &layout, .ops = &memory_ops,
        .info = {.capacity = sizeof(media), .write_size = 1}};
    struct fw_metadata original, readback;
    fw_layout_defaults(&layout);
    layout.region[FW_REGION_META0].offset = 0;
    layout.region[FW_REGION_META1].offset = FW_METADATA_SIZE;
    layout.region[FW_REGION_META0].size = FW_METADATA_SIZE;
    layout.region[FW_REGION_META1].size = FW_METADATA_SIZE;
    memset(media, 0xff, sizeof(media));
    fw_metadata_init(&original, 0);
    struct fw_identity *identity = &original.deployment[0].identity;
    identity->valid = 1;
    strcpy(identity->tag, "v1.0.12");
    memset(identity->id, 0x12, sizeof(identity->id));
    memset(identity->hash, 0xab, sizeof(identity->hash));
    identity->size[0] = 12345;
    identity->size[1] = 67890;
    assert(!fw_metadata_save(&storage, &original));
    unsigned char saved[sizeof(media)];
    memcpy(saved, media, sizeof(media));
    assert(!fw_metadata_load(&storage, &readback));
    assert(!memcmp(identity, &readback.deployment[0].identity, sizeof(*identity)));
    const size_t cuts[] = {1, 124, 128, 132, 256, 512, 4095};
    for (unsigned i = 0; i < sizeof(cuts) / sizeof(cuts[0]); ++i) {
        memcpy(media, saved, sizeof(media));
        readback = original;
        strcpy(readback.deployment[0].identity.tag, "v1.0.13");
        partial_write = cuts[i];
        assert(fw_metadata_save(&storage, &readback) == -EIO);
        partial_write = 0;
        assert(!fw_metadata_load(&storage, &readback));
        assert(readback.sequence == original.sequence);
        assert(!strcmp(readback.deployment[0].identity.tag, "v1.0.12"));
    }
    memcpy(media, saved, sizeof(media));
    readback = original;
    strcpy(readback.deployment[0].identity.tag, "v1.0.13");
    assert(!fw_metadata_save(&storage, &readback));
    media[readback.source_copy * FW_METADATA_SIZE + 160] ^= 1;
    assert(!fw_metadata_load(&storage, &readback));
    assert(readback.sequence == original.sequence);
    uint8_t target;
    assert(!fw_metadata_begin_release(&readback, 65549, &target));
    assert(readback.deployment[0].identity.valid && !readback.deployment[target].identity.valid);
    /* A genuine legacy header is accepted, without inventing an identity. */
    memcpy(media, saved, sizeof(media));
    unsigned char *legacy = media + original.source_copy * FW_METADATA_SIZE;
    legacy[18] = legacy[19] = 0;
    memset(legacy + 128, 0xff, FW_METADATA_SIZE - 128);
    uint32_t crc = ~0U;
    for (unsigned i = 0; i < 124; ++i) {
        crc ^= legacy[i];
        for (unsigned b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xedb88320U & (uint32_t)-(int32_t)(crc & 1U));
    }
    crc = ~crc;
    for (unsigned i = 0; i < 4; ++i) legacy[124 + i] = crc >> (8 * i);
    assert(!fw_metadata_load(&storage, &readback));
    assert(!readback.deployment[0].identity.valid);
    assert(!fw_metadata_save(&storage, &readback));
    assert(!fw_metadata_load(&storage, &readback));
    memset(media, 0, sizeof(media));
}

static void check_actual_image(void)
{
    char config_path[] = "/tmp/fw-check-config-XXXXXX";
    char device_path[] = "/tmp/fw-check-media-XXXXXX";
    int config_fd = mkstemp(config_path), fd = mkstemp(device_path);
    struct fw_layout layout;
    struct fw_metadata metadata;
    struct fw_storage storage = { .fd = -1 };
    uint64_t sizes[2] = { 7, 9 };
    uint8_t hashes[2][32];
    const char *data[2] = {"kernel!", "rootfs!!!"};
    int matches = -1;
    assert(config_fd >= 0 && fd >= 0);
    assert(!ftruncate(fd, FW_REGION_COUNT * 65536));
    FILE *config = fdopen(config_fd, "w");
    assert(config);
    fprintf(config, "device=%s\nproduct=test\nlayout-id=test-v1\n", device_path);
    fw_layout_defaults(&layout);
    for (unsigned i = 0; i < FW_REGION_COUNT; i++)
        fprintf(config, "%s-offset=%u\n%s-size=65536\n", layout.region[i].name,
                i * 65536U, layout.region[i].name);
    assert(!fclose(config));
    assert(!fw_layout_load(&layout, config_path));
    assert(!fw_storage_open(&storage, &layout, true));
    fw_metadata_init(&metadata, 0);
    metadata.deployment[0].release_version = 65537;
    /* Different slots for kernel and rootfs: deployment id is not a slot id. */
    metadata.deployment[0].component[FW_COMPONENT_ROOTFS].slot = 1;
    assert(!fw_metadata_save(&storage, &metadata));
    for (unsigned i = 0; i < 2; i++) {
        unsigned length;
        unsigned component = FW_COMPONENT_KERNEL + i;
        const struct fw_region *region = fw_layout_component_region(&layout, component, i);
        assert(!fw_storage_write(&storage, region->offset, data[i], sizes[i]));
        assert(EVP_Digest(data[i], sizes[i], hashes[i], &length, EVP_sha256(), NULL));
        assert(length == 32);
    }
    fw_storage_close(&storage);
    int handle = dm_fw_open(config_path);
    assert(handle > 0);
    assert(!dm_fw_check_image(handle, 0, 65537, sizes, hashes, &matches) && matches);
    assert(!dm_fw_check_image(handle, 0, 65538, sizes, hashes, &matches) && !matches);
    /* Padding is outside the effective payload and must not affect identity. */
    uint64_t offset = layout.region[FW_REGION_ROOTFS_B].offset;
    assert(pwrite(fd, "X", 1, offset + sizes[1]) == 1);
    assert(!dm_fw_check_image(handle, 0, 65537, sizes, hashes, &matches) && matches);
    assert(pwrite(fd, "X", 1, offset) == 1);
    assert(!dm_fw_check_image(handle, 0, 65537, sizes, hashes, &matches) && !matches);
    sizes[0] = 65537;
    assert(dm_fw_check_image(handle, 0, 65537, sizes, hashes, &matches) == -EFBIG);
    sizes[0] = 0;
    assert(dm_fw_check_image(handle, 0, 65537, sizes, hashes, &matches) == -EINVAL);
    assert(!dm_fw_close(handle));
    close(fd); unlink(device_path); unlink(config_path);
}

int main(void)
{
    check_identity_metadata();
    check_actual_image();
    char path[] = "/tmp/edk-firmware-test-XXXXXX";
    int fd = mkstemp(path);
    FILE *config;
    struct fw_layout layout;
    struct fw_storage storage = { .layout = &layout, .ops = &memory_ops,
        .info = { .capacity = sizeof(media), .write_size = 1 } };
    struct fw_metadata initial, restored;
    struct dm_fw_status status;
    dm_fw_handle_t old_token, token;
    assert(fd >= 0);
    config = fdopen(fd, "w");
    assert(config);
    fprintf(config, "device=/tmp/edk-test-device\nproduct=test\nlayout-id=test-v1\n");
    fw_layout_defaults(&layout);
    assert(!layout.device[0] && !layout.product[0]); /* no board defaults */
    for (unsigned int i = 0; i < FW_REGION_COUNT; ++i)
        fprintf(config, "%s-offset=%u\n%s-size=4096\n", layout.region[i].name,
            i * FW_METADATA_SIZE, layout.region[i].name);
    assert(!fclose(config));
    old_token = dm_fw_open(path);
    assert(old_token > 0 && !dm_fw_close(old_token));
    token = dm_fw_open(path);
    assert(token > 0 && token != old_token);
    assert(dm_fw_get_status(old_token, &status) == -EBADF);
    assert(dm_fw_close(old_token) == -EBADF);
    assert(!dm_fw_close(token));
    unlink(path);
    assert(dm_fw_open(path) == -ENOENT);

    layout.region[FW_REGION_META0].offset = 0;
    layout.region[FW_REGION_META1].offset = FW_METADATA_SIZE;
    fw_metadata_init(&initial, 0);
    assert(!fw_metadata_save(&storage, &initial));
    assert(!fw_metadata_save(&storage, &initial));
    assert(!fw_metadata_load(&storage, &restored));
    assert(restored.active_deployment == 0);
    uint8_t target;
    restored.deployment[0].release_version = 7;
    assert(!fw_metadata_begin_release(&restored, 6, &target) && target == 1);
    assert(restored.deployment[0].release_version == 7);
    /* One corrupt copy must not destroy the committed metadata. */
    media[0] ^= 0xff;
    assert(!fw_metadata_load(&storage, &restored));
    fail_write = 1;
    assert(fw_metadata_save(&storage, &restored) == -EIO);
    fail_write = 0;
    assert(!fw_metadata_load(&storage, &restored));
    media[FW_METADATA_SIZE] ^= 0xff;
    assert(fw_metadata_load(&storage, &restored) < 0);
    return 0;
}
