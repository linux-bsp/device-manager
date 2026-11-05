#ifndef LOG_BOOT_WORKER_H
#define LOG_BOOT_WORKER_H
/* runtime must be local volatile storage (/run/logctl in production). */
int log_boot_event_bounded(const char *runtime, const char *root,
                           const char *boot, const char *event);
#endif
