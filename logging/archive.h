#ifndef LOG_ARCHIVE_H
#define LOG_ARCHIVE_H
/* Create a gzip tar snapshot outside root. Compression failure preserves an
 * existing output. Rename follows file sync; parent sync errors are reported
 * even if the complete archive has already become visible. */
int log_archive_create(const char *root, const char *boot, const char *output);
#endif
