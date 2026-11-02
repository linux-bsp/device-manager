#define _XOPEN_SOURCE 700
#include "ota-common.h"
#include "fwctld-auto.h"
#include "fwctld-log.h"
#include "fwctld-network.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void stop_daemon(int sig)
{
    (void)sig;
    ota_stop = 1;
}

static int download(const char *url, const char *path, unsigned long long limit, int allow_absent, const char *device)
{
    char maximum[32], status[16];
    (void)device;
    snprintf(maximum, sizeof(maximum), "%llu", limit);
    char *args[] = {"curl", "--disable", "--silent", "--show-error",
        "--noproxy", "*", "--proto", "=http,https", "--connect-timeout", "10",
        "--max-time", "1800", "--speed-limit", "1024", "--speed-time", "30",
        "--max-filesize", maximum, "--output", (char *)path, "--write-out", "%{http_code}", "--url", (char *)url, NULL};
    if (fwctld_request(args, status, sizeof(status)))
        return -1;
    if (allow_absent && (!strcmp(status, "404") || !strcmp(status, "204"))) return 1;
    if (strcmp(status, "200")) { fwctld_error("HTTP %s while downloading %s", status, url); return -1; }
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    int rc = fsync(fd);
    close(fd);
    return rc;
}

/* Return 1 when installed version and payload hashes already match. */
static int poll_release(const char *server, const char *cache, const char *product, const char *layout,
                        int auto_install, int force, const char *device)
{
    static unsigned long long last_blocked;
    static int waiting;
    char temp[OTA_PATH], path[OTA_PATH], url[OTA_PATH], current[OTA_PATH], dest[OTA_PATH], name[64];
    struct release r, old;
    char label[128];
    int rc = -1, unchanged = 0;
    if (ota_path(temp, cache, ".download-XXXXXX") || !mkdtemp(temp))
        return -1;
    if (ota_path(path, temp, "manifest") || ota_path(url, server, "latest")) goto done;
    int fetched = download(url, path, 2048, 1, device);
    if (fetched == 1) {
        if (!waiting) fwctld_log("No active OTA publication; waiting");
        waiting = 1; rc = 1; goto done;
    }
    waiting = 0;
    if (fetched || ota_read(path, &r)) {
        fwctld_error("Cannot fetch a valid publication manifest; check network/server");
        goto done;
    }
    if (strcmp(r.product, product) || strcmp(r.layout, layout)) {
        fwctld_error("OTA product/layout mismatch\n");
        goto done;
    }
    if (fwctld_release_label(server, r.version, label)) { fwctld_error("Cannot fetch firmware tag/content identity"); goto done; }
    if (auto_install && !force) {
        int blocked = fwctld_retry_blocked(server, device, r.version);
        if (blocked < 0) { fwctld_error("Cannot verify previous upgrade failures; automatic installation deferred"); goto done; }
        if (blocked) {
            if (last_blocked != r.version) fwctld_log("Firmware %s is quarantined globally; automatic retry blocked", label);
            last_blocked = r.version; rc = 1; goto done;
        }
        last_blocked = 0;
        int matches = fwctld_image_matches(server, r.version);
        if (matches < 0) { fwctld_log("Firmware identity comparison failed"); goto done; }
        if (matches) {
            rc = 1;
            goto done;
        }
    }
    if (force) fwctld_log("Force recovery requested for firmware %s; downloading and reinstalling", label);
    if (ota_path(current, cache, "current"))
        goto done;
    if (!force && !access(current, F_OK)) {
        if (ota_read(current, &old)) {
            fwctld_error("Invalid local current record; inspect cache\n");
            goto done;
        }
        if (strcmp(old.product, product) || strcmp(old.layout, layout)) {
            fwctld_error("Cache belongs to a different product/layout\n");
            goto done;
        }
        if (r.version < old.version) {
            fwctld_error("Refusing stale firmware manifest; server publication predates the cached firmware");
            goto done;
        }
        if (r.version == old.version) {
            for (int i = 0; i < 3; i++)
                if (strcmp(r.hash[i], old.hash[i]) || r.size[i] != old.size[i]) {
                    fwctld_error("Published version changed unexpectedly\n");
                    goto done;
                }
            unchanged = 1;
        }
    }
    if (!unchanged) {
        fwctld_log("Discovered firmware %s; preparing download", label);
    }
    snprintf(name, sizeof(name), "%llu", r.version);
    if (ota_path(dest, cache, name))
        goto done;
    if (!access(dest, F_OK) && auto_install && (force || ota_verify(dest, &r))) {
        fwctld_log("Refreshing payload cache for firmware %s", label);
        for (int i = 0; i < 3; i++) {
            if (ota_path(path, dest, ota_files[i]) || (unlink(path) && errno != ENOENT)) goto done;
        }
        if (ota_path(path, dest, "manifest") || (unlink(path) && errno != ENOENT) || rmdir(dest)) goto done;
    }
    if (!access(dest, F_OK)) {
        if (ota_verify(dest, &r)) {
            fwctld_error("Cached files damaged; remove affected version directory and retry\n");
            goto done;
        }
    } else {
        for (int i = 0; i < 3; i++) {
            char relative[96];
            snprintf(relative, sizeof(relative), "releases/%llu/%s", r.version, ota_files[i]);
            fwctld_log("Downloading %s (%llu bytes), firmware %s", ota_files[i], r.size[i], label);
            if (ota_path(url, server, relative) || ota_path(path, temp, ota_files[i]) ||
                download(url, path, r.size[i], 0, device)) {
                fwctld_error("Download failed: %s, firmware %s", ota_files[i], label);
                if (auto_install) (void)fwctld_report_problem(server, device, product, layout, r.version, "device-error", "download-failed");
                goto done;
            }
            fwctld_log("Download complete: %s", ota_files[i]);
        }
        fwctld_log("Verifying SHA256 for firmware %s", label);
        if (ota_verify(temp, &r)) {
            if (auto_install) (void)fwctld_report_problem(server, device, product, layout, r.version, "verify-failed", "download-sha256-mismatch");
            goto done;
        }
        if (ota_sync_dir(temp) || rename(temp, dest) || ota_sync_dir(cache)) {
            if (auto_install) (void)fwctld_report_problem(server, device, product, layout, r.version, "device-error", "cache-storage-failed");
            goto done;
        }
        fwctld_log("Downloaded and SHA256-verified firmware %s", label);
    }
    if (unchanged && !force) {
        fwctld_log("Already cached firmware %s", label);
        rc = 0;
        goto done;
    }
    /* current means downloaded, NOT installed or boot-confirmed. */
    char record[OTA_PATH];
    snprintf(name, sizeof(name), ".current-%ld", (long)getpid());
    if (ota_path(record, cache, name))
        goto done;
    unlink(record);
    if (ota_write(record, &r) || rename(record, current) || ota_sync_dir(cache))
        goto done;
    fwctld_log("Ready: firmware %s", label);
    rc = 0;
done:
    for (int i = 0; i < 3; i++)
        if (!ota_path(path, temp, ota_files[i]))
            unlink(path);
    if (!ota_path(path, temp, "manifest"))
        unlink(path);
    rmdir(temp);
    return rc;
}

int main(int argc, char **argv)
{
    const char *server = NULL, *cache_arg = NULL, *product = NULL, *layout = NULL;
    unsigned long interval = 60;
    int once = 0, auto_install = 0, force = 0;
    const char *device = NULL, *console = NULL, *interface = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--force")) { force = 1; continue; }
        if (!strcmp(argv[i], "--auto-install")) {
            auto_install = 1;
            continue;
        }
        if (!strcmp(argv[i], "--once")) {
            once = 1;
            continue;
        }
        if (i + 1 == argc)
            goto usage;
        if (!strcmp(argv[i], "--console"))
            console = argv[++i];
        else if (!strcmp(argv[i], "--interface"))
            interface = argv[++i];
        else if (!strcmp(argv[i], "--device"))
            device = argv[++i];
        else if (!strcmp(argv[i], "--server"))
            server = argv[++i];
        else if (!strcmp(argv[i], "--cache"))
            cache_arg = argv[++i];
        else if (!strcmp(argv[i], "--product"))
            product = argv[++i];
        else if (!strcmp(argv[i], "--layout"))
            layout = argv[++i];
        else if (!strcmp(argv[i], "--interval")) {
            char *end;
            errno = 0;
            interval = strtoul(argv[++i], &end, 10);
            if (errno || *end || interval < 1 || interval > 86400)
                goto usage;
        } else
            goto usage;
    }
    if (device && !ota_identifier(device)) goto usage;
    if (interface && (!*interface || strlen(interface) >= IF_NAMESIZE)) goto usage;
    if ((force && (!once || !auto_install)) || (auto_install && (!device || !ota_identifier(device))) || !server || !cache_arg || !product || !layout || !ota_identifier(product) ||
        !ota_identifier(layout) || (strncmp(server, "http://", 7) && strncmp(server, "https://", 8)) ||
        strchr(server, '?') || strchr(server, '#') || strlen(server) > 3000)
        goto usage;
    fwctld_log_init(console);
    char base[OTA_PATH], cache[OTA_PATH];
    strcpy(base, server);
    size_t length = strlen(base);
    while (length && base[length - 1] == '/')
        base[--length] = 0;
    if (ota_mkdir(cache_arg) || !realpath(cache_arg, cache)) {
        perror("OTA cache (create its parent first)");
        return 1;
    }
    int lock = ota_lock(cache);
    if (lock < 0) {
        fwctld_log("Another fwctld instance is active or cache lock unavailable\n");
        return 1;
    }
    struct sigaction action = {.sa_handler = stop_daemon};
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    fwctld_log("Started; polling every %lu seconds; automatic installation %s", interval, auto_install ? "enabled" : "disabled");
    int rc, waiting_for_network = 0;
    do {
        if (interface && !fwctld_network_ready(interface)) {
            if (!waiting_for_network)
                fwctld_log("Waiting for network: %s needs link UP and an IP address; OTA checks deferred", interface);
            waiting_for_network = 1;
            rc = 1;
            goto wait_for_next_check;
        }
        waiting_for_network = 0;
        fwctld_network_context(interface, device);
        const char *stage = "transaction reconciliation";
        rc = auto_install ? fwctld_auto_reconcile(base, device, product, layout) : 0;
        if (!rc) {
            stage = "publication query/download";
            rc = poll_release(base, cache, product, layout, auto_install, force, device);
            if (!rc && auto_install) {
                stage = "installation/result reporting";
                rc = fwctld_auto_install(base, cache, device, product, layout);
            }
        }
        if (rc > 0) rc = 0;
        if (rc) {
            if (once) fwctld_log("OTA %s failed; inspect preceding error", stage);
            else fwctld_log("OTA %s incomplete; retrying in %lu seconds", stage, interval);
        }
wait_for_next_check:
        fflush(stdout);
        if (once || ota_stop)
            break;
        struct timespec delay = {.tv_sec = waiting_for_network ? 1 : (time_t)interval};
        while (nanosleep(&delay, &delay) && errno == EINTR && !ota_stop)
            ;
    } while (!ota_stop);
    close(lock);
    return ota_stop ? 0 : (rc ? 1 : 0);
usage:
    fwctld_log("Usage: fwctld --server URL --cache DIR --product ID --layout ID "
            "[--console PATH] [--interface IFACE] [--interval SECONDS] [--once] [--auto-install --device ID] [--force (requires --once --auto-install)]\n");
    return 1;
}
