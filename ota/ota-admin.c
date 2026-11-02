/* Host-only upload, draft storage and publication management. */
#define _XOPEN_SOURCE 700
#include "ota-admin.h"
#include "ota-device.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

static int draft_id(const char *id)
{
    if (strlen(id) != 12 || strncmp(id, "draft-", 6))
        return 0;
    for (int i = 6; i < 12; i++)
        if (!isalnum((unsigned char)id[i]))
            return 0;
    return 1;
}

static int regular_directory(const char *path)
{
    struct stat st;
    return !lstat(path, &st) && S_ISDIR(st.st_mode);
}

static int octal(const unsigned char *field, size_t length, unsigned long long *result)
{
    size_t i = 0;
    unsigned long long n = 0;
    while (i < length && field[i] == ' ')
        i++;
    int digits = 0;
    for (; i < length && field[i] >= '0' && field[i] <= '7'; i++) {
        if (n > (OTA_UPLOAD_MAX + 1048576) / 8)
            return -1;
        n = n * 8 + field[i] - '0';
        digits++;
    }
    for (; i < length; i++)
        if (field[i] && field[i] != ' ')
            return -1;
    *result = n;
    return digits ? 0 : -1;
}

static int gz_exact(gzFile file, void *data, unsigned length)
{
    unsigned pos = 0;
    while (pos < length) {
        int n = gzread(file, (char *)data + pos, length - pos);
        if (n <= 0)
            return -1;
        pos += (unsigned)n;
    }
    return 0;
}

/* Accept only harmless PAX metadata. Path/size/link/sparse overrides are rejected. */
static int pax_metadata(char *data, size_t size)
{
    size_t pos = 0;
    while (pos < size) {
        char *end;
        if (!isdigit((unsigned char)data[pos]))
            return -1;
        errno = 0;
        unsigned long n = strtoul(data + pos, &end, 10);
        if (errno || n < 5 || n > size - pos || end >= data + pos + n || *end != ' ' ||
            data[pos + n - 1] != '\n')
            return -1;
        char *key = end + 1;
        char *equals = memchr(key, '=', (size_t)(data + pos + n - 1 - key));
        if (!equals)
            return -1;
        *equals = 0;
        if (strcmp(key, "mtime") && strcmp(key, "atime") && strcmp(key, "ctime") &&
            strcmp(key, "uid") && strcmp(key, "gid") && strcmp(key, "uname") && strcmp(key, "gname"))
            return -1;
        pos += n;
    }
    return 0;
}

/* Require a complete gzip member, including CRC/length trailer. Some gzread
 * implementations treat a truncated trailer as EOF without a close error. */
static int gzip_complete(const char *path)
{
    FILE *input = fopen(path, "rb");
    if (!input) return -1;
    z_stream stream = {0};
    if (inflateInit2(&stream, 15 + 16) != Z_OK) { fclose(input); return -1; }
    unsigned char compressed[65536], plain[65536];
    int rc = -1;
    size_t n;
    while ((n = fread(compressed, 1, sizeof(compressed), input)) > 0) {
        stream.next_in = compressed; stream.avail_in = (uInt)n;
        do {
            stream.next_out = plain; stream.avail_out = sizeof(plain);
            int result = inflate(&stream, Z_NO_FLUSH);
            if (stream.total_out > OTA_UPLOAD_MAX + 1048576) goto done;
            if (result == Z_STREAM_END) {
                if (!stream.avail_in && fgetc(input) == EOF && !ferror(input)) rc = 0;
                goto done;
            }
            if (result == Z_BUF_ERROR && !stream.avail_in) break;
            if (result != Z_OK) goto done;
        } while (stream.avail_in || !stream.avail_out);
    }
done:
    inflateEnd(&stream); fclose(input); return rc;
}

/* A deliberately restricted tar reader: fixed filenames, regular files only.
 * No archive-supplied path is ever passed to open(). Format offsets: <tar.h>.
 */
static int unpack(const char *archive, const char *directory)
{
    if (gzip_complete(archive)) return -1;
    gzFile file = gzopen(archive, "rb");
    if (!file)
        return -1;
    int rc = -1, seen = 0, headers = 0, ended = 0;
    unsigned long long total = 0;
    unsigned char header[512], buffer[65536];
    if (gzdirect(file))
        goto done;
    while (!ended && ++headers <= 24) {
        if (gz_exact(file, header, sizeof(header)))
            goto done;
        int zero = 1;
        for (size_t i = 0; i < sizeof(header); i++)
            if (header[i])
                zero = 0;
        if (zero) {
            if (gz_exact(file, header, sizeof(header)))
                goto done;
            for (size_t i = 0; i < sizeof(header); i++)
                if (header[i])
                    goto done;
            ended = 1;
            break;
        }
        unsigned long long expected, size;
        unsigned sum = 0;
        for (unsigned i = 0; i < sizeof(header); i++)
            sum += i >= 148 && i < 156 ? ' ' : header[i];
        if (octal(header + 148, 8, &expected) || expected != sum ||
            memcmp(header + 257, "ustar", 5) || header[345] ||
            octal(header + 124, 12, &size) || size > OTA_UPLOAD_MAX ||
            total + size > OTA_UPLOAD_MAX)
            goto done;
        total += size;
        if (header[156] == 'x') {
            char metadata[4097];
            if (!size || size > sizeof(metadata) - 1 || gz_exact(file, metadata, (unsigned)size))
                goto done;
            metadata[size] = 0;
            if (memchr(metadata, 0, (size_t)size) || pax_metadata(metadata, (size_t)size))
                goto done;
        } else {
            if (header[156] != '0' && header[156] != 0)
                goto done;
            char name[101];
            memcpy(name, header, 100);
            name[100] = 0;
            const char *plain = !strncmp(name, "./", 2) ? name + 2 : name;
            const char *fixed = NULL;
            int index = 3;
            for (int i = 0; i < 3; i++)
                if (!strcmp(plain, ota_files[i])) {
                    fixed = ota_files[i];
                    index = i;
                }
            if (!strcmp(plain, "SHA256SUMS"))
                fixed = "SHA256SUMS";
            if (!fixed || (seen & (1 << index)) || !size || (index == 3 && size > 4096))
                goto done;
            seen |= 1 << index;
            char path[OTA_PATH];
            if (ota_path(path, directory, fixed))
                goto done;
            int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
            if (fd < 0)
                goto done;
            unsigned long long left = size;
            while (left) {
                unsigned count = left < sizeof(buffer) ? (unsigned)left : sizeof(buffer);
                if (gz_exact(file, buffer, count)) {
                    close(fd);
                    goto done;
                }
                unsigned pos = 0;
                while (pos < count) {
                    ssize_t n = write(fd, buffer + pos, count - pos);
                    if (n < 0 && errno == EINTR)
                        continue;
                    if (n <= 0) {
                        close(fd);
                        goto done;
                    }
                    pos += (unsigned)n;
                }
                left -= count;
            }
            int failed = fsync(fd);
            if (close(fd) || failed)
                goto done;
        }
        unsigned padding = (unsigned)((512 - size % 512) % 512);
        if (gz_exact(file, buffer, padding))
            goto done;
    }
    if (!ended || (seen & 7) != 7)
        goto done;
    /* Drain to EOF to check gzip CRC; bound and reject nonzero trailing data. */
    unsigned padding = 0;
    for (;;) {
        int n = gzread(file, buffer, sizeof(buffer));
        if (n < 0)
            goto done;
        if (!n) {
            int error;
            (void)gzerror(file, &error);
            if (error != Z_OK && error != Z_STREAM_END) goto done;
            break;
        }
        padding += (unsigned)n;
        if (padding > 1048576)
            goto done;
        for (int i = 0; i < n; i++)
            if (buffer[i])
                goto done;
    }
    rc = 0;
done:
    if (gzclose(file) != Z_OK)
        rc = -1;
    return rc;
}

void ota_draft_cleanup(const char *temp)
{
    char path[OTA_PATH];
    const char *names[] = {"package.tar.gz", "release.itb", "kernel.itb", "rootfs.itb", "SHA256SUMS", "manifest", "published"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!ota_path(path, temp, names[i]))
            unlink(path);
    rmdir(temp);
}

int ota_draft_begin(const char *root, char temp[OTA_PATH])
{
    char drafts[OTA_PATH], path[OTA_PATH];
    if (ota_path(drafts, root, "drafts") || ota_mkdir(drafts))
        return -1;
    DIR *dir = opendir(drafts);
    if (!dir)
        return -1;
    unsigned count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)))
        if (draft_id(entry->d_name))
            count++;
    closedir(dir);
    if (count >= 256 || ota_path(temp, drafts, ".upload-XXXXXX") || !mkdtemp(temp))
        return -1;
    if (ota_path(path, temp, "package.tar.gz")) {
        ota_draft_cleanup(temp);
        return -1;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        ota_draft_cleanup(temp);
    return fd;
}

int ota_draft_finish(const char *root, const char *temp, char id[32])
{
    struct release r;
    unsigned long fit_version;
    char path[OTA_PATH], drafts[OTA_PATH], target[OTA_PATH];
    if (ota_path(path, temp, "package.tar.gz") || unpack(path, temp) || ota_inspect(temp, &r, &fit_version))
        return -1;
    unlink(path);
    if (ota_path(path, temp, "manifest") || ota_write(path, &r) || ota_sync_dir(temp))
        return -1;
    snprintf(id, 32, "draft-%s", temp + strlen(temp) - 6);
    if (ota_path(drafts, root, "drafts") || ota_path(target, drafts, id) || !access(target, F_OK) ||
        rename(temp, target) || ota_sync_dir(drafts))
        return -1;
    struct ota_catalog_identity identity;
    return ota_catalog_get(root, target, &identity);
}


static unsigned long long published_number(const char *directory)
{
    char path[OTA_PATH];
    unsigned long long number = 0;
    if (!ota_path(path, directory, "published")) {
        FILE *f = fopen(path, "r");
        if (f) {
            if (fscanf(f, "%llu", &number) != 1)
                number = 0;
            fclose(f);
        }
    }
    return number;
}

static int number_id(const char *name, unsigned long long *number)
{
    char *end, canonical[32];
    errno = 0;
    *number = strtoull(name, &end, 10);
    if (errno || *end || !*number || *number > 2147483647)
        return 0;
    snprintf(canonical, sizeof(canonical), "%llu", *number);
    return !strcmp(name, canonical);
}

static int list_entries(FILE *f, const char *root, int draft, unsigned long long active)
{
    char base[OTA_PATH], directory[OTA_PATH], path[OTA_PATH];
    if (ota_path(base, root, draft ? "drafts" : "releases"))
        return -1;
    DIR *dir = opendir(base);
    if (!dir)
        return errno == ENOENT ? 0 : -1;
    struct dirent *entry;
    unsigned count = 0;
    while ((entry = readdir(dir))) {
        struct release r;
        unsigned long long number = 0;
        struct ota_catalog_identity identity;
        if ((draft && !draft_id(entry->d_name)) || (!draft && !number_id(entry->d_name, &number)))
            continue;
        if (ota_path(directory, base, entry->d_name) || !regular_directory(directory) ||
            ota_path(path, directory, "manifest") || ota_read(path, &r))
            continue;
        if (++count > 512) {
            closedir(dir);
            return -1;
        }
        if (ota_catalog_get(root, directory, &identity)) { closedir(dir); return -1; }
        struct stat st;
        if (stat(directory, &st)) {
            closedir(dir);
            return -1;
        }
        unsigned long long published = draft ? published_number(directory) : number;
        int blocked = ota_quarantine_hashes_blocked(root, identity.kernel, identity.rootfs);
        if (blocked < 0) { closedir(dir); return -1; }
        if (count > 1)
            fputc(',', f);
        fprintf(f, "{\"id\":\"%s\",\"product\":\"%s\",\"layout\":\"%s\","
            "\"firmware_version\":\"%s\",\"publication\":%llu,\"bytes\":%llu,\"created\":%lld,\"quarantined\":%s,\"tag\":\"v%s\",\"firmware_id\":\"%s\",\"uploaded_at\":%lld,\"valid\":%s,\"published\":%s}",
            entry->d_name, r.product, r.layout, identity.tag + 1, published,
            r.size[0] + r.size[1] + r.size[2], (long long)st.st_mtime, blocked ? "true" : "false", identity.tag + 1, identity.hash, identity.uploaded_at,
            blocked ? "false" : "true", published && published == active && !blocked ? "true" : "false");
    }
    closedir(dir);
    return 0;
}

int ota_admin_state(const char *root, char **json)
{
    char path[OTA_PATH];
    struct release latest;
    unsigned long long number = 0;
    if (ota_path(path, root, "latest"))
        return -1;
    if (!access(path, F_OK)) {
        if (ota_read(path, &latest))
            return -1;
        number = latest.version;
    }
    size_t size;
    *json = NULL;
    FILE *f = open_memstream(json, &size);
    if (!f)
        return -1;
    fprintf(f, "{\"latest\":%llu,\"drafts\":[", number);
    int failed = list_entries(f, root, 1, number);
    fprintf(f, "],\"releases\":[");
    failed |= list_entries(f, root, 0, number);
    fprintf(f, "],\"devices\":[");
    failed |= ota_devices_json(root, f);
    fprintf(f, "],\"device_history\":[");
    failed |= ota_history_json(root, f);
    fprintf(f, "],\"download_stats\":[");
    failed |= ota_catalog_downloads_json(root, f);
    fprintf(f, "],\"release_stats\":[");
    failed |= ota_history_summary(root, f);
    fprintf(f, "],\"audit_events\":[");
    failed |= ota_quarantine_json(root, f);
    fprintf(f, "]}\n");
    if (fclose(f))
        failed = 1;
    if (failed) {
        free(*json);
        *json = NULL;
        return -1;
    }
    return 0;
}

int ota_admin_publish(const char *root, const char *id, unsigned long long *number)
{
    if (!draft_id(id))
        return 404;
    char drafts[OTA_PATH], source[OTA_PATH], latest_path[OTA_PATH], releases[OTA_PATH];
    if (ota_path(drafts, root, "drafts") || ota_path(source, drafts, id) || !regular_directory(source))
        return 404;
    struct release r, latest;
    unsigned long fit_version;
    if (ota_inspect(source, &r, &fit_version))
        return 422;
    int blocked = ota_quarantine_blocked(root, source);
    if (blocked) return blocked > 0 ? 409 : 503;
    unsigned long long previous = published_number(source);
    if (previous && !ota_path(latest_path, root, "latest") &&
        !ota_read(latest_path, &latest) && latest.version == previous) {
        *number = previous;
        return 200; /* Repeated clicks on the active publication are idempotent. */
    }
    *number = 1;
    if (ota_path(latest_path, root, "latest") || ota_path(releases, root, "releases"))
        return 500;
    if (!access(latest_path, F_OK)) {
        if (ota_read(latest_path, &latest))
            return 500;
        if (strcmp(r.product, latest.product) || strcmp(r.layout, latest.layout))
            return 409;
        *number = latest.version + 1;
    }
    /* Skip an orphaned snapshot left by an interrupted CLI publication. */
    DIR *dir = opendir(releases);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            unsigned long long n;
            if (number_id(entry->d_name, &n) && n >= *number)
                *number = n + 1;
        }
        closedir(dir);
    } else if (errno != ENOENT) {
        return 500;
    }
    if (*number > 2147483647)
        return 409;
    char value[32];
    snprintf(value, sizeof(value), "%llu", *number);
    char *args[] = {"device-server", "publish", "--root", (char *)root, "--source", source,
        "--version", value, "--product", r.product, "--layout", r.layout, NULL};
    if (ota_publish(12, args))
        return 409; /* Includes concurrent CLI publication or a full filesystem. */
    char marker[OTA_PATH];
    if (!ota_path(marker, source, "published")) {
        FILE *f = fopen(marker, "w");
        if (f) {
            fprintf(f, "%llu\n", *number);
            fflush(f);
            fsync(fileno(f));
            fclose(f);
        }
    }
    return 200;
}

/* Explicit management export, including quarantined firmware for audit. Device
 * endpoints retain their quarantine checks; manual export never counts as OTA. */
int ota_admin_bundle(const char *root, const char *kind, const char *id,
                     char filename[128], unsigned long long *size)
{
    char base[OTA_PATH], directory[OTA_PATH], temporary[] = "/tmp/ota-export-XXXXXX";
    char archive[OTA_PATH], ignored[1024]; unsigned long long number;
    if ((!strcmp(kind, "draft") && !draft_id(id)) ||
        (!strcmp(kind, "release") && !number_id(id, &number)) ||
        (strcmp(kind, "draft") && strcmp(kind, "release"))) return -ENOENT;
    if (ota_path(base, root, !strcmp(kind, "draft") ? "drafts" : "releases") ||
        ota_path(directory, base, id) || !regular_directory(directory)) return -ENOENT;
    struct release release; unsigned long version; struct ota_catalog_identity identity;
    if (ota_inspect(directory, &release, &version) || ota_catalog_get(root, directory, &identity)) return -EIO;
    if (!mkdtemp(temporary)) return -EIO;
    int fd = -EIO;
    if (ota_path(archive, temporary, "firmware.tar.gz")) { rmdir(temporary); return -EIO; }
    char *args[] = {"tar", "--format=ustar", "-czf", archive, "-C", directory,
        "release.itb", "kernel.itb", "rootfs.itb", NULL};
    if (!ota_run(args, ignored, sizeof(ignored))) {
        struct stat st;
        fd = open(archive, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd >= 0 && (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0)) { close(fd); fd = -EIO; }
        if (fd >= 0) {
            *size = (unsigned long long)st.st_size;
            snprintf(filename, 128, "firmware-%s-%.8s.tar.gz", identity.tag, identity.hash);
        } else fd = -EIO;
    }
    unlink(archive); rmdir(temporary);
    return fd;
}
