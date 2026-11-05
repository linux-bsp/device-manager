#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "dm_fw_update_priv.h"
#include "dm_fw_fit_priv.h"

static unsigned char media[FW_REGION_COUNT * 1024], before[sizeof(media)];
static const unsigned char payload[] = "rootfs+verity";
static struct fw_package package;
static int fit_error, capability_error, corrupt_readback, fail_write;
static struct fw_layout layout;
int fw_fit_open(struct fw_fit *fit, const char *path, const struct fw_layout *l)
{ (void)fit; (void)path; (void)l; return 0; }
void fw_fit_close(struct fw_fit *fit) { (void)fit; }
int fw_fit_verify_package(const struct fw_layout *l, const struct fw_fit *fit, struct fw_package *p)
{ (void)l; (void)fit; *p=package; return fit_error; }
int fw_fit_recovery_capable(const struct fw_fit *fit) { (void)fit; return capability_error; }
int fw_fit_get_component_data(const struct fw_fit *fit, enum fw_component c, const char *name, const void **data, size_t *size)
{ (void)fit; (void)c; (void)name; *data=payload; *size=sizeof(payload); return 0; }
static int read_media(struct fw_storage *s, uint64_t offset, void *data, size_t size)
{
    (void)s; assert(offset+size<=sizeof(media)); memcpy(data,media+offset,size);
    if (corrupt_readback && offset==layout.region[FW_REGION_ROOTFS_A].offset) ((unsigned char *)data)[0]^=1;
    return 0;
}
static int write_media(struct fw_storage *s, uint64_t offset, const void *data, size_t size)
{
    (void)s; assert(offset+size<=sizeof(media));
    if (fail_write) return -EIO;
    memcpy(media+offset,data,size); return 0;
}
static int sync_media(struct fw_storage *s) { (void)s; return 0; }
int main(void)
{
    struct fw_metadata metadata;
    const struct fw_storage_ops ops={.read=read_media,.write=write_media,.sync=sync_media};
    struct fw_storage storage={.layout=&layout,.ops=&ops,.info={.capacity=sizeof(media),.write_size=1}};
    fw_layout_defaults(&layout); strcpy(layout.work_dir,"/tmp");
    for (unsigned i=0;i<FW_REGION_COUNT;i++) { layout.region[i].offset=i*1024; layout.region[i].size=1024; }
    fw_metadata_init(&metadata,0);
    metadata.deployment[0].component[FW_COMPONENT_ROOTFS].rollback_index=3;
    metadata.deployment[0].component[FW_COMPONENT_ROOTFS].version=8;
    metadata.deployment[0].release_version=8;
    assert(!fw_metadata_save(&storage,&metadata));
    uint8_t hash[32], wrong[32]={0}; unsigned length;
    assert(EVP_Digest(payload,sizeof(payload),hash,&length,EVP_sha256(),NULL));
    package.component=FW_COMPONENT_ROOTFS; package.version=8; package.rollback_index=3;
    memcpy(before,media,sizeof(media));
    assert(fw_restore_rootfs(&layout,&storage,0,"test.itb",sizeof(payload),wrong,false)==-EBADMSG);
    assert(fw_restore_rootfs(&layout,&storage,0,"test.itb",sizeof(payload)+1,hash,false)==-EBADMSG);
    assert(fw_restore_rootfs(&layout,&storage,1,"test.itb",sizeof(payload),hash,false)==-EPERM);
    package.rollback_index=2;
    assert(fw_restore_rootfs(&layout,&storage,0,"test.itb",sizeof(payload),hash,false)==-EPERM);
    package.rollback_index=3; package.component=FW_COMPONENT_KERNEL;
    assert(fw_restore_rootfs(&layout,&storage,0,"test.itb",sizeof(payload),hash,false)==-EPERM);
    package.component=FW_COMPONENT_ROOTFS; fit_error=-EBADMSG;
    assert(fw_restore_rootfs(&layout,&storage,0,"test.itb",sizeof(payload),hash,false)==-EBADMSG);
    assert(!memcmp(before,media,sizeof(media)));
    fit_error=0;
    assert(!fw_restore_rootfs(&layout,&storage,0,"test.itb",sizeof(payload),hash,false));
    size_t offset=layout.region[FW_REGION_ROOTFS_A].offset;
    assert(!memcmp(media+offset,payload,sizeof(payload)));
    memcpy(before+offset,payload,sizeof(payload));
    assert(!memcmp(before,media,sizeof(media))); /* Metadata, other slot, kernel untouched. */
    corrupt_readback=1;
    assert(fw_restore_rootfs(&layout,&storage,0,"test.itb",sizeof(payload),hash,false)==-EIO);
    corrupt_readback=0; fail_write=1;
    assert(fw_restore_rootfs(&layout,&storage,0,"test.itb",sizeof(payload),hash,false)==-EIO);
    fail_write=0;
    assert(fw_validate_recovery_kernel(&layout,&storage,&metadata,0)==-EINVAL);
    uint32_t header[]={htonl(0xd00dfeed),htonl(40)};
    memcpy(media+layout.region[FW_REGION_KERNEL_A].offset,header,sizeof(header));
    package.component=FW_COMPONENT_KERNEL;
    assert(!fw_validate_recovery_kernel(&layout,&storage,&metadata,0));
    capability_error=-ENOENT;
    assert(fw_validate_recovery_kernel(&layout,&storage,&metadata,0)==-ENOENT);
    capability_error=0; fit_error=-EBADMSG;
    assert(fw_validate_recovery_kernel(&layout,&storage,&metadata,0)==-EBADMSG);
    struct fw_metadata untouched=metadata;
    assert(fw_metadata_fallback(&metadata,0)==-ENOENT);
    assert(!memcmp(&metadata,&untouched,sizeof(metadata)));
    metadata.deployment[1]=metadata.deployment[0]; metadata.deployment[1].state=FW_STATE_CONFIRMED;
    metadata.deployment[1].successful=1;
    assert(!fw_metadata_fallback(&metadata,0));
    assert(metadata.active_deployment==1 && metadata.selected_deployment==1 && metadata.last_good_deployment==1);
    assert(metadata.deployment[0].state==FW_STATE_FAILED && !metadata.deployment[0].successful);
    untouched=metadata;
    assert(fw_metadata_fallback(&metadata,1)==-ENOENT); /* Never bounce to known failed slot. */
    assert(!memcmp(&metadata,&untouched,sizeof(metadata)));
    /* Standby repair must not write a shared running slot or a stale payload. */
    fit_error=0; package.component=FW_COMPONENT_ROOTFS;
    assert(!fw_metadata_save(&storage,&metadata));
    assert(fw_restore_rootfs(&layout,&storage,0,"test.itb",sizeof(payload),hash,true)==-EPERM);
    metadata.deployment[0].component[FW_COMPONENT_ROOTFS].slot=1;
    metadata.deployment[0].identity.valid=1;
    metadata.deployment[0].identity.size[1]=sizeof(payload);
    memcpy(metadata.deployment[0].identity.hash[1],hash,32);
    assert(!fw_metadata_save(&storage,&metadata));
    assert(fw_restore_rootfs(&layout,&storage,0,"test.itb",sizeof(payload),wrong,true)==-ESTALE);
    metadata.pending_deployment=0;
    assert(!fw_metadata_save(&storage,&metadata));
    assert(fw_restore_rootfs(&layout,&storage,0,"test.itb",sizeof(payload),hash,true)==-EBUSY);
    metadata.pending_deployment=FW_SLOT_NONE;
    assert(!fw_metadata_save(&storage,&metadata));
    char file[]="/tmp/fw-recovery-claim-XXXXXX";
    storage.fd=mkstemp(file); assert(storage.fd>=0); unlink(file);
    memcpy(before,media,sizeof(media));
    assert(!fw_restore_rootfs(&layout,&storage,0,"test.itb",sizeof(payload),hash,true));
    offset=layout.region[FW_REGION_ROOTFS_B].offset;
    memcpy(before+offset,payload,sizeof(payload));
    assert(!memcmp(before,media,sizeof(media)));
    assert(!fw_metadata_load(&storage,&metadata));
    assert(metadata.deployment[0].state==FW_STATE_FAILED);
    close(storage.fd);
    return 0;
}
