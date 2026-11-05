#ifndef LOG_BOOT_RECORD_H
#define LOG_BOOT_RECORD_H
#include <stddef.h>
/* Observations and software intent never imply a hardware reset cause. */
int log_boot_start(const char *root, const char *boot, const char *previous);
int log_boot_event(const char *root, const char *boot, const char *event);
int log_boot_history(const char *root, const char *boot);
int log_boot_show(const char *root, const char *boot);
int log_boot_metadata(const char *root, const char *boot, char *out, size_t capacity);
#endif
