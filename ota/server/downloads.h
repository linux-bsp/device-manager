#ifndef DEVICE_MANAGER_DOWNLOADS_H
#define DEVICE_MANAGER_DOWNLOADS_H
#include "dm/transfer-server.h"
/* Read-only transport workers never access catalog databases. Audit completion
 * and publication quarantine decisions run in the parent route process. */
void server_download(const struct dm_transfer_serve *serve, const char *root,
                      unsigned long long publication, const char *device, int count_download,
                      int *response_deferred, const char *artifact_directory);
void server_downloads_poll(void);
void server_downloads_stop(void);
#endif
