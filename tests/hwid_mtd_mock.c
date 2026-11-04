/* SPDX-License-Identifier: GPL-2.0+ */
/* Test-only NOR device backed by a private regular file. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <mtd/mtd-user.h>
#include <unistd.h>
int test_mtd_type = MTD_NORFLASH, test_fail_write, test_erases, test_nor_enabled;
static const char *fake = "/dev/mtd-hwid-test";
int __real_stat(const char *, struct stat *);
int __wrap_stat(const char *path, struct stat *st)
{
    if (strcmp(path, fake)) return __real_stat(path, st);
    int rc = __real_stat(getenv("HWID_TEST_NOR"), st);
    if (!rc) st->st_mode = (st->st_mode & ~S_IFMT) | S_IFCHR;
    return rc;
}
char *__real_realpath(const char *, char *);
char *__wrap_realpath(const char *path, char *resolved)
{
    if (strcmp(path, fake)) return __real_realpath(path, resolved);
    if (resolved) { strcpy(resolved, fake); return resolved; }
    return strdup(fake);
}
int __real_open(const char *, int, ...);
int __wrap_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, unsigned int); va_end(ap); }
    return __real_open(!strcmp(path, fake) ? getenv("HWID_TEST_NOR") : path, flags, mode);
}
int __real_ioctl(int, unsigned long, ...);
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list ap; va_start(ap, request); void *arg = va_arg(ap, void *); va_end(ap);
    if (request == MEMGETINFO) {
        struct stat st; if (fstat(fd, &st)) return -1;
        struct mtd_info_user *m = arg; memset(m, 0, sizeof(*m));
        m->type = test_mtd_type; m->size = st.st_size; m->erasesize = 65536; m->writesize = 1; return 0;
    }
    if (request == MEMUNLOCK || request == MEMLOCK) return 0;
    if (request == MEMERASE) {
        struct erase_info_user *e = arg; unsigned char block[4096]; memset(block, 255, sizeof(block));
        if (e->start % 65536 || e->length % 65536) { errno = EINVAL; return -1; }
        test_erases++;
        for (unsigned int p = 0; p < e->length; p += sizeof(block))
            if (pwrite(fd, block, sizeof(block), e->start + p) != (ssize_t)sizeof(block)) return -1;
        return 0;
    }
    return __real_ioctl(fd, request, arg);
}
ssize_t __real_write(int, const void *, size_t);
ssize_t __wrap_write(int fd, const void *buf, size_t size)
{
    char name[64], path[512]; snprintf(name, sizeof(name), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(name, path, sizeof(path) - 1);
    if (n > 0) {
        path[n] = 0; const char *media = getenv("HWID_TEST_NOR");
        if (test_nor_enabled && media && !strcmp(path, media)) {
            if (test_fail_write) { errno = EIO; return -1; }
            off_t offset = lseek(fd, 0, SEEK_CUR); unsigned char *old = malloc(size);
            if (!old) { errno = ENOMEM; return -1; }
            if (pread(fd, old, size, offset) != (ssize_t)size) { free(old); errno = EIO; return -1; }
            for (size_t i = 0; i < size; i++)
                if ((old[i] & ((const unsigned char *)buf)[i]) != ((const unsigned char *)buf)[i]) { free(old); errno = EIO; return -1; }
            free(old);
        }
    }
    return __real_write(fd, buf, size);
}
