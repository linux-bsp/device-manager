#define _GNU_SOURCE
#include "retention.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

int log_retention_record(const char *root, const char *boot, const char *reason,
                         const char *stage, uint64_t generation, uint64_t bytes)
{
    int dir = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0) return -1;
    int fd = -1, rc = -1;
    if (flock(dir, LOCK_EX | LOCK_NB)) { int error = errno; close(dir); errno = error; return -1; }
    struct stat st;
    if (!fstatat(dir, ".retention.log", &st, AT_SYMLINK_NOFOLLOW)) {
        if (!S_ISREG(st.st_mode) || st.st_nlink != 1) { errno = EINVAL; goto out; }
        if (st.st_size >= 65536 && renameat(dir, ".retention.log", dir, ".retention.log.1")) goto out;
    } else if (errno != ENOENT) goto out;
    fd = openat(dir, ".retention.log", O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) goto out;
    char record[512];
    int length = snprintf(record, sizeof(record),
        "{\"time\":%lld,\"boot\":\"%s\",\"reason\":\"%s\",\"stage\":\"%s\",\"generation\":%llu,\"bytes\":%llu}\n",
        (long long)time(NULL), boot, reason, stage,
        (unsigned long long)generation, (unsigned long long)bytes);
    if (length < 0 || length >= (int)sizeof(record)) { errno = EOVERFLOW; goto out; }
    for (int offset = 0; offset < length;) {
        ssize_t n = write(fd, record + offset, (size_t)(length - offset));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; goto out; }
        offset += (int)n;
    }
    if (fsync(fd) || fsync(dir)) goto out;
    rc = 0;
out:;
    int error = errno;
    if (fd >= 0 && close(fd) && !rc) { rc = -1; error = errno; }
    close(dir); errno = error;
    return rc;
}
