#define _POSIX_C_SOURCE 200809L
#include "fwctld-state.h"
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define RECORD_SIZE (OTA_REPORT_SIZE + 256)
#define MAX_EVENTS 64

static uint32_t crc32_record(const char *text, size_t size)
{
    uint32_t crc = ~0U;
    for (size_t i = 0; i < size; i++) {
        crc ^= (unsigned char)text[i];
        for (unsigned b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

static int encode(const struct fwctld_record *record, char text[RECORD_SIZE])
{
    char body[OTA_REPORT_SIZE], tag[32];
    snprintf(tag, sizeof(tag), "v%u.%u.%u", record->report.version >> 16,
             (record->report.version >> 8) & 255, record->report.version & 255);
    if (!record->sequence || strcmp(tag, record->tag) ||
        strlen(record->identity) != 64 || strspn(record->identity, "0123456789abcdef") != 64 ||
        ota_report_format(body, &record->report)) return -1;
    int n = snprintf(text, RECORD_SIZE, "BSPOTATXN1 %llu %s %s\n%s",
                     record->sequence, record->tag, record->identity, body);
    if (n < 0 || n > RECORD_SIZE - 16) return -1;
    snprintf(text + n, RECORD_SIZE - n, "crc32 %08x\n", crc32_record(text, n));
    return 0;
}

static int read_record(struct fwctld_journal *journal, const char *name,
                       struct fwctld_record *record)
{
    char path[OTA_PATH], text[RECORD_SIZE], canonical[RECORD_SIZE], extra;
    if (ota_path(path, journal->directory, name)) return -1;
    int fd = open(path, O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? 1 : -1;
    struct stat st;
    int rc = -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size >= RECORD_SIZE) goto done;
    size_t used = 0;
    while (used < (size_t)st.st_size) {
        ssize_t n = read(fd, text + used, st.st_size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done;
        used += (size_t)n;
    }
    text[used] = 0;
    if (memchr(text, 0, used)) goto done;
    char *body = strchr(text, '\n');
    if (!body) goto done;
    *body = 0;
    memset(record, 0, sizeof(*record));
    if (sscanf(text, "BSPOTATXN1 %llu %31s %64s %c", &record->sequence,
               record->tag, record->identity, &extra) != 3) goto done;
    *body++ = '\n';
    char *footer = strchr(body, '\n');
    if (!footer) goto done;
    char saved = footer[1]; footer[1] = 0;
    int parsed = ota_report_parse(body, &record->report);
    footer[1] = saved;
    if (parsed || strcmp(record->report.device, journal->device) ||
        strcmp(record->report.hwid, journal->hwid) || encode(record, canonical) ||
        strcmp(canonical, text)) goto done;
    rc = 0;
done:
    if (close(fd)) rc = -1;
    if (rc < 0) errno = EBADMSG;
    return rc;
}

static int write_record(struct fwctld_journal *journal, const char *name,
                        const struct fwctld_record *record)
{
    char text[RECORD_SIZE], path[OTA_PATH], temporary[OTA_PATH];
    if (encode(record, text) || ota_path(path, journal->directory, name) ||
        ota_path(temporary, journal->directory, ".record-XXXXXX")) return -1;
    int fd = mkstemp(temporary);
    if (fd < 0) return -1;
    size_t left = strlen(text), offset = 0;
    int rc = -1;
    while (left) {
        ssize_t n = write(fd, text + offset, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done;
        offset += (size_t)n; left -= (size_t)n;
    }
    if (fsync(fd)) goto done;
    if (close(fd)) { fd = -1; goto done; }
    fd = -1;
    if (rename(temporary, path) || ota_sync_dir(journal->directory)) goto done;
    rc = 0;
done:
    if (fd >= 0) close(fd);
    unlink(temporary);
    return rc;
}

static int ensure_directory(const char *path)
{
    char parent[OTA_PATH];
    if (strlen(path) >= sizeof(parent) || ota_mkdir(path)) return -1;
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    if (!slash) strcpy(parent, ".");
    else if (slash == parent) slash[1] = 0;
    else *slash = 0;
    return ota_sync_dir(path) || ota_sync_dir(parent) ? -1 : 0;
}

int fwctld_state_open(struct fwctld_journal *journal, const char *base,
                      const char *device, const char *hwid)
{
    char directory[OTA_PATH];
    memset(journal, 0, sizeof(*journal)); journal->lock = -1;
    if (!ota_identifier(device) || !isalnum((unsigned char)device[0]) || strlen(device) >= sizeof(journal->device) ||
        !ota_hwid_valid(hwid) || ensure_directory(base) ||
        ota_path(directory, base, hwid) || ensure_directory(directory) ||
        ota_path(journal->directory, directory, device) || ensure_directory(journal->directory)) return -1;
    strcpy(journal->device, device); strcpy(journal->hwid, hwid);
    journal->lock = ota_lock(journal->directory);
    return journal->lock < 0 ? -1 : 0;
}

void fwctld_state_close(struct fwctld_journal *journal)
{
    if (journal->lock >= 0) close(journal->lock);
    journal->lock = -1;
}

static int scan(struct fwctld_journal *journal, struct fwctld_record *first,
                struct fwctld_record *last, unsigned *count)
{
    DIR *dir = opendir(journal->directory);
    if (!dir) return -1;
    struct dirent *entry;
    int rc = 0; *count = 0;
    while ((entry = readdir(dir))) {
        if (strlen(entry->d_name) != 26 || strcmp(entry->d_name + 20, ".event")) continue;
        unsigned long long sequence; char extra;
        struct fwctld_record value;
        if (strspn(entry->d_name, "0123456789") != 20 ||
            sscanf(entry->d_name, "%20llu.event%c", &sequence, &extra) != 1 ||
            read_record(journal, entry->d_name, &value) || value.sequence != sequence ||
            ++*count > MAX_EVENTS) { rc = -1; break; }
        if (!first->sequence || value.sequence < first->sequence) *first = value;
        if (!last->sequence || value.sequence > last->sequence) *last = value;
    }
    closedir(dir);
    if (rc) errno = EBADMSG;
    return rc;
}

int fwctld_state_read(struct fwctld_journal *journal, struct fwctld_record *record)
{
    struct fwctld_record first = {0}, last = {0}; unsigned count;
    int rc = read_record(journal, "transaction", &last);
    if (rc < 0 || scan(journal, &first, &last, &count)) return -1;
    if (!last.sequence) { memset(record, 0, sizeof(*record)); return 1; }
    *record = last; return 0;
}

int fwctld_state_append(struct fwctld_journal *journal, struct fwctld_record *record)
{
    struct fwctld_record first = {0}, last = {0}; unsigned count;
    int rc = read_record(journal, "transaction", &last);
    if (rc < 0 || scan(journal, &first, &last, &count)) return -1;
    if (count == MAX_EVENTS || last.sequence == ULLONG_MAX) { errno = ENOSPC; return -1; }
    record->sequence = last.sequence + 1;
    char name[32]; snprintf(name, sizeof(name), "%020llu.event", record->sequence);
    /* The immutable event is the write-ahead record; a crash before the latest
     * snapshot rename is recovered by selecting the highest valid event. */
    if (write_record(journal, name, record)) return -1;
    return write_record(journal, "transaction", record);
}

int fwctld_state_first(struct fwctld_journal *journal, struct fwctld_record *record)
{
    struct fwctld_record first = {0}, last = {0}; unsigned count;
    if (scan(journal, &first, &last, &count)) return -1;
    if (!count) return 1;
    *record = first; return 0;
}

int fwctld_state_ack(struct fwctld_journal *journal, unsigned long long sequence)
{
    struct fwctld_record last;
    if (!sequence || fwctld_state_read(journal, &last) ||
        write_record(journal, "transaction", &last)) return -1;
    char name[32], path[OTA_PATH];
    snprintf(name, sizeof(name), "%020llu.event", sequence);
    if (ota_path(path, journal->directory, name) || unlink(path)) return -1;
    return ota_sync_dir(journal->directory);
}
