#ifndef STARTUP_FORWARD_H
#define STARTUP_FORWARD_H
#include <stddef.h>
typedef int (*startup_post_fn)(const char *path, const char *data, size_t length, long long *next);
int startup_forward(const char *device, startup_post_fn post);
#endif
