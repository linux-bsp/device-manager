#define _GNU_SOURCE
#include <assert.h>
#include "../storage/dm_fw_storage_block.c"

int main(void)
{
    char cached_path[] = "/tmp/fw-cached-view-XXXXXX";
    char media_path[] = "/tmp/fw-media-view-XXXXXX";
    int cached = mkstemp(cached_path), media = mkstemp(media_path);
    assert(cached >= 0 && media >= 0);
    unlink(cached_path); unlink(media_path);
    unsigned char old[8192] = {0}, actual[8192], result[8192];
    for (unsigned i = 0; i < sizeof(actual); i++) actual[i] = i % 251 + 1;
    assert(write(cached, old, sizeof(old)) == sizeof(old));
    assert(write(media, actual, sizeof(actual)) == sizeof(actual));
    struct fw_storage storage = {
        .fd = cached, .read_fd = media, .read_direct = true,
        .info = { .read_size = 512, .capacity = sizeof(actual) },
    };
    /* Model distinct cached and media views; header and FIT-tail reads may
     * have arbitrary byte lengths and offsets despite direct-I/O alignment. */
    const size_t offsets[] = {0, 1, 511, 512, 4095, 4096, 8183};
    for (unsigned i = 0; i < sizeof(offsets)/sizeof(offsets[0]); i++) {
        size_t n = i == 0 ? sizeof(result) : 8;
        assert(!block_read(&storage, offsets[i], result, n));
        assert(!memcmp(result, actual + offsets[i], n));
    }
    storage.info.read_size = 4096;
    assert(!block_read(&storage, 4093, result, 31));
    assert(!memcmp(result, actual + 4093, 31));
    assert(block_read(&storage, 1, result, SIZE_MAX) == -EOVERFLOW);
    close(media);
    assert(block_read(&storage, 0, result, 8) == -EBADF);
    close(cached);
    return 0;
}
