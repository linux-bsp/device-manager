#define _GNU_SOURCE
#include "uploads.h"
#include "worker.h"
#include "http.h"
#include "ota-admin.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define UPLOAD_WORKERS 2
struct upload {
    pid_t pid;
    int client;
    struct ota_prepared_draft *prepared;
    char root[OTA_PATH], temp[OTA_PATH];
};
static struct upload workers[UPLOAD_WORKERS];
static int stopping;

void server_uploads_poll(void)
{
    for (int i = 0; i < UPLOAD_WORKERS; i++) {
        struct upload *job = &workers[i];
        if (!job->pid) continue;
        int status;
        pid_t result = waitpid(job->pid, &status, WNOHANG);
        if (result == 0 || (result < 0 && errno == EINTR)) continue;
        int code = result == job->pid && WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        /* All FIT helpers belong to the upload process group. A timeout or
         * signal must not leave a helper writing an abandoned staging file. */
        (void)kill(-job->pid, SIGKILL);
        char id[32], body[128];
        if (!stopping && !code && !ota_draft_finish(job->root, job->temp, job->prepared, id)) {
            snprintf(body, sizeof(body), "{\"id\":\"%s\",\"status\":\"draft\"}\n", id);
            http_json(job->client, 201, "Created", body, false);
        } else {
            ota_draft_cleanup(job->temp);
            if (!stopping) http_json(job->client, code == 1 ? 400 : 422,
                code == 1 ? "Bad Request" : "Unprocessable Content",
                code == 1 ? "{\"error\":\"上传或校验未完成，可能超过处理时限，请重试。\"}" :
                "{\"error\":\"固件包校验失败；检查 FIT、HWID 和组件 SHA256。\"}", false);
        }
        munmap(job->prepared, sizeof(*job->prepared));
        close(job->client); job->pid = 0;
    }
}

void server_upload(int client, unsigned long long length, const struct server_context *context)
{
    server_uploads_poll();
    int slot = -1;
    for (int i = 0; i < UPLOAD_WORKERS; i++) if (!workers[i].pid) { slot = i; break; }
    if (slot < 0 || !context->response_deferred) {
        http_json(client, 503, "Unavailable", "{\"error\":\"上传任务繁忙，请稍后重试。\"}", false); return;
    }
    struct upload job = {0};
    if (snprintf(job.root, sizeof(job.root), "%s", context->directory) >= (int)sizeof(job.root)) {
        http_json(client, 503, "Unavailable", "{\"error\":\"Upload root unavailable\"}", false); return;
    }
    int file = ota_draft_begin(context->directory, job.temp);
    if (file < 0) {
        http_json(client, 503, "Unavailable", "{\"error\":\"无法创建上传目录，请检查空间或清理旧草稿。\"}", false); return;
    }
    job.client = fcntl(client, F_DUPFD_CLOEXEC, 3);
    if (job.client < 0) {
        close(file); ota_draft_cleanup(job.temp);
        http_json(client, 503, "Unavailable", "{\"error\":\"Upload connection unavailable\"}", false); return;
    }
    job.prepared = mmap(NULL, sizeof(*job.prepared), PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (job.prepared == MAP_FAILED) {
        close(job.client); close(file); ota_draft_cleanup(job.temp);
        http_json(client, 503, "Unavailable", "{\"error\":\"Upload result memory unavailable\"}", false); return;
    }
    pid_t parent = getpid(), child = fork();
    if (child < 0) {
        munmap(job.prepared, sizeof(*job.prepared));
        close(job.client); close(file); ota_draft_cleanup(job.temp);
        http_json(client, 503, "Unavailable", "{\"error\":\"Upload worker unavailable\"}", false); return;
    }
    if (!child) {
        if (setpgid(0, 0) || server_worker_enter(parent, client, file, 180)) _exit(125);
        int rc = dm_transfer_receive_file(client, file, length, OTA_UPLOAD_MAX, 180000, NULL);
        if (close(file)) rc = -EIO;
        if (rc) _exit(1);
        close(client);
        /* FIT helper stdout uses pipes. Keep standard descriptors reserved,
         * and never let a helper inherit the upload response socket. */
        int null = open("/dev/null", O_RDWR);
        if (null < 0) _exit(2);
        for (int fd = 0; fd < 3; fd++) if (dup2(null, fd) < 0) _exit(2);
        if (null > 2) close(null);
        _exit(ota_draft_prepare(job.temp, job.prepared) ? 2 : 0);
    }
    (void)setpgid(child, child);
    close(file); job.pid = child; workers[slot] = job;
    *context->response_deferred = 1;
}

void server_uploads_stop(void)
{
    stopping = 1;
    for (int i = 0; i < UPLOAD_WORKERS; i++) if (workers[i].pid) {
        (void)kill(-workers[i].pid, SIGTERM);
        (void)kill(workers[i].pid, SIGTERM);
    }
    for (int attempt = 0; attempt < 20; attempt++) {
        server_uploads_poll();
        int active = 0;
        for (int i = 0; i < UPLOAD_WORKERS; i++) active += workers[i].pid != 0;
        if (!active) return;
        struct timespec pause = {.tv_nsec=10000000}; nanosleep(&pause, NULL);
    }
    for (int i = 0; i < UPLOAD_WORKERS; i++) if (workers[i].pid) {
        (void)kill(-workers[i].pid, SIGKILL);
        (void)kill(workers[i].pid, SIGKILL);
        close(workers[i].client);
        /* If kernel IO is stuck, preserve the partial directory for inspection.
         * Never wait forever or rename it into a verified draft. */
    }
}
