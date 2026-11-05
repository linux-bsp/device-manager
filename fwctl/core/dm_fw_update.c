// SPDX-License-Identifier: GPL-2.0+
#include "dm_fw_update_priv.h"

#include "dm_fw_fit_priv.h"
#include "dm_fw_source_priv.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int write_region(struct fw_storage *storage, const struct fw_region *region,
                         const void *data, size_t size)
{
    int claim = -1;
    if (storage->fd >= 0) {
        struct stat st;
        if (fstat(storage->fd, &st)) return -errno;
        if (S_ISBLK(st.st_mode)) {
            int ret = fw_storage_claim_region(storage, region, &claim);
            if (ret) return ret;
        }
    }
    int ret = fw_storage_write_region(storage, region, data, size);
    if (claim >= 0) close(claim);
    return ret;
}

static void report_progress(dm_fw_progress_fn progress, void *userdata,
                            enum dm_fw_progress_event event,
                            enum dm_fw_component component,
                            uint32_t deployment, uint32_t release_version,
                            uint32_t component_version, uint32_t attempts)
{
    struct dm_fw_progress value = {
        .event = event,
        .component = component,
        .deployment = deployment,
        .release_version = release_version,
        .component_version = component_version,
        .attempts = attempts,
    };

    if (progress)
        progress(&value, userdata);
}

static int write_bootloader(const struct fw_layout *layout,
                            struct fw_storage *storage,
                            const struct fw_fit *fit, uint8_t slot)
{
    static const struct {
        const char *name;
        enum fw_region_id region_a;
    } images[] = {
        { "uboot", FW_REGION_UBOOT_A },
        { "tispl", FW_REGION_TISPL_A },
        { "tiboot3", FW_REGION_TIBOOT3_A },
    };
    unsigned int i;

    for (i = 0; i < sizeof(images) / sizeof(images[0]); i++) {
        const void *data;
        size_t size;
        int ret;

        ret = fw_fit_get_component_data(fit, FW_COMPONENT_BOOTLOADER,
                                        images[i].name, &data, &size);
        if (ret)
            return ret;
        ret = write_region(
            storage, &layout->region[images[i].region_a + slot], data, size);
        if (ret)
            return ret;
    }
    return 0;
}

static int write_component(const struct fw_layout *layout,
                           struct fw_storage *storage,
                           struct fw_metadata *metadata, uint8_t deployment,
                           const struct fw_package *package,
                           const struct fw_fit *fit)
{
    struct fw_component_slot *current;
    const struct fw_region *region;
    const void *data;
    size_t size;
    uint8_t target;
    int ret;

    current = &metadata->deployment[1U - deployment]
                   .component[package->component];
    target = 1U - current->slot;
    if (package->rollback_index < current->rollback_index)
        return -EPERM;

    if (package->component == FW_COMPONENT_BOOTLOADER) {
        ret = write_bootloader(layout, storage, fit, target);
    } else {
        ret = fw_fit_get_component_data(fit, package->component, NULL,
                                        &data, &size);
        if (ret)
            return ret;
        region = fw_layout_component_region(layout, package->component,
                                            target);
        if (!region)
            return -ENOENT;
        ret = write_region(storage, region, data, size);
    }
    if (ret)
        return ret;
    return fw_metadata_stage_component(metadata, deployment,
                                       package->component, target,
                                       package->version,
                                       package->rollback_index,
                                       package->build_time_minutes);
}

int fw_update_release(const struct fw_layout *layout,
                      struct fw_storage *storage,
                      const char *release_source,
                      uint32_t component,
                      dm_fw_progress_fn progress, void *userdata)
{
    struct fw_artifact release_artifact = { 0 };
    struct fw_artifact component_artifact = { 0 };
    struct fw_metadata metadata;
    struct fw_release release;
    struct fw_fit release_fit;
    uint8_t deployment;
    unsigned int i;
    unsigned int matched = 0;
    int ret;

    if (component > DM_FW_COMPONENT_COUNT) return -EINVAL;

    ret = fw_source_get_release(layout, release_source, &release_artifact);
    if (ret)
        return ret;
    ret = fw_fit_open(&release_fit, release_artifact.path, layout);
    if (ret)
        goto out_release_artifact;
    ret = fw_fit_verify_release(layout, &release_fit, &release);
    if (ret)
        goto out_release_fit;
    /* Reject an absent component or prohibited bootloader before metadata is
     * changed. A rejected request must not invalidate an existing candidate. */
    for (i = 0; i < release.count; i++) {
        if (component != DM_FW_UPDATE_COMPONENT_ALL && component != release.component[i].component + 1U) continue;
        if (release.component[i].component == FW_COMPONENT_BOOTLOADER &&
            (!layout->allow_bootloader_update || !release.allow_bootloader)) {
            ret = -EPERM; goto out_release_fit;
        }
        ++matched;
    }
    if (!matched) { ret = -ENOENT; goto out_release_fit; }
    ret = fw_metadata_load(storage, &metadata);
    if (ret)
        goto out_release_fit;
    ret = component == DM_FW_UPDATE_COMPONENT_ALL ?
        fw_metadata_begin_release(&metadata, release.version, &deployment) :
        fw_metadata_begin_component(&metadata, component - 1U, &deployment);
    if (ret)
        goto out_release_fit;
    if (component != DM_FW_UPDATE_COMPONENT_ALL && component != FW_COMPONENT_BOOTLOADER + 1U)
        metadata.deployment[deployment].release_version = release.version;

    /* Persist a non-bootable transaction before any target region changes. */
    ret = fw_metadata_save(storage, &metadata);
    if (ret)
        goto out_release_fit;
    report_progress(progress, userdata, DM_FW_PROGRESS_STARTED,
                    DM_FW_COMPONENT_BOOTLOADER, deployment, release.version,
                    0, 0);

    for (i = 0; i < release.count; i++) {
        const struct fw_release_component *item = &release.component[i];
        struct fw_package package;
        struct fw_fit component_fit;
        uint8_t digest[FW_SHA256_SIZE];

        if (component != DM_FW_UPDATE_COMPONENT_ALL &&
            component != item->component + 1U)
            continue;
        ret = fw_source_get_component(layout, release_source, item->file,
                                      &component_artifact);
        if (ret)
            goto out_release_fit;
        ret = fw_sha256_file(component_artifact.path, digest);
        if (ret || memcmp(digest, item->sha256, sizeof(digest))) {
            if (!ret)
                ret = -EBADMSG;
            goto out_component_artifact;
        }
        ret = fw_fit_open(&component_fit, component_artifact.path, layout);
        if (ret)
            goto out_component_artifact;
        ret = fw_fit_verify_package(layout, &component_fit, &package);
        if (!ret && (package.component != item->component ||
                     package.rollback_index < release.rollback_index))
            ret = -EPERM;
        if (!ret && release.identity.valid && package.component != FW_COMPONENT_BOOTLOADER &&
            package.version != release.version)
            ret = -EBADMSG;
        if (!ret) ret = fw_fit_check_identity(&component_fit, package.component, &release.identity);
        if (!ret)
            ret = write_component(layout, storage, &metadata, deployment,
                                  &package, &component_fit);
        fw_fit_close(&component_fit);
        if (ret)
            goto out_component_artifact;
        report_progress(progress, userdata, DM_FW_PROGRESS_COMPONENT_STAGED,
                        (enum dm_fw_component)package.component, deployment,
                        release.version,
                        package.version, 0);
        fw_source_put(&component_artifact);
    }

    if (component == DM_FW_UPDATE_COMPONENT_ALL) metadata.deployment[deployment].identity = release.identity;
    ret = fw_metadata_commit_release(&metadata, deployment,
                                     layout->attempts);
    if (!ret)
        ret = fw_metadata_save(storage, &metadata);
    if (!ret)
        report_progress(progress, userdata, DM_FW_PROGRESS_READY,
                        DM_FW_COMPONENT_BOOTLOADER, deployment,
                        release.version, 0, layout->attempts);
    goto out_release_fit;

out_component_artifact:
    fw_source_put(&component_artifact);
out_release_fit:
    fw_fit_close(&release_fit);
out_release_artifact:
    fw_source_put(&release_artifact);
    return ret;
}
