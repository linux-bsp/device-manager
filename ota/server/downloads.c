#define _GNU_SOURCE
#include "downloads.h"
#include "ota-admin.h"
#include "http.h"
#include "worker.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DOWNLOAD_WORKERS 4
struct download {
    pid_t pid;
    int count, cancelled;
    unsigned long long publication;
    char root[OTA_PATH], device[64], kernel_hash[65], rootfs_hash[65];
};
static struct download workers[DOWNLOAD_WORKERS];

static void completed(struct download *job)
{
    if (job->count && ota_catalog_download(job->root, job->publication,
                                          job->device[0] ? job->device : NULL))
        fprintf(stderr, "[device-server] [ERROR] Cannot record completed firmware download\n");
}

void server_downloads_poll(void)
{
    for (int i = 0; i < DOWNLOAD_WORKERS; i++) {
        struct download *job = &workers[i];
        if (!job->pid) continue;
        int status;
        pid_t result = waitpid(job->pid, &status, WNOHANG);
        if (result == job->pid) {
            if (!job->cancelled && WIFEXITED(status) && WEXITSTATUS(status) == 0) completed(job);
            job->pid = 0; continue;
        }
        if (result < 0 && errno == ECHILD) { job->pid = 0; continue; }
        if (job->publication && !job->cancelled) {
            if (ota_quarantine_hashes_blocked(job->root, job->kernel_hash, job->rootfs_hash)) {
                job->cancelled = 1;
                (void)kill(job->pid, SIGTERM);
                fprintf(stderr, "[device-server] [WARN] Active firmware download cancelled by quarantine policy\n");
            }
        }
    }
}

void server_download(const struct dm_transfer_serve *serve, const char *root,
                      unsigned long long publication, const char *device, int count_download,
                      int *response_deferred)
{
    server_downloads_poll();
    struct download job = {.publication=publication, .count=count_download && !serve->head};
    if ((root && snprintf(job.root, sizeof(job.root), "%s", root) >= (int)sizeof(job.root)) ||
        (device && snprintf(job.device, sizeof(job.device), "%s", device) >= (int)sizeof(job.device))) {
        http_text(serve->socket, 503, "Unavailable", "Download context unavailable\n", serve->head); return;
    }
    /* Tiny metadata and HEAD replies fit within the control path. */
    if (serve->head || serve->size <= 65536) {
        if (!dm_transfer_serve_file(serve)) completed(&job);
        return;
    }
    int slot = -1;
    for (int i = 0; i < DOWNLOAD_WORKERS; i++) if (!workers[i].pid) { slot = i; break; }
    if (slot < 0) {
        http_text(serve->socket, 503, "Unavailable", "Download capacity busy; retry later\n", false); return;
    }
    if (!response_deferred) {
        http_text(serve->socket, 503, "Unavailable", "Download ownership unavailable\n", false); return;
    }
    if (publication) {
        char relative[64], directory[OTA_PATH];
        struct ota_catalog_identity identity;
        snprintf(relative, sizeof(relative), "releases/%llu", publication);
        if (!root || ota_path(directory, root, relative) || ota_catalog_get(root, directory, &identity)) {
            http_text(serve->socket, 503, "Unavailable", "Download identity unavailable\n", false); return;
        }
        /* Reuse catalog identity. Polling active transfers must not rehash
         * whole firmware payloads on every control request. */
        memcpy(job.kernel_hash, identity.kernel, sizeof(job.kernel_hash));
        memcpy(job.rootfs_hash, identity.rootfs, sizeof(job.rootfs_hash));
    }
    pid_t parent = getpid(), child = fork();
    if (child < 0) {
        http_text(serve->socket, 503, "Unavailable", "Download worker unavailable\n", false); return;
    }
    if (!child) {
        if (server_worker_enter(parent, serve->socket, serve->fd, 180)) _exit(125);
        struct dm_transfer_serve transport = *serve;
        memset(&transport.callbacks, 0, sizeof(transport.callbacks));
        _exit(dm_transfer_serve_file(&transport) ? 1 : 0);
    }
    job.pid = child; workers[slot] = job;
    *response_deferred = 1;
}

void server_downloads_stop(void)
{
    server_downloads_poll();
    for (int i = 0; i < DOWNLOAD_WORKERS; i++) if (workers[i].pid) {
        workers[i].cancelled = 1; (void)kill(workers[i].pid, SIGTERM);
    }
    for (int attempt = 0; attempt < 20; attempt++) {
        server_downloads_poll();
        int active = 0;
        for (int i = 0; i < DOWNLOAD_WORKERS; i++) active += workers[i].pid != 0;
        if (!active) return;
        struct timespec pause = {.tv_nsec=10000000}; nanosleep(&pause, NULL);
    }
    for (int i = 0; i < DOWNLOAD_WORKERS; i++) if (workers[i].pid) {
        (void)kill(workers[i].pid, SIGKILL);
        (void)waitpid(workers[i].pid, NULL, WNOHANG);
    }
    /* No unbounded wait for kernel D-state IO. Parent death also sends SIGKILL. */
}
