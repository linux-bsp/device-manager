// SPDX-License-Identifier: GPL-2.0+
#define _GNU_SOURCE
#include "dm_fw_fit_priv.h"
#include "dm/hwid.h"

#include <errno.h>
#include <fcntl.h>
#include <libfdt.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int copy_string(char *dst, size_t dst_size, const char *src,
                       int src_len)
{
    if (!src || src_len < 2 || src[src_len - 1] ||
        strnlen(src, (size_t)src_len) != (size_t)src_len - 1)
        return -EINVAL;
    if ((size_t)src_len > dst_size)
        return -ENAMETOOLONG;
    memcpy(dst, src, (size_t)src_len);
    return 0;
}

static int get_string(const struct fw_fit *fit, int node, const char *name,
                      const char **value)
{
    const char *property;
    int len;

    property = fdt_getprop(fit->data, node, name, &len);
    if (!property)
        return len;
    if (len < 2 || property[len - 1] ||
        strnlen(property, (size_t)len) != (size_t)len - 1)
        return -EINVAL;
    *value = property;
    return 0;
}

static int get_u32(const struct fw_fit *fit, int node, const char *name,
                   uint32_t *value)
{
    const fdt32_t *property;
    int len;

    property = fdt_getprop(fit->data, node, name, &len);
    if (!property)
        return len;
    if (len != (int)sizeof(*property))
        return -EINVAL;
    *value = fdt32_to_cpu(*property);
    return 0;
}

static int sha256_data(const void *data, size_t size,
                       uint8_t digest[FW_SHA256_SIZE])
{
    EVP_MD_CTX *ctx;
    unsigned int digest_size = 0;
    int ret = -EIO;

    ctx = EVP_MD_CTX_new();
    if (!ctx)
        return -ENOMEM;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
        EVP_DigestUpdate(ctx, data, size) == 1 &&
        EVP_DigestFinal_ex(ctx, digest, &digest_size) == 1 &&
        digest_size == FW_SHA256_SIZE)
        ret = 0;
    EVP_MD_CTX_free(ctx);
    return ret;
}

int fw_sha256_file(const char *path, uint8_t digest[FW_SHA256_SIZE])
{
    EVP_MD_CTX *ctx;
    uint8_t buffer[64U * 1024U];
    unsigned int digest_size = 0;
    int fd;
    int ret = 0;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    ctx = EVP_MD_CTX_new();
    if (!ctx) {
        close(fd);
        return -ENOMEM;
    }
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
        ret = -EIO;
        goto out;
    }
    for (;;) {
        ssize_t count = read(fd, buffer, sizeof(buffer));

        if (count < 0) {
            if (errno == EINTR)
                continue;
            ret = -errno;
            goto out;
        }
        if (!count)
            break;
        if (EVP_DigestUpdate(ctx, buffer, (size_t)count) != 1) {
            ret = -EIO;
            goto out;
        }
    }
    if (EVP_DigestFinal_ex(ctx, digest, &digest_size) != 1 ||
        digest_size != FW_SHA256_SIZE)
        ret = -EIO;

out:
    EVP_MD_CTX_free(ctx);
    close(fd);
    return ret;
}

static int verify_signature(const struct fw_fit *fit,
                            const struct fw_layout *layout)
{
    pid_t pid;
    int status;

    if (!layout->signature_key[0])
        return layout->require_signature ? -EKEYREJECTED : 0;
    pid = fork();
    if (pid < 0)
        return -errno;
    if (!pid) {
        execl("/usr/bin/fit_check_sign", "fit_check_sign", "-f",
              fit->path, "-k", layout->signature_key, "-c",
              fit->configuration_name, (char *)NULL);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -errno;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status))
        return -EKEYREJECTED;
    return 0;
}

int fw_fit_open(struct fw_fit *fit, const char *path,
                const struct fw_layout *layout)
{
    struct stat st;
    const char *default_configuration;
    int configurations;
    int len;
    int ret;

    if (!fit || !path || !layout)
        return -EINVAL;
    memset(fit, 0, sizeof(*fit));
    fit->fd = -1;
    if (strlen(path) >= sizeof(fit->path))
        return -ENAMETOOLONG;
    strcpy(fit->path, path);
    fit->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fit->fd < 0)
        return -errno;
    if (fstat(fit->fd, &st) < 0) {
        ret = -errno;
        goto error;
    }
    if (st.st_size <= 0) {
        ret = -EINVAL;
        goto error;
    }
    if ((uintmax_t)st.st_size > SIZE_MAX) {
        ret = -EFBIG;
        goto error;
    }
    fit->mapped_size = (size_t)st.st_size;
    fit->data = mmap(NULL, fit->mapped_size, PROT_READ, MAP_PRIVATE,
                     fit->fd, 0);
    if (fit->data == MAP_FAILED) {
        fit->data = NULL;
        ret = -errno;
        goto error;
    }
    ret = fdt_check_header(fit->data);
    if (ret) {
        ret = -EBADMSG;
        goto error;
    }
    fit->fit_size = (size_t)fdt_totalsize(fit->data);
    if (fit->fit_size > fit->mapped_size) {
        ret = -EFBIG;
        goto error;
    }
    configurations = fdt_path_offset(fit->data, "/configurations");
    if (configurations < 0) {
        ret = configurations;
        goto error;
    }
    default_configuration = fdt_getprop(fit->data, configurations,
                                        "default", &len);
    ret = copy_string(fit->configuration_name,
                      sizeof(fit->configuration_name),
                      default_configuration, len);
    if (ret)
        goto error;
    fit->configuration = fdt_subnode_offset(fit->data, configurations,
                                            fit->configuration_name);
    if (fit->configuration < 0) {
        ret = fit->configuration;
        goto error;
    }
    ret = verify_signature(fit, layout);
    if (ret)
        goto error;
    return 0;

error:
    fw_fit_close(fit);
    return ret;
}

void fw_fit_close(struct fw_fit *fit)
{
    if (!fit)
        return;
    if (fit->data)
        munmap(fit->data, fit->mapped_size);
    if (fit->fd >= 0)
        close(fit->fd);
    memset(fit, 0, sizeof(*fit));
    fit->fd = -1;
}

static int get_image_node(const struct fw_fit *fit, const char *name)
{
    int images;

    images = fdt_path_offset(fit->data, "/images");
    if (images < 0)
        return images;
    return fdt_subnode_offset(fit->data, images, name);
}

static int verify_image(const struct fw_fit *fit, int image,
                        const void **data_out, size_t *size_out)
{
    const void *data;
    uint8_t digest[FW_SHA256_SIZE];
    int data_len;
    int hash;
    int found = 0;
    int ret;

    data = fdt_getprop(fit->data, image, "data", &data_len);
    if (!data || data_len < 0)
        return -EOPNOTSUPP;
    fdt_for_each_subnode(hash, fit->data, image) {
        const char *name = fdt_get_name(fit->data, hash, NULL);
        const char *algo;
        const uint8_t *expected;
        int len;

        if (!name || strncmp(name, "hash", 4))
            continue;
        algo = fdt_getprop(fit->data, hash, "algo", &len);
        if (!algo || len != 7 || memcmp(algo, "sha256", 7))
            continue;
        expected = fdt_getprop(fit->data, hash, "value", &len);
        if (!expected || len != FW_SHA256_SIZE)
            return -EBADMSG;
        ret = sha256_data(data, (size_t)data_len, digest);
        if (ret)
            return ret;
        if (memcmp(digest, expected, sizeof(digest)))
            return -EBADMSG;
        found = 1;
    }
    if (!found)
        return -ENOKEY;
    if (data_out)
        *data_out = data;
    if (size_out)
        *size_out = (size_t)data_len;
    return 0;
}

static int verify_config_images(const struct fw_fit *fit, const char *property)
{
    int count;
    int i;

    count = fdt_stringlist_count(fit->data, fit->configuration, property);
    if (count <= 0)
        return count < 0 ? count : -ENOENT;
    for (i = 0; i < count; i++) {
        const char *name;
        int image;
        int len;
        int ret;

        name = fdt_stringlist_get(fit->data, fit->configuration, property,
                                  i, &len);
        if (!name)
            return len;
        image = get_image_node(fit, name);
        if (image < 0)
            return image;
        ret = verify_image(fit, image, NULL, NULL);
        if (ret)
            return ret;
    }
    return 0;
}

static int component_from_name(const char *name,
                               enum fw_component *component)
{
    unsigned int i;

    for (i = 0; i < FW_COMPONENT_COUNT; i++) {
        if (!strcmp(name, fw_component_name(i))) {
            *component = i;
            return 0;
        }
    }
    return -EINVAL;
}

static int verify_identity(const struct fw_layout *layout,
                           const struct fw_fit *fit, const char **type)
{
    const char *product;
    const char *layout_id;
    int ret;

    ret = get_string(fit, fit->configuration, "fw,type", type);
    if (!ret)
        ret = get_string(fit, fit->configuration, "fw,product", &product);
    if (!ret)
        ret = get_string(fit, fit->configuration, "fw,layout-id",
                         &layout_id);
    if (ret)
        return ret;
    if (strcmp(product, layout->product) ||
        strcmp(layout_id, layout->layout_id))
        return -EPERM;
    if (layout->hwid_config[0]) {
        struct dm_hwid_status device;
        ret = dm_hwid_read(layout->hwid_config, &device);
        if (ret) return ret;
        int count = fdt_stringlist_count(fit->data, fit->configuration, "fw,hwids");
        if (count < 1 || count > 256) return -EPERM;
        for (int i = 0; i < count; i++) {
            int length; unsigned char value[8];
            const char *text = fdt_stringlist_get(fit->data, fit->configuration, "fw,hwids", i, &length);
            if (!text || length != 16 || !dm_hwid_parse(text, value)) return -EINVAL;
        }
        if (fdt_stringlist_search(fit->data, fit->configuration, "fw,hwids", device.text) < 0) return -EPERM;
    }
    return 0;
}

int fw_fit_verify_package(const struct fw_layout *layout,
                          const struct fw_fit *fit,
                          struct fw_package *package)
{
    const char *type;
    int ret;

    if (!layout || !fit || !package)
        return -EINVAL;
    ret = verify_identity(layout, fit, &type);
    if (ret)
        return ret;
    ret = component_from_name(type, &package->component);
    if (ret)
        return -EOPNOTSUPP;
    ret = get_u32(fit, fit->configuration, "fw,version", &package->version);
    if (!ret)
        ret = get_u32(fit, fit->configuration, "fw,rollback-index",
                      &package->rollback_index);
    if (!ret) {
        int timestamp_ret = get_u32(fit, fit->configuration,
                                    "fw,build-time-minutes",
                                    &package->build_time_minutes);

        if (timestamp_ret == -FDT_ERR_NOTFOUND)
            package->build_time_minutes = 0;
        else if (timestamp_ret)
            ret = timestamp_ret;
    }
    if (ret)
        return ret;
    if (package->component == FW_COMPONENT_KERNEL) {
        ret = verify_config_images(fit, "kernel");
        if (!ret)
            ret = verify_config_images(fit, "fdt");
        if (!ret && fdt_getprop(fit->data, fit->configuration, "ramdisk", NULL))
            ret = verify_config_images(fit, "ramdisk");
    } else if (package->component == FW_COMPONENT_ROOTFS) {
        ret = verify_config_images(fit, "firmware");
    } else {
        ret = verify_config_images(fit, "firmware");
        if (!ret)
            ret = verify_config_images(fit, "loadables");
    }
    return ret;
}

static int decode_hex(char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

static int parse_sha256(const char *text, int len,
                        uint8_t digest[FW_SHA256_SIZE])
{
    unsigned int i;

    if (!text || len != (int)(FW_SHA256_SIZE * 2U + 1U) ||
        text[len - 1])
        return -EINVAL;
    for (i = 0; i < FW_SHA256_SIZE; i++) {
        int high = decode_hex(text[i * 2U]);
        int low = decode_hex(text[i * 2U + 1U]);

        if (high < 0 || low < 0)
            return -EINVAL;
        digest[i] = (uint8_t)((high << 4) | low);
    }
    return 0;
}

static int release_file_valid(const char *file)
{
    const char *p;

    if (!file || !*file || *file == '/' || strstr(file, ".."))
        return 0;
    for (p = file; *p; p++) {
        if ((*p < 'a' || *p > 'z') && (*p < 'A' || *p > 'Z') &&
            (*p < '0' || *p > '9') && !strchr("._-/", *p))
            return 0;
    }
    return 1;
}

static int read_release_identity(const void *fit, int conf, uint32_t version, struct fw_identity *identity)
{
    int len;
    const char *value = fdt_getprop(fit, conf, "fw,firmware-id", &len);
    memset(identity, 0, sizeof(*identity));
    if (!value) return fdt_getprop(fit, conf, "fw,tag", NULL) ? -EINVAL : 0;
    uint8_t *dst = identity->id;
    if (parse_sha256(value, len, dst)) return -EINVAL;
    char expected[32];
    snprintf(expected, sizeof(expected), "v%u.%u.%u", version >> 16, (version >> 8) & 255, version & 255);
    value = fdt_getprop(fit, conf, "fw,tag", &len);
    if (!value || len != (int)strlen(expected) + 1 || memcmp(value, expected, len)) return -EINVAL;
    strcpy(identity->tag, expected);
    char hex[2][65];
    for (unsigned i = 0; i < 2; i++) {
        char key[48]; fdt64_t bytes;
        snprintf(key, sizeof(key), "fw,%s-payload-bytes", i ? "rootfs" : "kernel");
        const void *raw = fdt_getprop(fit, conf, key, &len);
        if (!raw || len != 8) return -EINVAL;
        memcpy(&bytes, raw, 8); identity->size[i] = fdt64_to_cpu(bytes);
        if (!identity->size[i] || identity->size[i] > 512ULL*1024*1024) return -EINVAL;
        snprintf(key, sizeof(key), "fw,%s-payload-sha256", i ? "rootfs" : "kernel");
        value = fdt_getprop(fit, conf, key, &len); dst = identity->hash[i];
        if (parse_sha256(value, len, dst)) return -EINVAL;
        for (unsigned j = 0; j < 32; j++) snprintf(hex[i]+j*2, 3, "%02x", dst[j]);
    }
    char canonical[256]; uint8_t digest[32];
    int n = snprintf(canonical, sizeof(canonical), "BSPFWCONTENT1\nkernel %llu %s\nrootfs %llu %s\n",
        (unsigned long long)identity->size[0], hex[0], (unsigned long long)identity->size[1], hex[1]);
    if (n < 0 || n >= (int)sizeof(canonical)) return -EINVAL;
    if (sha256_data(canonical, (size_t)n, digest)) return -EIO;
    if (memcmp(identity->id, digest, 32)) return -EBADMSG;
    identity->valid = 1; return 0;
}

int fw_fit_verify_release(const struct fw_layout *layout,
                          const struct fw_fit *fit,
                          struct fw_release *release)
{
    const char *type;
    unsigned int seen = 0;
    int count;
    int i;
    int ret;

    if (!layout || !fit || !release)
        return -EINVAL;
    memset(release, 0, sizeof(*release));
    ret = verify_identity(layout, fit, &type);
    if (ret)
        return ret;
    if (strcmp(type, "release"))
        return -EOPNOTSUPP;
    ret = get_u32(fit, fit->configuration, "fw,version", &release->version);
    if (!ret)
        ret = get_u32(fit, fit->configuration, "fw,rollback-index",
                      &release->rollback_index);
    if (!ret)
        ret = verify_config_images(fit, "firmware");
    if (ret)
        return ret;
    release->allow_bootloader = fdt_getprop(
        fit->data, fit->configuration, "fw,allow-bootloader", NULL) != NULL;
    count = fdt_stringlist_count(fit->data, fit->configuration,
                                 "fw,components");
    if (count <= 0 || count > FW_COMPONENT_COUNT)
        return count < 0 ? count : -E2BIG;
    for (i = 0; i < count; i++) {
        struct fw_release_component *item = &release->component[i];
        const char *name;
        const char *property;
        char key[48];
        int len;

        name = fdt_stringlist_get(fit->data, fit->configuration,
                                  "fw,components", i, &len);
        if (!name)
            return len;
        ret = component_from_name(name, &item->component);
        if (ret)
            return ret;
        if (seen & (1U << item->component))
            return -EEXIST;
        seen |= 1U << item->component;
        snprintf(key, sizeof(key), "fw,%s-file", name);
        property = fdt_getprop(fit->data, fit->configuration, key, &len);
        ret = copy_string(item->file, sizeof(item->file), property, len);
        if (ret || !release_file_valid(item->file))
            return ret ? ret : -EINVAL;
        snprintf(key, sizeof(key), "fw,%s-sha256", name);
        property = fdt_getprop(fit->data, fit->configuration, key, &len);
        ret = parse_sha256(property, len, item->sha256);
        if (ret)
            return ret;
    }
    release->count = (unsigned int)count;
    if ((seen & 6) != 6 && fdt_getprop(fit->data, fit->configuration, "fw,firmware-id", NULL)) return -EINVAL;
    return read_release_identity(fit->data, fit->configuration, release->version, &release->identity);
}

static int get_referenced_image(const struct fw_fit *fit,
                                const char *property, const char *target,
                                const void **data, size_t *size)
{
    int count;
    int i;

    count = fdt_stringlist_count(fit->data, fit->configuration, property);
    if (count < 0)
        return count;
    for (i = 0; i < count; i++) {
        const char *name;
        int image;
        int len;

        name = fdt_stringlist_get(fit->data, fit->configuration, property,
                                  i, &len);
        if (!name)
            return len;
        if (target && strcmp(name, target))
            continue;
        image = get_image_node(fit, name);
        if (image < 0)
            return image;
        return verify_image(fit, image, data, size);
    }
    return -ENOENT;
}

int fw_fit_get_component_data(const struct fw_fit *fit,
                              enum fw_component component,
                              const char *image_name, const void **data,
                              size_t *size)
{
    int ret;

    if (!fit || !data || !size)
        return -EINVAL;
    if (component == FW_COMPONENT_KERNEL) {
        *data = fit->data;
        *size = fit->fit_size;
        return 0;
    }
    ret = get_referenced_image(fit, "firmware", image_name, data, size);
    if (ret == -FDT_ERR_NOTFOUND || ret == -ENOENT)
        ret = get_referenced_image(fit, "loadables", image_name, data, size);
    return ret;
}

/* The fallback kernel must carry its own independent, hash-verified recovery
 * environment; otherwise a bad alternate rootfs could strand the boot chain. */
int fw_fit_recovery_capable(const struct fw_fit *fit)
{
    const void *data;
    size_t size;
    int ret = get_referenced_image(fit, "ramdisk", NULL, &data, &size);
    if (ret || !size)
        return ret ? ret : -EINVAL;
    ret = get_referenced_image(fit, "fdt", NULL, &data, &size);
    if (ret || size < sizeof(struct fdt_header))
        return ret ? ret : -EINVAL;
    /* FIT properties are only 4-byte aligned; libfdt requires an 8-byte base. */
    void *dtb = malloc(size);
    if (!dtb)
        return -ENOMEM;
    memcpy(dtb, data, size);
    ret = -EINVAL;
    if (fdt_check_header(dtb) || fdt_totalsize(dtb) > size)
        goto out;
    int node = fdt_path_offset(dtb, "/chosen/fw-verity"), length;
    const char *hash = fdt_getprop(dtb, node, "payload-sha256", &length);
    if (!hash || length != 65 || hash[64] || strspn(hash, "0123456789abcdef") != 64)
        goto out;
    const fdt32_t *bytes = fdt_getprop(dtb, node, "payload-bytes", &length);
    if (bytes && length == 4 && fdt32_to_cpu(*bytes))
        ret = 0;
out:
    free(dtb);
    return ret;
}

int fw_fit_check_identity(const struct fw_fit *fit, enum fw_component component, const struct fw_identity *identity)
{
    if (!identity->valid || component == FW_COMPONENT_BOOTLOADER) return 0;
    const void *data; size_t bytes; uint8_t digest[32];
    int ret = fw_fit_get_component_data(fit, component, NULL, &data, &bytes);
    if (ret) return ret;
    unsigned i = component == FW_COMPONENT_ROOTFS;
    if (bytes != identity->size[i] || sha256_data(data, bytes, digest) || memcmp(digest, identity->hash[i], 32)) return -EBADMSG;
    return 0;
}

/* Read rootfs payload identity from the hash-verified DTB inside this kernel FIT. */
int fw_fit_rootfs_identity(const struct fw_fit *fit, uint64_t *bytes, uint8_t hash[32])
{
    const void *data;
    size_t size;
    int ret = get_referenced_image(fit, "fdt", NULL, &data, &size);
    if (ret || size < sizeof(struct fdt_header)) return ret ? ret : -EINVAL;
    void *dtb = malloc(size);
    if (!dtb) return -ENOMEM;
    memcpy(dtb, data, size);
    ret = -EBADMSG;
    if (fdt_check_header(dtb) || fdt_totalsize(dtb) > size) goto out;
    int node = fdt_path_offset(dtb, "/chosen/fw-verity"), length;
    const fdt32_t *value = fdt_getprop(dtb, node, "payload-bytes", &length);
    if (!value || length != 4 || !fdt32_to_cpu(*value)) goto out;
    *bytes = fdt32_to_cpu(*value);
    const char *sha = fdt_getprop(dtb, node, "payload-sha256", &length);
    ret = parse_sha256(sha, length, hash);
out:
    free(dtb);
    return ret;
}
