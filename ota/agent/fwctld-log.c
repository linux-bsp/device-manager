#define _POSIX_C_SOURCE 200809L
#include "ota-common.h"
#include "fwctld-log.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int console_fd = -1;

void fwctld_log_init(const char *console)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (console && *console) {
        console_fd = open(console, O_WRONLY | O_APPEND | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
        if (console_fd < 0)
            fprintf(stderr, "[fwctld] [WARN] cannot open console %s: %s; file logging continues\n",
                console, strerror(errno));
    }
}

static void emit(const char *message)
{
    size_t n = strlen(message);
    fputs(message, stdout);
    fflush(stdout);
    /* A blocked/disconnected console must not stall an update. */
    if (console_fd >= 0) {
        ssize_t sent;
        do { sent = write(console_fd, message, n); } while (sent < 0 && errno == EINTR);
        (void)sent;
    }
}
static void log_format(const char *level, const char *format, va_list args)
{
    char message[2048];
    size_t prefix = (size_t)snprintf(message, sizeof(message), "[fwctld] [%s] ", level);
    vsnprintf(message + prefix, sizeof(message) - prefix - 1, format, args);
    size_t n = strlen(message);
    if (!n || message[n - 1] != '\n') message[n++] = '\n';
    message[n] = 0;
    emit(message);
}
void fwctld_log(const char *format, ...)
{
    va_list args; va_start(args, format);
    log_format("INFO", format, args); va_end(args);
}
void fwctld_error(const char *format, ...)
{
    va_list args; va_start(args, format);
    log_format("ERROR", format, args); va_end(args);
}
static void child_line(char *line, size_t size)
{
    line[size] = 0;
    if (!strncmp(line, "[fwctl] [", 9)) {
        line[size] = '\n'; line[size + 1] = 0;
        emit(line);
    } else if (size) fwctld_log("%s", line);
}

/* Drain fwctl stdout AND stderr while it runs, instead of buffering until exit. */
int fwctld_run_logged(char *const argv[])
{
    int pipes[2], status, failed = 0;
    if (pipe(pipes)) return -1;
    pid_t pid = fork();
    if (!pid) {
        close(pipes[0]);
        if (dup2(pipes[1], STDOUT_FILENO) < 0 || dup2(pipes[1], STDERR_FILENO) < 0) _exit(126);
        close(pipes[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pipes[1]);
    if (pid < 0) { close(pipes[0]); return -1; }
    char buffer[1024], line[2048];
    size_t used = 0;
    for (;;) {
        ssize_t n = read(pipes[0], buffer, sizeof(buffer) - 1);
        if (n < 0 && errno == EINTR) {
            if (ota_stop) kill(pid, SIGTERM);
            continue;
        }
        if (n <= 0) { failed = n < 0; break; }
        for (ssize_t i = 0; i < n; i++) {
            if (buffer[i] == '\n') { child_line(line, used); used = 0; }
            else {
                line[used++] = buffer[i];
                if (used == sizeof(line) - 2) { child_line(line, used); used = 0; }
            }
        }
    }
    if (used) child_line(line, used);
    close(pipes[0]);
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
        if (ota_stop) kill(pid, SIGTERM);
    }
    return !failed && WIFEXITED(status) && !ota_stop ? WEXITSTATUS(status) : -1;
}
