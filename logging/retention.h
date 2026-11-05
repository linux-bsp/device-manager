#ifndef LOG_RETENTION_H
#define LOG_RETENTION_H
#include <stdint.h>
#define LOG_KEEP_BOOTS 12
/* Written before intentional loss. Two bounded, fsynced journal files. */
int log_retention_record(const char *root, const char *boot, const char *reason,
                         const char *stage, uint64_t generation, uint64_t bytes);
#endif
