// SPDX-License-Identifier: GPL-2.0+
#define _DEFAULT_SOURCE
#include "dm_fw_metadata_priv.h"

#include "dm_fw_storage_priv.h"

#include <endian.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define FW_METADATA_MAGIC 0x46574d44U
#define FW_METADATA_VERSION 1U
#define FW_BUILD_TIME_MAGIC 0x314d5442U

struct fw_disk_component {
    uint8_t slot;
    uint8_t valid;
    uint16_t reserved;
    uint32_t version;
    uint32_t rollback_index;
} __attribute__((packed));

struct fw_disk_deployment {
    struct fw_disk_component component[FW_COMPONENT_COUNT];
    uint8_t state;
    uint8_t tries_remaining;
    uint8_t successful;
    uint8_t reserved;
    uint32_t release_version;
} __attribute__((packed));

struct fw_disk_identity {
    uint32_t valid;
    char tag[32];
    uint8_t id[32];
    uint64_t size[2];
    uint8_t hash[2][32];
} __attribute__((packed));

struct fw_disk_metadata {
    uint32_t magic;
    uint16_t format_version;
    uint16_t header_size;
    uint32_t sequence;
    uint8_t active_deployment;
    uint8_t pending_deployment;
    uint8_t last_good_deployment;
    uint8_t boot_once_deployment;
    uint8_t update_state;
    uint8_t selected_deployment;
    uint8_t reserved0[2];
    struct fw_disk_deployment deployment[2];
    uint8_t reserved1[16];
    uint32_t crc32;
    uint32_t identity_magic;
    struct fw_disk_identity identity[2];
    uint8_t identity_reserved[3664];
    uint32_t identity_crc32;
} __attribute__((packed));

_Static_assert(sizeof(struct fw_disk_metadata) == FW_METADATA_SIZE,
               "firmware metadata size mismatch");

static uint32_t fw_crc32(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t i;
    int bit;

    crc = ~crc;
    for (i = 0; i < len; i++) {
        crc ^= p[i];
        for (bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320U &
                  (uint32_t)-(int32_t)(crc & 1U));
    }
    return ~crc;
}

const char *fw_component_name(enum fw_component component)
{
    static const char *const names[] = {
        "bootloader", "kernel", "rootfs",
    };

    return component < FW_COMPONENT_COUNT ? names[component] : "invalid";
}

const char *fw_state_name(enum fw_deployment_state state)
{
    static const char *const names[] = {
        "empty", "writing", "ready", "boot-testing", "confirmed",
        "failed",
    };

    return state <= FW_STATE_FAILED ? names[state] : "invalid";
}

int fw_deployment_is_valid(const struct fw_deployment *deployment)
{
    unsigned int i;

    if (!deployment || deployment->state == FW_STATE_EMPTY ||
        deployment->state == FW_STATE_FAILED)
        return 0;
    for (i = 0; i < FW_COMPONENT_COUNT; i++) {
        if (!deployment->component[i].valid ||
            deployment->component[i].slot > 1)
            return 0;
    }
    return 1;
}

void fw_metadata_init(struct fw_metadata *metadata, uint8_t slot)
{
    struct fw_deployment *deployment;
    unsigned int i;

    memset(metadata, 0, sizeof(*metadata));
    metadata->pending_deployment = FW_SLOT_NONE;
    metadata->boot_once_deployment = FW_SLOT_NONE;
    metadata->source_copy = 1;
    metadata->update_state = FW_STATE_CONFIRMED;

    deployment = &metadata->deployment[0];
    deployment->state = FW_STATE_CONFIRMED;
    deployment->successful = 1;
    for (i = 0; i < FW_COMPONENT_COUNT; i++) {
        deployment->component[i].slot = slot;
        deployment->component[i].valid = 1;
    }
}

static int fw_metadata_decode(const void *buf, struct fw_metadata *metadata)
{
    const struct fw_disk_metadata *disk = buf;
    uint32_t expected_crc;
    uint32_t actual_crc;
    unsigned int i;
    unsigned int j;
    uint32_t build_time_magic;
    uint16_t build_time_high[2][FW_COMPONENT_COUNT];

    if (le32toh(disk->magic) != FW_METADATA_MAGIC ||
        le16toh(disk->format_version) != FW_METADATA_VERSION ||
        le16toh(disk->header_size) != 128U)
        return -EINVAL;
    expected_crc = le32toh(disk->crc32);
    actual_crc = fw_crc32(0, buf, offsetof(struct fw_disk_metadata, crc32));
    if (actual_crc != expected_crc)
        return -EBADMSG;

    /* New records require the extension even when a torn write leaves it erased. */
    if ((disk->reserved0[0] || disk->reserved0[1]) &&
        (disk->reserved0[0] != 'I' || disk->reserved0[1] != 'D' ||
         le32toh(disk->identity_magic) != 0x31444946U))
        return -EBADMSG;
    if (le32toh(disk->identity_magic) == 0x31444946U) {
        if (le32toh(disk->identity_crc32) != fw_crc32(0, buf, offsetof(struct fw_disk_metadata, identity_crc32))) return -EBADMSG;
    } else if (le32toh(disk->identity_magic) != 0 && le32toh(disk->identity_magic) != 0xffffffffU) return -EBADMSG;
    memset(metadata, 0, sizeof(*metadata));
    metadata->sequence = le32toh(disk->sequence);
    metadata->active_deployment = disk->active_deployment;
    metadata->pending_deployment = disk->pending_deployment;
    metadata->last_good_deployment = disk->last_good_deployment;
    metadata->boot_once_deployment = disk->boot_once_deployment;
    metadata->selected_deployment = disk->selected_deployment;
    metadata->update_state = disk->update_state;
    memcpy(&build_time_magic, disk->reserved1, sizeof(build_time_magic));
    memcpy(build_time_high, disk->reserved1 + sizeof(build_time_magic),
           sizeof(build_time_high));
    for (i = 0; i < 2; i++) {
        struct fw_deployment *dst = &metadata->deployment[i];
        const struct fw_disk_deployment *src = &disk->deployment[i];

        if (le32toh(disk->identity_magic) == 0x31444946U) {
            const struct fw_disk_identity *identity = &disk->identity[i];
            dst->identity.valid = le32toh(identity->valid);
            if (dst->identity.valid > 1 || (dst->identity.valid && !memchr(identity->tag, 0, sizeof(identity->tag)))) return -EINVAL;
            memcpy(dst->identity.tag, identity->tag, 32);
            memcpy(dst->identity.id, identity->id, 32);
            memcpy(dst->identity.hash, identity->hash, 64);
            dst->identity.size[0] = le64toh(identity->size[0]);
            dst->identity.size[1] = le64toh(identity->size[1]);
        }
        dst->state = src->state;
        dst->tries_remaining = src->tries_remaining;
        dst->successful = src->successful;
        dst->release_version = le32toh(src->release_version);
        for (j = 0; j < FW_COMPONENT_COUNT; j++) {
            dst->component[j].slot = src->component[j].slot;
            dst->component[j].valid = src->component[j].valid;
            dst->component[j].version = le32toh(src->component[j].version);
            dst->component[j].rollback_index =
                le32toh(src->component[j].rollback_index);
            if (le32toh(build_time_magic) == FW_BUILD_TIME_MAGIC)
                dst->build_time_minutes[j] =
                    ((uint32_t)le16toh(build_time_high[i][j]) << 16) |
                    le16toh(src->component[j].reserved);
        }
    }

    if (metadata->active_deployment > 1 ||
        metadata->last_good_deployment > 1 ||
        (metadata->pending_deployment != FW_SLOT_NONE &&
         metadata->pending_deployment > 1) ||
        (metadata->boot_once_deployment != FW_SLOT_NONE &&
         metadata->boot_once_deployment > 1) ||
        metadata->selected_deployment > 1 ||
        metadata->update_state > FW_STATE_FAILED)
        return -EINVAL;
    return 0;
}

static void fw_metadata_encode(const struct fw_metadata *metadata, void *buf)
{
    struct fw_disk_metadata *disk = buf;
    unsigned int i;
    unsigned int j;
    uint32_t build_time_magic = htole32(FW_BUILD_TIME_MAGIC);
    uint16_t build_time_high[2][FW_COMPONENT_COUNT];

    memset(disk, 0, sizeof(*disk));
    disk->magic = htole32(FW_METADATA_MAGIC);
    disk->format_version = htole16(FW_METADATA_VERSION);
    disk->header_size = htole16(128U);
    disk->sequence = htole32(metadata->sequence);
    disk->active_deployment = metadata->active_deployment;
    disk->pending_deployment = metadata->pending_deployment;
    disk->last_good_deployment = metadata->last_good_deployment;
    disk->boot_once_deployment = metadata->boot_once_deployment;
    disk->selected_deployment = metadata->selected_deployment;
    disk->update_state = metadata->update_state;
    disk->reserved0[0] = 'I';
    disk->reserved0[1] = 'D';
    memset(build_time_high, 0, sizeof(build_time_high));
    for (i = 0; i < 2; i++) {
        const struct fw_deployment *src = &metadata->deployment[i];
        struct fw_disk_deployment *dst = &disk->deployment[i];

        disk->identity[i].valid = htole32(src->identity.valid);
        memcpy(disk->identity[i].tag, src->identity.tag, 32);
        memcpy(disk->identity[i].id, src->identity.id, 32);
        memcpy(disk->identity[i].hash, src->identity.hash, 64);
        disk->identity[i].size[0] = htole64(src->identity.size[0]);
        disk->identity[i].size[1] = htole64(src->identity.size[1]);
        dst->state = src->state;
        dst->tries_remaining = src->tries_remaining;
        dst->successful = src->successful;
        dst->release_version = htole32(src->release_version);
        for (j = 0; j < FW_COMPONENT_COUNT; j++) {
            dst->component[j].slot = src->component[j].slot;
            dst->component[j].valid = src->component[j].valid;
            dst->component[j].version = htole32(src->component[j].version);
            dst->component[j].rollback_index =
                htole32(src->component[j].rollback_index);
            dst->component[j].reserved = htole16(
                src->build_time_minutes[j] & UINT16_MAX);
            build_time_high[i][j] = htole16(
                src->build_time_minutes[j] >> 16);
        }
    }
    memcpy(disk->reserved1, &build_time_magic, sizeof(build_time_magic));
    memcpy(disk->reserved1 + sizeof(build_time_magic), build_time_high,
           sizeof(build_time_high));
    disk->crc32 = htole32(fw_crc32(
        0, disk, offsetof(struct fw_disk_metadata, crc32)));
    disk->identity_magic = htole32(0x31444946U);
    disk->identity_crc32 = htole32(fw_crc32(0, buf, offsetof(struct fw_disk_metadata, identity_crc32)));
}

static int sequence_newer(uint32_t lhs, uint32_t rhs)
{
    return (int32_t)(lhs - rhs) > 0;
}

int fw_metadata_load(struct fw_storage *storage,
                     struct fw_metadata *metadata)
{
    struct fw_metadata copy[FW_METADATA_COPIES];
    uint8_t buf[FW_METADATA_SIZE];
    int valid[FW_METADATA_COPIES] = { 0, 0 };
    unsigned int i;
    int ret;

    if (!storage || !metadata)
        return -EINVAL;
    for (i = 0; i < FW_METADATA_COPIES; i++) {
        ret = fw_storage_read_metadata(storage, i, buf, sizeof(buf));
        if (!ret && !fw_metadata_decode(buf, &copy[i]))
            valid[i] = 1;
    }
    if (!valid[0] && !valid[1])
        return -ENODATA;
    i = valid[1] && (!valid[0] ||
        sequence_newer(copy[1].sequence, copy[0].sequence));
    *metadata = copy[i];
    metadata->source_copy = i;
    return 0;
}

int fw_metadata_save(struct fw_storage *storage,
                     struct fw_metadata *metadata)
{
    struct fw_metadata check;
    uint8_t buf[FW_METADATA_SIZE];
    uint8_t verify[FW_METADATA_SIZE];
    unsigned int target;
    int ret;

    if (!storage || !metadata)
        return -EINVAL;
    target = 1U - metadata->source_copy;
    metadata->sequence++;
    fw_metadata_encode(metadata, buf);
    ret = fw_storage_write_metadata(storage, target, buf, sizeof(buf));
    if (ret)
        goto restore_sequence;
    ret = fw_storage_read_metadata(storage, target, verify, sizeof(verify));
    if (ret)
        goto restore_sequence;
    ret = fw_metadata_decode(verify, &check);
    if (ret || check.sequence != metadata->sequence) {
        if (!ret)
            ret = -EIO;
        goto restore_sequence;
    }
    metadata->source_copy = target;
    return 0;

restore_sequence:
    metadata->sequence--;
    return ret;
}

int fw_metadata_check_update_source(const struct fw_metadata *metadata, uint32_t booted)
{
    if (!metadata || booted > 1 || booted != metadata->active_deployment)
        return -EBUSY;
    const struct fw_deployment *source = &metadata->deployment[booted];
    return source->successful && source->state == FW_STATE_CONFIRMED &&
        fw_deployment_is_valid(source) ? 0 : -EBUSY;
}

int fw_metadata_begin_component(struct fw_metadata *metadata,
    enum fw_component component, uint8_t *deployment)
{
    if (!metadata || !deployment || component >= FW_COMPONENT_COUNT ||
        metadata->active_deployment > 1 || metadata->last_good_deployment > 1) return -EINVAL;
    uint8_t source_id = metadata->active_deployment;
    struct fw_deployment *source = &metadata->deployment[source_id];
    if (!source->successful || !fw_deployment_is_valid(source)) {
        source_id = metadata->last_good_deployment;
        source = &metadata->deployment[source_id];
    }
    if (!source->successful || source->state != FW_STATE_CONFIRMED ||
        !fw_deployment_is_valid(source)) return -ENOENT;
    uint8_t target_id = 1U - source_id;
    struct fw_deployment *target = &metadata->deployment[target_id];
    if (metadata->pending_deployment != FW_SLOT_NONE) {
        if (metadata->pending_deployment != target_id || target->state != FW_STATE_READY ||
            !fw_deployment_is_valid(target)) return -EBUSY;
    } else *target = *source;
    target->state = FW_STATE_WRITING;
    target->tries_remaining = 0;
    target->successful = 0;
    if (component != FW_COMPONENT_BOOTLOADER) memset(&target->identity, 0, sizeof(target->identity));
    metadata->pending_deployment = FW_SLOT_NONE;
    metadata->boot_once_deployment = FW_SLOT_NONE;
    metadata->selected_deployment = source_id;
    metadata->update_state = FW_STATE_WRITING;
    *deployment = target_id;
    return 0;
}

int fw_metadata_begin_release(struct fw_metadata *metadata,
                              uint32_t release_version,
                              uint8_t *deployment)
{
    struct fw_deployment *source;
    struct fw_deployment *target;
    uint8_t source_id;
    uint8_t target_id;

    if (!metadata || !release_version || !deployment ||
        metadata->active_deployment > 1 || metadata->last_good_deployment > 1)
        return -EINVAL;
    source_id = metadata->active_deployment;
    source = &metadata->deployment[source_id];
    if (!source->successful || !fw_deployment_is_valid(source)) {
        source_id = metadata->last_good_deployment;
        source = &metadata->deployment[source_id];
    }
    if (!source->successful || !fw_deployment_is_valid(source))
        return -ENOENT;

    target_id = 1U - source_id;
    if (metadata->pending_deployment != FW_SLOT_NONE &&
        metadata->pending_deployment != target_id)
        return -EINVAL;
    target = &metadata->deployment[target_id];
    *target = *source;
    memset(&target->identity, 0, sizeof(target->identity));
    target->state = FW_STATE_WRITING;
    target->tries_remaining = 0;
    target->successful = 0;
    target->release_version = release_version;
    metadata->pending_deployment = FW_SLOT_NONE;
    metadata->boot_once_deployment = FW_SLOT_NONE;
    metadata->selected_deployment = source_id;
    metadata->update_state = FW_STATE_WRITING;
    *deployment = target_id;
    return 0;
}

int fw_metadata_stage_component(struct fw_metadata *metadata,
                                uint8_t deployment,
                                enum fw_component component, uint8_t slot,
                                uint32_t version, uint32_t rollback_index,
                                uint32_t build_time_minutes)
{
    struct fw_component_slot *target;

    if (!metadata || deployment > 1 || component >= FW_COMPONENT_COUNT ||
        slot > 1 ||
        metadata->deployment[deployment].state != FW_STATE_WRITING)
        return -EINVAL;
    target = &metadata->deployment[deployment].component[component];
    target->slot = slot;
    target->valid = 1;
    target->version = version;
    target->rollback_index = rollback_index;
    metadata->deployment[deployment].build_time_minutes[component] =
        build_time_minutes;
    return 0;
}

int fw_metadata_commit_release(struct fw_metadata *metadata,
                               uint8_t deployment, uint8_t attempts)
{
    struct fw_deployment *target;

    if (!metadata || deployment > 1 || !attempts)
        return -EINVAL;
    target = &metadata->deployment[deployment];
    if (target->state != FW_STATE_WRITING ||
        !fw_deployment_is_valid(target))
        return -EINVAL;
    target->state = FW_STATE_READY;
    target->tries_remaining = attempts;
    target->successful = 0;
    metadata->pending_deployment = deployment;
    metadata->update_state = FW_STATE_READY;
    return 0;
}

int fw_metadata_mark_good(struct fw_metadata *metadata, int deployment)
{
    struct fw_deployment *target;

    if (!metadata)
        return -EINVAL;
    if (deployment < 0)
        deployment = metadata->pending_deployment;
    if (deployment < 0 || deployment > 1)
        return -EINVAL;
    target = &metadata->deployment[deployment];
    if (target->state == FW_STATE_WRITING || !fw_deployment_is_valid(target))
        return -EINVAL;
    target->state = FW_STATE_CONFIRMED;
    target->successful = 1;
    target->tries_remaining = 0;
    metadata->active_deployment = deployment;
    metadata->last_good_deployment = deployment;
    metadata->selected_deployment = deployment;
    metadata->pending_deployment = FW_SLOT_NONE;
    metadata->update_state = FW_STATE_CONFIRMED;
    return 0;
}

int fw_metadata_mark_bad(struct fw_metadata *metadata, uint8_t deployment)
{
    if (!metadata || deployment > 1)
        return -EINVAL;
    metadata->deployment[deployment].state = FW_STATE_FAILED;
    metadata->deployment[deployment].successful = 0;
    metadata->deployment[deployment].tries_remaining = 0;
    if (metadata->pending_deployment == deployment)
        metadata->pending_deployment = FW_SLOT_NONE;
    if (metadata->active_deployment == deployment)
        metadata->active_deployment = metadata->last_good_deployment;
    if (metadata->selected_deployment == deployment)
        metadata->selected_deployment = metadata->last_good_deployment;
    metadata->update_state = FW_STATE_FAILED;
    return 0;
}

int fw_metadata_rollback(struct fw_metadata *metadata)
{
    uint8_t target;

    if (!metadata)
        return -EINVAL;
    target = metadata->last_good_deployment;
    if (target > 1 || !metadata->deployment[target].successful ||
        !fw_deployment_is_valid(&metadata->deployment[target]))
        return -ENOENT;
    if (metadata->pending_deployment != FW_SLOT_NONE)
        metadata->deployment[metadata->pending_deployment].state =
            FW_STATE_FAILED;
    metadata->pending_deployment = FW_SLOT_NONE;
    metadata->boot_once_deployment = FW_SLOT_NONE;
    metadata->active_deployment = target;
    metadata->selected_deployment = target;
    metadata->update_state = FW_STATE_CONFIRMED;
    return 0;
}

/* Never switch only rootfs: select the complete confirmed deployment and reboot.
 * If no other good deployment exists, leave metadata usable for exact repair. */
int fw_metadata_fallback(struct fw_metadata *metadata, uint8_t failed)
{
    if (!metadata || failed > 1) return -EINVAL;
    uint8_t other = 1U - failed;
    const struct fw_deployment *target = &metadata->deployment[other];
    if (target->state != FW_STATE_CONFIRMED || !target->successful ||
        !fw_deployment_is_valid(target)) return -ENOENT;
    metadata->last_good_deployment = other;
    int ret = fw_metadata_mark_bad(metadata, failed);
    if (ret) return ret;
    metadata->active_deployment = other;
    metadata->selected_deployment = other;
    metadata->pending_deployment = FW_SLOT_NONE;
    metadata->boot_once_deployment = FW_SLOT_NONE;
    metadata->update_state = FW_STATE_CONFIRMED;
    return 0;
}
