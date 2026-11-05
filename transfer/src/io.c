// SPDX-License-Identifier: GPL-2.0+
#define _POSIX_C_SOURCE 200809L
#include "private.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int dm_transfer_cancelled(const struct dm_transfer_callbacks *callbacks)
{
    return callbacks && callbacks->cancelled && callbacks->cancelled(callbacks->context);
}
int dm_transfer_write_all(int fd, const void *data, size_t length)
{
    const unsigned char *cursor = data;
    while (length) {
        ssize_t n = write(fd, cursor, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return n < 0 ? -errno : -EIO;
        cursor += n; length -= (size_t)n;
    }
    return 0;
}
int dm_transfer_sync_parent(const char *path)
{
    char parent[DM_TRANSFER_PATH];
    if (!path || strlen(path) >= sizeof(parent)) return -ENAMETOOLONG;
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    if (!slash) strcpy(parent, ".");
    else if (slash == parent) slash[1] = 0;
    else *slash = 0;
    int fd = open(parent, O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if (fd < 0) return -errno;
    int rc = fsync(fd) ? -errno : 0;
    if (close(fd) && !rc) rc = -errno;
    return rc;
}
int dm_transfer_sync_file(const char *path)
{
    int fd = open(path, O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if (fd < 0) return -errno;
    struct stat st;
    int rc = fstat(fd, &st) ? -errno : !S_ISREG(st.st_mode) ? -EINVAL : 0;
    if (!rc && fsync(fd)) rc = -errno;
    if (close(fd) && !rc) rc = -errno;
    return rc ? rc : dm_transfer_sync_parent(path);
}
int dm_transfer_push_file(const struct dm_transfer_push *push, uint64_t *next)
{
    if (!push || !next || !push->exchange || !push->chunk_size || push->chunk_size > 1024*1024 ||
        push->size > INT64_MAX) return -EINVAL;
    if (dm_transfer_cancelled(&push->callbacks)) return -ECANCELED;
    uint64_t offset = 0;
    int rc = push->exchange(push->context, 0, "", 0, &offset);
    if (rc) return rc;
    if (offset > push->size) return -EPROTO;
    unsigned char *buffer = malloc(push->chunk_size);
    if (!buffer) return -ENOMEM;
    unsigned batches = 0, corrections = 0;
    while (offset < push->size) {
        if (dm_transfer_cancelled(&push->callbacks)) { rc = -ECANCELED; break; }
        if (push->max_chunks && batches >= push->max_chunks) { rc = 1; break; }
        size_t count = push->size-offset < push->chunk_size ? (size_t)(push->size-offset) : push->chunk_size;
        ssize_t n = pread(push->fd, buffer, count, (off_t)offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { rc = n < 0 ? -errno : -EIO; break; }
        uint64_t acknowledged;
        rc = push->exchange(push->context, offset, buffer, (size_t)n, &acknowledged);
        if (rc) break;
        if (acknowledged > push->size || (acknowledged <= offset && ++corrections > 2)) { rc = -EPROTO; break; }
        offset = acknowledged; batches++;
        if (push->callbacks.progress) push->callbacks.progress(push->callbacks.context, offset, push->size);
    }
    *next = offset;
    free(buffer); return rc;
}
