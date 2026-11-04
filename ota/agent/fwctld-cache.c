#define _POSIX_C_SOURCE 200809L
#include "fwctld-cache.h"
#include "ota-common.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int number(const char *name, unsigned long long *value)
{
    if (!*name || *name == '0' || strspn(name, "0123456789") != strlen(name)) return -1;
    char *end;
    errno = 0; *value = strtoull(name, &end, 10);
    return errno || *end || *value > 2147483647 ? -1 : 0;
}

static int manifest(int directory, const char *name, struct release *release)
{
    int fd = openat(directory, name, O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if (fd < 0) return -1;
    FILE *file = fdopen(fd, "r");
    if (!file) { close(fd); return -1; }
    int rc = ota_read_stream(file, release);
    if (fclose(file)) rc = -1;
    return rc;
}

/* No recursive deletion: unknown names, links, directories and special files
 * preserve the entire entry for inspection. Tombstones may be incomplete. */
static int owned(int fd, const struct release *release, unsigned long long *bytes)
{
    DIR *dir = fdopendir(dup(fd));
    if (!dir) return -1;
    struct dirent *entry;
    unsigned mask = 0;
    int rc = 0;
    *bytes = 0;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        int index = 3;
        if (strcmp(entry->d_name, "manifest")) {
            for (index = 0; index < 3; index++)
                if (!strcmp(entry->d_name, ota_files[index])) break;
            if (index == 3) { rc = -1; break; }
        }
        struct stat st;
        if (fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) ||
            !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
            (release && index < 3 && (unsigned long long)st.st_size != release->size[index])) {
            rc = -1; break;
        }
        mask |= 1U << index;
        *bytes += (unsigned long long)st.st_size;
    }
    closedir(dir);
    return rc || (release && mask != 15) ? -1 : 0;
}

static int erase(int root, const char *name, unsigned long long *bytes)
{
    int fd = openat(root, name, O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if (fd < 0) return -1;
    int rc = owned(fd, NULL, bytes);
    if (!rc) {
        for (unsigned i = 0; i < 4; i++) {
            const char *file = i < 3 ? ota_files[i] : "manifest";
            if (unlinkat(fd, file, 0) && errno != ENOENT) { rc = -1; break; }
        }
        if (!rc && fsync(fd)) rc = -1;
        if (!rc && (unlinkat(root, name, AT_REMOVEDIR) || fsync(root))) rc = -1;
    }
    close(fd);
    return rc;
}

static int newest_first(const void *a, const void *b)
{
    unsigned long long x = *(const unsigned long long *)a, y = *(const unsigned long long *)b;
    return x < y ? 1 : x > y ? -1 : 0;
}

int fwctld_cache_prune(const char *directory, const char *hwid, unsigned keep,
                       unsigned *removed, unsigned long long *bytes)
{
    *removed = 0; *bytes = 0;
    if (!keep) return 0;
    int root = open(directory, O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if (root < 0) return errno == ENOENT ? 0 : -1;
    struct release current;
    /* A corrupt/foreign current record never authorizes deletion. */
    if (manifest(root, "current", &current)) {
        int rc = errno == ENOENT ? 0 : -1;
        close(root); return rc;
    }
    if (strcmp(current.hwid, hwid)) { close(root); return -1; }
    DIR *dir = fdopendir(dup(root));
    if (!dir) { close(root); return -1; }
    struct dirent *entry;
    unsigned long long *versions = NULL, value;
    size_t count = 0;
    int rc = 0;
    while ((entry = readdir(dir))) {
        if (!strncmp(entry->d_name, ".gc-", 4) && !number(entry->d_name + 4, &value)) {
            unsigned long long freed;
            if (!erase(root, entry->d_name, &freed)) { ++*removed; *bytes += freed; }
            continue;
        }
        if (number(entry->d_name, &value)) continue;
        int fd = openat(root, entry->d_name, O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if (fd < 0) continue;
        struct release release;
        unsigned long long unused;
        int valid = !manifest(fd, "manifest", &release) && release.version == value &&
            !strcmp(release.hwid, hwid) && !owned(fd, &release, &unused);
        close(fd);
        if (!valid) continue;
        if (count >= 4096) { errno = E2BIG; rc = -1; break; }
        void *next = realloc(versions, (count + 1) * sizeof(*versions));
        if (!next) { rc = -1; break; }
        versions = next; versions[count++] = value;
    }
    closedir(dir);
    if (!rc) {
        if (count) qsort(versions, count, sizeof(*versions), newest_first);
        for (size_t i = keep; i < count; i++) {
            if (versions[i] == current.version || ota_stop) continue;
            char name[32], tombstone[40];
            snprintf(name, sizeof(name), "%llu", versions[i]);
            snprintf(tombstone, sizeof(tombstone), ".gc-%llu", versions[i]);
            struct stat st;
            if (!fstatat(root, tombstone, &st, AT_SYMLINK_NOFOLLOW) || errno != ENOENT ||
                renameat(root, name, root, tombstone) || fsync(root)) { rc = -1; break; }
            unsigned long long freed;
            if (erase(root, tombstone, &freed)) { rc = -1; break; }
            ++*removed; *bytes += freed;
        }
    }
    free(versions); close(root);
    return rc;
}
