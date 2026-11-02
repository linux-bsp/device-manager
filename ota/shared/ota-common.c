#define _POSIX_C_SOURCE 200809L
#include "ota-common.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

const char *ota_files[3] = {"release.itb", "kernel.itb", "rootfs.itb"};
volatile sig_atomic_t ota_stop;

int ota_path(char *out, const char *base, const char *name)
{
    int n = snprintf(out, OTA_PATH, "%s/%s", base, name);
    return n < 0 || n >= OTA_PATH ? -1 : 0;
}

int ota_identifier(const char *s)
{
    if (!s[0] || strlen(s) > 63)
        return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_' && *s != '.')
            return 0;
    return 1;
}

int ota_mkdir(const char *path)
{
    struct stat st;
    if (mkdir(path, 0755) && errno != EEXIST)
        return -1;
    return lstat(path, &st) || !S_ISDIR(st.st_mode) ? -1 : 0;
}

int ota_sync_dir(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int rc = fsync(fd);
    close(fd);
    return rc;
}

int ota_lock(const char *root)
{
    char path[OTA_PATH];
    if (ota_path(path, root, ".lock"))
        return -1;
    int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB)) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Children are executed directly: no shell interpretation of paths or URLs. */
int ota_run(char *const argv[], char *output, unsigned capacity)
{
    int pipes[2], status;
    if (pipe(pipes))
        return -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(pipes[0]);
        if (dup2(pipes[1], STDOUT_FILENO) < 0)
            _exit(126);
        close(pipes[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pipes[1]);
    if (pid < 0) {
        close(pipes[0]);
        return -1;
    }
    unsigned used = 0;
    char buffer[512];
    for (;;) {
        ssize_t n = read(pipes[0], buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) {
            if (ota_stop)
                kill(pid, SIGTERM);
            continue;
        }
        if (n <= 0)
            break;
        if (output && capacity > used + 1) {
            unsigned take = (unsigned)n < capacity - used - 1 ? (unsigned)n : capacity - used - 1;
            memcpy(output + used, buffer, take);
            used += take;
        }
    }
    close(pipes[0]);
    if (output && capacity)
        output[used] = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
        if (ota_stop)
            kill(pid, SIGTERM);
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 && !ota_stop ? 0 : -1;
}

int ota_hash(const char *path, char hash[65], unsigned long long *size)
{
    struct stat st;
    char output[256];
    char *args[] = {"sha256sum", (char *)path, NULL};
    if (lstat(path, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (unsigned long long)st.st_size > OTA_MAX_FILE || ota_run(args, output, sizeof(output)))
        return -1;
    if (strlen(output) < 65 || output[64] != ' ')
        return -1;
    for (int i = 0; i < 64; i++)
        if (!isxdigit((unsigned char)output[i]))
            return -1;
    memcpy(hash, output, 64);
    hash[64] = 0;
    *size = (unsigned long long)st.st_size;
    return 0;
}

int ota_read(const char *path, struct release *r)
{
    FILE *f = fopen(path, "r");
    char magic[16], tail;
    struct stat st;
    if (!f)
        return -1;
    int rc = -1;
    if (fstat(fileno(f), &st) || !S_ISREG(st.st_mode) || st.st_size > 2048)
        goto out;
    if (fscanf(f, "%15s %llu %63s %63s", magic, &r->version, r->product, r->layout) != 4 ||
        strcmp(magic, "BSPOTA1") || !r->version || r->version > 2147483647 ||
        !ota_identifier(r->product) || !ota_identifier(r->layout))
        goto out;
    for (int i = 0; i < 3; i++) {
        char name[32];
        if (fscanf(f, "%31s %llu %64s", name, &r->size[i], r->hash[i]) != 3 ||
            strcmp(name, ota_files[i]) || !r->size[i] || r->size[i] > OTA_MAX_FILE ||
            strlen(r->hash[i]) != 64)
            goto out;
        for (int j = 0; j < 64; j++)
            if (!((r->hash[i][j] >= '0' && r->hash[i][j] <= '9') ||
                  (r->hash[i][j] >= 'a' && r->hash[i][j] <= 'f')))
                goto out;
    }
    if (fscanf(f, " %c", &tail) == EOF && !ferror(f))
        rc = 0;
out:
    fclose(f);
    return rc;
}

int ota_write(const char *path, const struct release *r)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        return -1;
    }
    int rc = fprintf(f, "BSPOTA1 %llu %s %s\n", r->version, r->product, r->layout) < 0;
    for (int i = 0; i < 3; i++)
        if (fprintf(f, "%s %llu %s\n", ota_files[i], r->size[i], r->hash[i]) < 0)
            rc = 1;
    if (fflush(f) || fsync(fd))
        rc = 1;
    if (fclose(f))
        rc = 1;
    return rc ? -1 : 0;
}

int ota_verify(const char *directory, const struct release *r)
{
    for (int i = 0; i < 3; i++) {
        char path[OTA_PATH], hash[65];
        unsigned long long size;
        if (ota_path(path, directory, ota_files[i]) || ota_hash(path, hash, &size) ||
            size != r->size[i] || strcmp(hash, r->hash[i]))
            return -1;
    }
    return 0;
}

int ota_copy(const char *source, const char *dest)
{
    int in = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat st;
    if (in < 0)
        return -1;
    if (fstat(in, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (unsigned long long)st.st_size > OTA_MAX_FILE) {
        close(in);
        return -1;
    }
    int out = open(dest, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    int rc = -1;
    if (out < 0)
        goto done;
    char buffer[65536];
    off_t total = 0;
    for (;;) {
        ssize_t n = read(in, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 || total + n > st.st_size)
            goto done;
        if (!n)
            break;
        total += n;
        ssize_t pos = 0;
        while (pos < n) {
            ssize_t written = write(out, buffer + pos, (size_t)(n - pos));
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
                goto done;
            pos += written;
        }
    }
    if (total == st.st_size && !fsync(out))
        rc = 0;
done:
    close(in);
    if (out >= 0)
        close(out);
    return rc;
}
