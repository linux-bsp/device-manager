#ifndef DEVICE_MANAGER_WORKER_H
#define DEVICE_MANAGER_WORKER_H
#include <sys/types.h>
/* Called only after fork in this single-threaded host process. */
int server_worker_enter(pid_t parent, int socket, int file, unsigned seconds);
#endif
