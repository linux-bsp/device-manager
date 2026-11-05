/* SPDX-License-Identifier: GPL-2.0+ */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifndef FWCTL_SYSTEMCTL
#define FWCTL_SYSTEMCTL "/usr/bin/systemctl"
#endif
static int64_t now_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return -1;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
/* No libsystemd dependency in the independent recovery initramfs. The query
 * child, output size and total wait are bounded, including EINTR retries. */
int fwctl_systemd_unit_health(const char *unit)
{
    int pipes[2], status = 0, result = -EAGAIN;
    size_t used = 0;
    char output[1024];
    if (!unit || !*unit || strlen(unit) > 128 ||
        strspn(unit, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.@-:") != strlen(unit) ||
        unit[0] == '-') return -EINVAL;
    if (pipe2(pipes, O_CLOEXEC | O_NONBLOCK)) return -errno;
    pid_t child = fork();
    if (child < 0) { result = -errno; close(pipes[0]); close(pipes[1]); return result; }
    if (!child) {
        close(pipes[0]);
        dup2(pipes[1], STDOUT_FILENO); close(pipes[1]);
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) { dup2(null, STDERR_FILENO); close(null); }
        execl(FWCTL_SYSTEMCTL, "systemctl", "show", "--no-pager",
              "--property=LoadState,ActiveState,SubState,Result", "--", unit, (char *)NULL);
        _exit(127);
    }
    close(pipes[1]);
    int64_t deadline = now_ms() + 1000;
    int exited = 0;
    for (;;) {
        ssize_t n = read(pipes[0], output + used, sizeof(output) - 1 - used);
        if (n > 0) {
            used += (size_t)n;
            if (used == sizeof(output) - 1) { result = -EOVERFLOW; break; }
        } else if (n < 0 && errno != EAGAIN && errno != EINTR) { result = -errno; break; }
        if (!exited) {
            pid_t p = waitpid(child, &status, WNOHANG);
            if (p == child) exited = 1;
            else if (p < 0 && errno != EINTR) break;
        }
        if (exited && n <= 0) {
            output[used] = 0;
            if (!WIFEXITED(status) || WEXITSTATUS(status)) break;
            int loaded = 0, active = 0, failed = 0;
            char *save = NULL;
            for (char *line = strtok_r(output, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
                if (!strcmp(line, "LoadState=loaded")) loaded = 1;
                if (!strcmp(line, "ActiveState=active")) active = 1;
                if (!strcmp(line, "ActiveState=failed") || !strcmp(line, "LoadState=not-found") ||
                    !strcmp(line, "LoadState=masked")) failed = 1;
            }
            result = failed ? -EIO : loaded && active ? 0 : -EAGAIN;
            break;
        }
        int64_t left = deadline - now_ms();
        if (left <= 0) { result = -ETIMEDOUT; break; }
        struct pollfd fd = {.fd=pipes[0], .events=POLLIN};
        /* Bound polling even when a peer closes stdout before exiting. */
        if (n == 0) poll(NULL, 0, left < 10 ? (int)left : 10);
        else poll(&fd, 1, left < 10 ? (int)left : 10);
    }
    close(pipes[0]);
    if (!exited) {
        kill(child, SIGKILL);
        for (unsigned i=0; i<10; i++) {
            if (waitpid(child, &status, WNOHANG) == child) break;
            struct timespec pause = {.tv_nsec=1000000}; nanosleep(&pause, NULL);
        }
    }
    return result;
}
