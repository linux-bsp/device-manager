#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "fwctld-auto.h"
#include "ota-device.h"
#include "fwctld-log.h"
#include "fwctld-network.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct fw_snapshot {
    unsigned booted, active, pending, version[2], good[2], state[2];
};
static int reboot_requested;

static int snapshot(struct fw_snapshot *s)
{
    char text[512], extra;
    char *args[] = {"fwctl", "ota-status", NULL};
    if (ota_run(args, text, sizeof(text)) || sscanf(text,
        "FWOTA1 %u %u %u %u %u %u %u %u %u %c", &s->booted, &s->active, &s->pending,
        &s->version[0], &s->good[0], &s->state[0], &s->version[1], &s->good[1], &s->state[1], &extra) != 9 ||
        s->booted > 1 || s->active > 1 || (s->pending > 1 && s->pending != 255) ||
        s->good[0] > 1 || s->good[1] > 1 || s->state[0] > 5 || s->state[1] > 5) {
        fwctld_error("Cannot read valid local firmware status (fwctl ota-status)");
        return -1;
    }
    return 0;
}

static int boot_id(char id[64])
{
    FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
    if (!f) return -1;
    int rc = fgets(id, 64, f) ? 0 : -1;
    fclose(f);
    id[strcspn(id, "\r\n")] = 0;
    return rc || !ota_identifier(id) ? -1 : 0;
}

static int get(const char *url, char *text, unsigned size)
{
    char *args[] = {"curl", "--disable", "--fail", "--silent", "--show-error",
        "--noproxy", "*", "--proto", "=http,https", "--connect-timeout", "5",
        "--max-time", "15", "--max-filesize", "767", "--url", (char *)url, NULL};
    return fwctld_request(args, text, size);
}

int fwctld_release_label(const char *server, unsigned long long publication, char label[128])
{
    char relative[128], url[OTA_PATH], text[192], tag[32], hash[65], extra;
    unsigned long long number;
    snprintf(relative, sizeof(relative), "releases/%llu/version", publication);
    if (ota_path(url, server, relative) || get(url, text, sizeof(text)) ||
        sscanf(text, "BSPOTAVERSION1 %llu %31s %64s %c", &number, tag, hash, &extra) != 3 ||
        number != publication || tag[0] != 'v' || strspn(tag + 1, "0123456789.") != strlen(tag + 1) ||
        strlen(hash) != 64 || strspn(hash, "0123456789abcdef") != 64) return -1;
    snprintf(label, 128, "%s / %.8s", tag, hash); return 0;
}

int fwctld_retry_blocked(const char *server, const char *device, unsigned long long publication)
{
    char relative[160], url[OTA_PATH], text[128], state[16], extra;
    unsigned long long number;
    snprintf(relative, sizeof(relative), "api/policy/%s/%llu", device, publication);
    if (ota_path(url, server, relative) || get(url, text, sizeof(text)) ||
        sscanf(text, "BSPOTAPOLICY1 %llu %15s %c", &number, state, &extra) != 2 || number != publication) return -1;
    if (!strcmp(state, "blocked")) return 1;
    return strcmp(state, "allow") ? -1 : 0;
}

/* 1 means installed version and actual flash contents match; 0 needs install. */
int fwctld_image_matches(const char *server, unsigned long long publication)
{
    char relative[128], url[OTA_PATH], text[512], extra;
    unsigned long long number, size[2]; unsigned version;
    char hash[2][65], v[32], lengths[2][32];
    snprintf(relative, sizeof(relative), "releases/%llu/identity", publication);
    if (ota_path(url, server, relative) || get(url, text, sizeof(text)) ||
        sscanf(text, "BSPOTAIMAGE1 %llu %u %llu %64s %llu %64s %c", &number, &version,
            &size[0], hash[0], &size[1], hash[1], &extra) != 6 ||
        number != publication || !version || !size[0] || !size[1] ||
        size[0] > OTA_MAX_FILE || size[1] > OTA_MAX_FILE) return -1;
    snprintf(v, sizeof(v), "%u", version);
    for (unsigned i = 0; i < 2; i++) {
        if (strlen(hash[i]) != 64 || strspn(hash[i], "0123456789abcdef") != 64) return -1;
        snprintf(lengths[i], sizeof(lengths[i]), "%llu", size[i]);
    }
    char *args[] = {"fwctl", "ota-check", v, lengths[0], hash[0], lengths[1], hash[1], NULL};
    if (ota_run(args, text, sizeof(text))) {
        fwctld_error("Cannot compare installed firmware; use explicit force recovery if storage is unreadable");
        return -1;
    }
    if (!strcmp(text, "FWCHECK1 match\n")) return 1;
    if (!strcmp(text, "FWCHECK1 different\n")) return 0;
    return -1;
}

static int read_report(const char *server, const char *device, const char *product,
                       const char *layout, struct ota_report *r)
{
    char relative[128], url[OTA_PATH], text[OTA_REPORT_SIZE];
    snprintf(relative, sizeof(relative), "api/devices/%s", device);
    if (ota_path(url, server, relative) || get(url, text, sizeof(text))) {
        fwctld_error("Cannot query device transaction from OTA server; network/HTTP request failed");
        return -1;
    }
    if (!*text) { memset(r, 0, sizeof(*r)); return 0; }
    if (ota_report_parse(text, r) || strcmp(r->device, device) ||
        strcmp(r->product, product) || strcmp(r->layout, layout)) {
        fwctld_error("Invalid server transaction or mismatched device/product/layout");
        return -1;
    }
    return 0;
}

static int report(const char *server, struct ota_report *r, const char *phase, unsigned running)
{
    char relative[128], url[OTA_PATH], text[OTA_REPORT_SIZE], response[64];
    if (strcmp(r->phase, phase)) {
        const char *reason = !strcmp(phase, "rolled-back") ? "boot-failed-rolled-back" :
            !strcmp(phase, "interrupted") ? "transaction-interrupted" :
            !strcmp(phase, "reboot-failed") ? "reboot-command-failed" : "none";
        snprintf(r->reason, sizeof(r->reason), "%s", reason);
    }
    if (phase != r->phase) snprintf(r->phase, sizeof(r->phase), "%s", phase);
    r->running = running;
    snprintf(relative, sizeof(relative), "api/devices/%s", r->device);
    if (ota_report_format(text, r) || ota_path(url, server, relative)) return -1;
    char *args[] = {"curl", "--disable", "--fail", "--silent", "--show-error",
        "--noproxy", "*", "--proto", "=http,https", "--connect-timeout", "5", "--max-time", "15",
        "--header", "X-OTA-Request: 1", "--header", "Content-Type: text/plain",
        "--data-binary", text, "--url", url, NULL};
    if (fwctld_request(args, response, sizeof(response)) || strcmp(response, "ok\n")) return -1;
    /* Persist every phase, but local boot confirmation already has one owner. */
    if (!strcmp(phase, "boot-testing") || !strcmp(phase, "success") || !strcmp(phase, "up-to-date")) return 0;
    char label[128];
    if (fwctld_release_label(server, r->publication, label))
        snprintf(label, sizeof(label), "v%u.%u.%u / identity-unavailable", r->version >> 16, (r->version >> 8) & 255, r->version & 255);
    fwctld_log("OTA firmware %s: %s (running=v%u.%u.%u)",
        label, phase, running >> 16, (running >> 8) & 255, running & 255);
    return 0;
}

static int reboot_device(const char *server, struct ota_report *r, const struct fw_snapshot *s)
{
    if (reboot_requested) return 1;
    /* The server must durably acknowledge the transaction before reboot. */
    if (report(server, r, "rebooting", s->version[s->booted])) return -1;
    fwctld_log("Installation verified; rebooting now. Success requires health confirmation after boot.");
    sync();
    char *args[] = {"reboot", NULL};
    if (ota_run(args, NULL, 0)) {
        (void)report(server, r, "reboot-failed", s->version[s->booted]);
        return -1;
    }
    reboot_requested = 1;
    return 1;
}

/* Returns 1 while an update/boot test blocks new installation, 0 when idle. */
int fwctld_auto_reconcile(const char *server, const char *device, const char *product, const char *layout)
{
    struct ota_report r;
    struct fw_snapshot s;
    char boot[64] = "";
    if (read_report(server, device, product, layout, &r) || snapshot(&s) || boot_id(boot)) return -1;
    if (!r.publication) return s.pending != 255 || !s.good[s.booted] ? 1 : 0;
    int in_progress = !strcmp(r.phase, "installing") || !strcmp(r.phase, "rebooting") ||
                      !strcmp(r.phase, "boot-testing");
    if (!in_progress) return s.pending != 255 || !s.good[s.booted] ? 1 : 0;
    if (s.booted == r.target && s.version[s.booted] == r.version) {
        if (s.good[s.booted] && s.state[s.booted] == 4)
            return report(server, &r, "success", s.version[s.booted]) ? -1 : 0;
        if (strcmp(r.phase, "boot-testing") && report(server, &r, "boot-testing", s.version[s.booted])) return -1;
        return 1; /* S99fw-mark-good and the existing watchdog own health policy. */
    }
    if (!strcmp(boot, r.boot) && s.pending == r.target &&
        s.version[r.target] == r.version && s.state[r.target] == 2)
        return reboot_device(server, &r, &s);
    if (strcmp(boot, r.boot) && s.pending == r.target && s.version[r.target] == r.version)
        return 1; /* Boot-once or another boot path: do not claim rollback yet. */
    return report(server, &r, s.state[r.target] == 5 ? "rolled-back" : "interrupted",
        s.version[s.booted]) ? -1 : 0;
}

int fwctld_auto_install(const char *server, const char *cache, const char *device,
                     const char *product, const char *layout)
{
    struct fw_snapshot s;
    struct ota_report old, r = {0};
    struct release release;
    char path[OTA_PATH], relative[128], url[OTA_PATH], info[128], boot[64] = "", extra;
    unsigned long long publication;
    unsigned version;
    if (snapshot(&s) || boot_id(boot) || read_report(server, device, product, layout, &old) ||
        ota_path(path, cache, "current") || ota_read(path, &release)) return -1;
    if (s.pending != 255 || s.active != s.booted || !s.good[s.booted] || s.state[s.booted] != 4)
        return 0;
    snprintf(relative, sizeof(relative), "releases/%llu/info", release.version);
    if (ota_path(url, server, relative) || get(url, info, sizeof(info)) ||
        sscanf(info, "BSPOTAINFO1 %llu %u %c", &publication, &version, &extra) != 2 ||
        publication != release.version || !version) return -1;
    snprintf(r.device, sizeof(r.device), "%s", device);
    snprintf(r.product, sizeof(r.product), "%s", product);
    snprintf(r.layout, sizeof(r.layout), "%s", layout);
    strcpy(r.boot, boot);
    r.publication = publication; r.version = version; r.target = 1 - s.booted;
    unsigned floor = s.version[s.booted];
    snprintf(relative, sizeof(relative), "%llu/release.itb", publication);
    if (ota_path(path, cache, relative)) return -1;
    char label[128];
    if (fwctld_release_label(server, publication, label)) return -1;
    fwctld_log("Preparing installation: v%u.%u.%u -> %s, deployment %u",
        floor >> 16, (floor >> 8) & 255, floor & 255, label, r.target);
    if (report(server, &r, "installing", s.version[s.booted])) return -1;
    fwctld_log("Installing firmware into inactive deployment %u; do not power off", r.target);
    char *args[] = {"fwctl", "update", path, NULL};
    int update_rc = fwctld_run_logged(args);
    int status_rc = snapshot(&s);
    if (update_rc || status_rc || s.pending != r.target ||
        s.version[r.target] != version || s.state[r.target] != 2) {
        int device_error = update_rc < 0 || update_rc == EIO || update_rc == ENOSPC ||
            update_rc == ENODEV || update_rc == ENOMEM || update_rc == EBUSY || status_rc;
        snprintf(r.phase, sizeof(r.phase), "%s", device_error ? "device-error" : "install-failed");
        if (update_rc) snprintf(r.reason, sizeof(r.reason), "fwctl-exit-%d", update_rc < 0 ? 255 : update_rc);
        else snprintf(r.reason, sizeof(r.reason), "%s", status_rc ? "firmware-status-unavailable" : "pending-deployment-not-ready");
        (void)report(server, &r, r.phase, r.running);
        return -1;
    }
    return reboot_device(server, &r, &s) < 0 ? -1 : 0;
}

int fwctld_report_problem(const char *server, const char *device, const char *product,
                         const char *layout, unsigned long long publication,
                         const char *phase, const char *reason)
{
    struct ota_report r = {0}; struct fw_snapshot s = {0};
    char relative[128], url[OTA_PATH], info[128], extra; unsigned long long number;
    snprintf(relative, sizeof(relative), "releases/%llu/info", publication);
    if (ota_path(url, server, relative) || get(url, info, sizeof(info)) ||
        sscanf(info, "BSPOTAINFO1 %llu %u %c", &number, &r.version, &extra) != 2 ||
        number != publication || boot_id(r.boot)) return -1;
    (void)snapshot(&s);
    snprintf(r.device, sizeof(r.device), "%s", device);
    snprintf(r.product, sizeof(r.product), "%s", product);
    snprintf(r.layout, sizeof(r.layout), "%s", layout);
    snprintf(r.phase, sizeof(r.phase), "%s", phase);
    snprintf(r.reason, sizeof(r.reason), "%s", reason);
    r.publication = publication; r.target = 1 - s.booted;
    return report(server, &r, r.phase, s.version[s.booted]);
}
