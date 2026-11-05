#define _GNU_SOURCE
#include "worker.h"
#include <dirent.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <unistd.h>

int server_worker_enter(pid_t parent, int socket, int file, unsigned seconds)
{
    signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGALRM, SIG_DFL);
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) return -1;
    alarm(seconds);
    DIR *directory = opendir("/proc/self/fd");
    if (!directory) return -1;
    int scan = dirfd(directory);
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        char *end; long fd = strtol(entry->d_name, &end, 10);
        if (*entry->d_name && !*end && fd >= 0 && fd != socket && fd != file && fd != scan)
            close((int)fd);
    }
    closedir(directory); return 0;
}
