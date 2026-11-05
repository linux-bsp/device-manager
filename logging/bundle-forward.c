#define _GNU_SOURCE
#include "bundle-forward.h"
#include "archive.h"
#include "retention.h"
#include "dm/transfer.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct exchange {
    char path[512];
    startup_post_fn post;
};

static int safe(const char *s)
{
    if (!*s || strlen(s) > 63) return 0;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') ||
              *s == '-' || *s == '_')) return 0;
    return 1;
}

static int text(const char *path, char *buffer, size_t capacity)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return -errno;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return -EINVAL; }
    ssize_t n = read(fd, buffer, capacity - 1);
    close(fd);
    if (n <= 0) return -EIO;
    buffer[n] = 0;
    buffer[strcspn(buffer, "\r\n")] = 0;
    return 0;
}

static int boot_order(const char *root, const char *boot, unsigned long long *order)
{
    char path[PATH_MAX],value[64],extra;
    if (snprintf(path,sizeof(path),"%s/%s/order",root,boot) >= (int)sizeof(path)) return -EINVAL;
    if (text(path,value,sizeof(value)) || sscanf(value,"%llu%c",order,&extra) != 1 ||
        !*order || *order > 0x7fffffffffffffffULL) return -EINVAL;
    return 0;
}

static int same_snapshot(const char *root, const struct log_archive_manifest *manifest)
{
    char index[LOG_ARCHIVE_INDEX_CAP];
    return !log_archive_index(root,manifest->boot,index,sizeof(index)) &&
           !strcmp(index,manifest->index);
}

/* Ignore only our own interrupted atomic writes. Never follow symlinks or
 * recursively remove a directory. This uploader is the only cache writer. */
static int clean_temps(const char *cache)
{
    DIR *dir = opendir(cache);
    if (!dir) return -errno;
    struct dirent *entry;
    int rc = 0;
    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name,"pending.tar.gz.tmp.",19)) continue;
        if (unlinkat(dirfd(dir),entry->d_name,0)) rc = -errno;
    }
    if (fsync(dirfd(dir))) rc = -errno;
    closedir(dir);
    return rc;
}

static int choose(const char *root, const char *current, struct log_archive_manifest *manifest)
{
    DIR *dir = opendir(root);
    if (!dir) return -errno;
    struct dirent *entry;
    unsigned long long oldest = ~0ULL;
    char path[PATH_MAX];
    while ((entry = readdir(dir))) {
        const char *boot = entry->d_name;
        unsigned long long order;
        if (!safe(boot) || !strcmp(boot,current) || boot_order(root,boot,&order) ||
            log_archive_receipted(root,boot)) continue;
        struct log_archive_manifest rejected;
        if (snprintf(path,sizeof(path),"%s/%s/.archive-unavailable",root,boot) >= (int)sizeof(path)) continue;
        if (!log_archive_manifest_read(path,&rejected) && !strcmp(rejected.boot,boot) &&
            !strcmp(rejected.server,manifest->server) && !strcmp(rejected.device,manifest->device)) continue;
        if (order < oldest && !log_archive_index(root,boot,manifest->index,sizeof(manifest->index))) {
            oldest = order;
            strcpy(manifest->boot,boot);
        }
    }
    closedir(dir);
    /* The last examined candidate may have replaced the scratch index. */
    return oldest == ~0ULL ? 1 : log_archive_index(root,manifest->boot,manifest->index,sizeof(manifest->index));
}

static int chunk(void *opaque, uint64_t offset, const void *data, size_t length, uint64_t *next)
{
    struct exchange *exchange = opaque;
    char path[600]; long long received;
    snprintf(path,sizeof(path),"%s%llu",exchange->path,(unsigned long long)offset);
    int rc = exchange->post(path,data,length,&received);
    if (!rc) *next = (uint64_t)received;
    return rc;
}

static int clear_pending(const char *state, const char *archive)
{
    if (unlink(state) && errno != ENOENT) return -errno;
    if (unlink(archive) && errno != ENOENT) return -errno;
    return dm_transfer_sync_parent(state);
}

int bundle_forward(const char *root, const char *server, const char *device,
                   startup_post_fn post, int (*cancelled)(void *))
{
    char current[64],cache[PATH_MAX],state[PATH_MAX],archive[PATH_MAX],receipt[PATH_MAX];
    const char *override = getenv("LOGCTL_BOOT_ID");
    if (override) snprintf(current,sizeof(current),"%s",override);
    else if (text("/proc/sys/kernel/random/boot_id",current,sizeof(current))) return -EIO;
    if (!safe(current) || strlen(server) >= 512 || !safe(device)) return -EINVAL;
    /* Sibling cache keeps tar output outside the source tree. */
    if (!realpath(root,cache)) return -errno;
    size_t n = strlen(cache);
    if (snprintf(cache+n,sizeof(cache)-n,"-archive-upload") >= (int)(sizeof(cache)-n)) return -EINVAL;
    if (mkdir(cache,0700) && errno != EEXIST) return -errno;
    struct stat st;
    if (lstat(cache,&st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077)) return -EINVAL;
    if (dm_transfer_sync_parent(cache)) return -EIO;
    if (snprintf(state,sizeof(state),"%s/pending.state",cache) >= (int)sizeof(state) ||
        snprintf(archive,sizeof(archive),"%s/pending.tar.gz",cache) >= (int)sizeof(archive)) return -EINVAL;
    int rc = clean_temps(cache);
    if (rc) return rc;
    struct log_archive_manifest manifest = {0};
    int loaded = !log_archive_manifest_read(state,&manifest);
    if (loaded && (!safe(manifest.boot) || !strcmp(manifest.boot,current) ||
                   strcmp(server,manifest.server) || strcmp(device,manifest.device))) loaded = 0;
    if (loaded && log_archive_receipted(root,manifest.boot)) return clear_pending(state,archive);
    if (loaded && !same_snapshot(root,&manifest)) loaded = 0;
    if (!loaded) {
        if ((rc = clear_pending(state,archive))) return rc;
        memset(&manifest,0,sizeof(manifest));
        strcpy(manifest.server,server); strcpy(manifest.device,device);
        rc = choose(root,current,&manifest);
        if (rc) return rc < 0 ? rc : 0;
        rc = log_archive_create_cancel(root,manifest.boot,archive,cancelled);
        if (rc) return rc;
        if (!same_snapshot(root,&manifest)) return -EAGAIN;
    }
    int fd = open(archive,O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) { int error=errno; (void)clear_pending(state,archive); return -error; }
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size <= 0 || st.st_size > 32*1024*1024) {
        close(fd); return -EINVAL;
    }
    struct dm_transfer_callbacks callbacks = {.cancelled=cancelled};
    if (!loaded) {
        manifest.size = (uint64_t)st.st_size;
        rc = dm_transfer_sha256_fd(fd,manifest.size,manifest.sha,&callbacks);
        if (!rc) rc = log_archive_manifest_write(state,&manifest);
        if (rc) { close(fd); return rc; }
    } else if (manifest.size != (uint64_t)st.st_size) {
        close(fd); (void)clear_pending(state,archive); return -EIO;
    }
    unsigned long long order;
    if (boot_order(root,manifest.boot,&order)) { close(fd); return -EINVAL; }
    struct exchange exchange = {.post=post};
    snprintf(exchange.path,sizeof(exchange.path),"/api/sol/bundles/%s/%s/%s/%llu/%llu/",
             device,manifest.boot,manifest.sha,(unsigned long long)manifest.size,order);
    struct dm_transfer_push push = {.fd=fd,.size=manifest.size,.chunk_size=4096,.max_chunks=32,
        .exchange=chunk,.context=&exchange,.callbacks=callbacks};
    uint64_t next;
    rc = dm_transfer_push_file(&push,&next);
    close(fd);
    if (!rc) {
        char path[600]; long long committed = 0;
        snprintf(path,sizeof(path),"%scommit",exchange.path);
        rc = post(path,"",0,&committed);
        if (!rc && committed != 1) {
            /* A full-length package failing SHA must be rebuilt, not looped. */
            (void)clear_pending(state,archive);
            return -EBADMSG;
        }
    }
    if (rc == -ESTALE) {
        /* Server deletion/expiry is not successful archival. Keep local files,
         * record the explicit loss and avoid repeatedly recreating the item. */
        if (log_retention_record(root,manifest.boot,"server-archive-unavailable","bundle",0,manifest.size)) return -errno;
        if (snprintf(receipt,sizeof(receipt),"%s/%s/.archive-unavailable",root,manifest.boot) >= (int)sizeof(receipt)) return -EINVAL;
        if ((rc = log_archive_manifest_write(receipt,&manifest))) return rc;
        return clear_pending(state,archive);
    }
    if (rc) return rc;
    if (!same_snapshot(root,&manifest)) return -EAGAIN;
    if (snprintf(receipt,sizeof(receipt),"%s/%s/.archive-receipt",root,manifest.boot) >= (int)sizeof(receipt)) return -EINVAL;
    if ((rc = log_archive_manifest_write(receipt,&manifest))) return rc;
    fprintf(stderr,"[logctld] [INFO] Boot archive committed: %s, sha256=%.8s\n",manifest.boot,manifest.sha);
    return clear_pending(state,archive);
}
