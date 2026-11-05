#ifndef DEVICE_MANAGER_ADMISSION_H
#define DEVICE_MANAGER_ADMISSION_H
#include "router.h"
/* Header collection is concurrent; all route/database handlers stay serialized. */
int server_accept_loop(int listener, const struct server_context *context);
#endif
