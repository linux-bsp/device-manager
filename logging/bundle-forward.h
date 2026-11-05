#ifndef LOG_BUNDLE_FORWARD_H
#define LOG_BUNDLE_FORWARD_H
#include "startup-forward.h"
/* One persistent package, at most 128 KiB per cycle. Receipt is distinct from
 * transport progress. The caller holds the root's exclusive uploader lock. */
int bundle_forward(const char *root, const char *server, const char *device,
                   startup_post_fn post, int (*cancelled)(void *));
#endif
