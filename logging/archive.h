#ifndef LOG_ARCHIVE_H
#define LOG_ARCHIVE_H
#include <stdint.h>
#define LOG_ARCHIVE_INDEX_CAP 16384
struct log_archive_manifest {
    char server[512], device[64], boot[64], sha[65];
    uint64_t size;
    char index[LOG_ARCHIVE_INDEX_CAP];
};
int log_archive_index(const char *root, const char *boot, char *index, unsigned capacity);
int log_archive_manifest_read(const char *path, struct log_archive_manifest *manifest);
int log_archive_manifest_write(const char *path, const struct log_archive_manifest *manifest);
int log_archive_receipted(const char *root, const char *boot);
/* Create a gzip tar snapshot outside root. Compression failure preserves an
 * existing output. Rename follows file sync; parent sync errors are reported
 * even if the complete archive has already become visible. */
int log_archive_create_cancel(const char *root, const char *boot, const char *output, int (*cancelled)(void *));
int log_archive_create(const char *root, const char *boot, const char *output);
#endif
