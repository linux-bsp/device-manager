#ifndef OTA_COMMON_H
#define OTA_COMMON_H
#include <stdint.h>
#include <signal.h>
#define OTA_PATH 4096
#define OTA_MAX_FILE (512ULL * 1024 * 1024)
struct release {
    unsigned long long version;
    char product[64], layout[64];
    unsigned long long size[3];
    char hash[3][65];
};
extern const char *ota_files[3];
extern volatile sig_atomic_t ota_stop;
int ota_path(char *out, const char *base, const char *name);
int ota_mkdir(const char *path);
int ota_lock(const char *root);
int ota_sync_dir(const char *path);
int ota_read(const char *path, struct release *r);
int ota_write(const char *path, const struct release *r);
int ota_hash(const char *path, char hash[65], unsigned long long *size);
int ota_verify(const char *directory, const struct release *r);
int ota_copy(const char *source, const char *dest);
int ota_run(char *const argv[], char *output, unsigned capacity);
int ota_publish(int argc, char **argv);
int ota_identifier(const char *value);
#endif
