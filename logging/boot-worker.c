#define _GNU_SOURCE
#include "boot-worker.h"
#include "boot-record.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long long monotonic_ms(void)
{
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) ? -1 :
        (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int log_boot_event_bounded(const char *runtime, const char *root,
                           const char *boot, const char *event)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/boot-event.lock", runtime) >= (int)sizeof(path))
        return -ENAMETOOLONG;
    int lease = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (lease < 0) return -errno;
    struct stat st;
    int result = 0;
    if (fstat(lease, &st)) result = -errno;
    else if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != geteuid()) result = -EINVAL;
    else if (flock(lease, LOCK_EX | LOCK_NB)) result = -errno;
    if (result) { close(lease); return result; }

    long long start = monotonic_ms();
    int channel[2];
    if (start < 0 || pipe2(channel, O_CLOEXEC | O_NONBLOCK)) {
        result = start < 0 ? -EIO : -errno;
        close(lease); return result;
    }
    pid_t parent = getpid(), worker = fork();
    if (worker < 0) {
        result = -errno; close(lease); close(channel[0]); close(channel[1]); return result;
    }
    if (!worker) {
        close(channel[0]);
        /* Keep the inherited /run lease, including while blocked in kernel IO.
         * A later CLI cannot create another storage worker until this exits. */
        close(STDIN_FILENO); close(STDOUT_FILENO); close(STDERR_FILENO);
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(125);
        result = log_boot_event(root, boot, event);
        ssize_t sent;
        do { sent = write(channel[1], &result, sizeof(result)); } while (sent < 0 && errno == EINTR);
        _exit(sent == sizeof(result) ? 0 : 125);
    }
    close(lease); close(channel[1]);
    for (;;) {
        int status;
        pid_t reaped = waitpid(worker, &status, WNOHANG);
        if (reaped == worker) {
            result = -EIO;
            int child_result;
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                read(channel[0], &child_result, sizeof(child_result)) == sizeof(child_result))
                result = child_result;
            close(channel[0]); return result;
        }
        if (reaped < 0 && errno != EINTR) {
            result = -errno; close(channel[0]); return result;
        }
        long long now = monotonic_ms();
        if (now < 0 || now - start >= 2000) { result = now < 0 ? -EIO : -ETIMEDOUT; break; }
        struct timespec pause = {.tv_nsec = 2000000};
        nanosleep(&pause, NULL);
    }
    (void)kill(worker, SIGKILL);
    /* SIGKILL cannot resolve D-state IO. Never wait without a deadline; the
     * worker retains the lease and init will reap it if it outlives this CLI. */
    for (int attempt = 0; attempt < 10; attempt++) {
        pid_t reaped = waitpid(worker, NULL, WNOHANG);
        if (reaped == worker || (reaped < 0 && errno == ECHILD)) break;
        struct timespec pause = {.tv_nsec = 10000000};
        nanosleep(&pause, NULL);
    }
    close(channel[0]); return result;
}
