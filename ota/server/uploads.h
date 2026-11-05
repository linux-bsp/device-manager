#ifndef DEVICE_MANAGER_UPLOADS_H
#define DEVICE_MANAGER_UPLOADS_H
#include "router.h"
void server_upload(int client, unsigned long long length, const struct server_context *context);
void server_uploads_poll(void);
void server_uploads_stop(void);
#endif
