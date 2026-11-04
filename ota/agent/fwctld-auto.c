#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "fwctld-auto.h"
#include "ota-device.h"
#include "fwctld-log.h"
#include "fwctld-network.h"
#include "fwctld-diagnostics.h"
#include "fwctld-state.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct fw_snapshot {
    unsigned booted, active, pending, version[2], good[2], state[2];
    char identity[2][65];
};
static int reboot_requested;
static struct fwctld_journal journal = {.lock = -1};
static struct fwctld_record transaction;
static int have_transaction, network_online = 1;
static int state_warning;
static int state_fault_reported;

int fwctld_auto_prepare(const char *directory, const char *device,
                        const char *hwid, int online)
{
    network_online = online;
    if (journal.lock < 0 && fwctld_state_open(&journal, directory, device, hwid)) {
        if (state_warning != 1) fwctld_error("Local OTA journal unavailable; automatic writes deferred");
        state_warning = 1;
        return -1;
    }
    int rc = fwctld_state_read(&journal, &transaction);
    if (rc < 0) {
        if (state_warning != 2) fwctld_error("Local OTA journal is damaged or unreadable; preserving evidence and deferring writes");
        state_warning = 2;
        return -1;
    }
    have_transaction = !rc;
    if (state_warning) fwctld_log("Local OTA journal available; resuming transaction reconciliation");
    state_warning = 0;
    state_fault_reported = 0;
    return 0;
}

void fwctld_auto_close(void)
{
    fwctld_state_close(&journal);
}

static int snapshot(struct fw_snapshot *s)
{
    char text[512], extra;
    char *args[] = {"fwctl", "ota-status", NULL};
    if (ota_run(args, text, sizeof(text)) || sscanf(text,
        "FWOTA2 %u %u %u %u %u %u %64s %u %u %u %64s %c", &s->booted, &s->active, &s->pending,
        &s->version[0], &s->good[0], &s->state[0], s->identity[0],
        &s->version[1], &s->good[1], &s->state[1], s->identity[1], &extra) != 11 ||
        s->booted > 1 || s->active > 1 || (s->pending > 1 && s->pending != 255) ||
        s->good[0] > 1 || s->good[1] > 1 || s->state[0] > 5 || s->state[1] > 5) {
        fwctld_error("Cannot read valid local firmware status (fwctl ota-status)");
        return -1;
    }
    for (unsigned i = 0; i < 2; i++)
        if (strcmp(s->identity[i], "-") &&
            (strlen(s->identity[i]) != 64 || strspn(s->identity[i], "0123456789abcdef") != 64)) {
            fwctld_error("Invalid firmware identity in local status");
            return -1;
        }
    return 0;
}

int fwctld_auto_cache_idle(void)
{
    struct fw_snapshot s;
    struct fwctld_record event;
    if (state_warning || journal.lock < 0 || snapshot(&s) || s.pending != 255 ||
        s.active != s.booted || !s.good[s.booted] || s.state[s.booted] != 4) return 0;
    if (have_transaction && strcmp(transaction.report.phase, "success") &&
        strcmp(transaction.report.phase, "up-to-date")) return 0;
    return fwctld_state_first(&journal, &event) == 1;
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

int fwctld_release_identity(const char *server, unsigned long long publication,
                            char tag[32], char hash[65])
{
    char relative[128], url[OTA_PATH], text[192], parsed_tag[32], parsed_hash[65], extra;
    unsigned long long number;
    snprintf(relative, sizeof(relative), "releases/%llu/version", publication);
    if (ota_path(url, server, relative) || get(url, text, sizeof(text)) ||
        sscanf(text, "BSPOTAVERSION1 %llu %31s %64s %c", &number, parsed_tag, parsed_hash, &extra) != 3 ||
        number != publication || parsed_tag[0] != 'v' ||
        strspn(parsed_tag + 1, "0123456789.") != strlen(parsed_tag + 1) ||
        strlen(parsed_hash) != 64 || strspn(parsed_hash, "0123456789abcdef") != 64) return -1;
    strcpy(tag, parsed_tag);
    strcpy(hash, parsed_hash);
    return 0;
}

int fwctld_release_label(const char *server, unsigned long long publication, char label[128])
{
    char tag[32], hash[65];
    if (fwctld_release_identity(server, publication, tag, hash)) return -1;
    snprintf(label, 128, "%s / %.8s", tag, hash);
    return 0;
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

static int read_report(const char *server, const char *device, const char *hwid, struct ota_report *r)
{
    char relative[128], url[OTA_PATH], text[OTA_REPORT_SIZE];
    snprintf(relative, sizeof(relative), "api/devices/%s", device);
    if (ota_path(url, server, relative) || get(url, text, sizeof(text))) {
        fwctld_error("Cannot query device transaction from OTA server; network/HTTP request failed");
        return -1;
    }
    if (!*text) { memset(r, 0, sizeof(*r)); return 0; }
    if (ota_report_parse(text, r) || strcmp(r->device, device) ||
        strcmp(r->hwid, hwid)) {
        fwctld_error("Invalid server transaction or mismatched device/HWID");
        return -1;
    }
    return 0;
}

static int post_report(const char *server, const struct ota_report *r)
{
    char relative[128], url[OTA_PATH], text[OTA_REPORT_SIZE], response[64];
    snprintf(relative, sizeof(relative), "api/devices/%s", r->device);
    if (ota_report_format(text, r) || ota_path(url, server, relative)) return -1;
    char *args[] = {"curl", "--disable", "--fail", "--silent", "--show-error",
        "--noproxy", "*", "--proto", "=http,https", "--connect-timeout", "5", "--max-time", "15",
        "--header", "X-OTA-Request: 1", "--header", "Content-Type: text/plain",
        "--data-binary", text, "--url", url, NULL};
    return fwctld_request(args, response, sizeof(response)) || strcmp(response, "ok\n") ? -1 : 0;
}

void fwctld_auto_report_state_problem(const char *server, const char *device,
                                      const char *hwid)
{
    if (state_fault_reported || !network_online || !state_warning) return;
    struct ota_report report;
    struct fw_snapshot local;
    /* The failing state store cannot journal its own failure. Report only an
     * existing, server-associated transaction, without inventing a target or
     * attaching unavailable disk logs. Repeated reports are idempotent. */
    if (read_report(server, device, hwid, &report) || !report.publication) return;
    strcpy(report.phase, "device-error");
    strcpy(report.reason, state_warning == 2 ? "data-ota-state-corrupt" : "data-ota-state-unavailable");
    if (!snapshot(&local)) report.running = local.version[local.booted];
    if (!post_report(server, &report)) state_fault_reported = 1;
}

static int flush_reports(const char *server)
{
    if (!network_online) return 1;
    struct fwctld_record event;
    for (;;) {
        int rc = fwctld_state_first(&journal, &event);
        if (rc) return rc > 0 ? 0 : -1;
        char tag[32], identity[65];
        /* Publication numbers are server-local. Never replay into a different
         * publication after a catalog restore/replacement. */
        if (fwctld_release_identity(server, event.report.publication, tag, identity) ||
            strcmp(tag, event.tag) || strcmp(identity, event.identity) ||
            post_report(server, &event.report)) return 1;
        if (fwctld_state_ack(&journal, event.sequence)) return -1;
    }
}

static int report(const char *server, struct ota_report *r, const char *phase, unsigned running)
{
    char text[OTA_REPORT_SIZE], previous[OTA_REPORT_SIZE];
    char previous_reason[64];snprintf(previous_reason,sizeof(previous_reason),"%s",r->reason);
    if (strcmp(r->phase, phase)) {
        const char *reason = !strcmp(phase, "rolled-back") ? "boot-failed-rolled-back" :
            !strcmp(phase, "interrupted") ? "transaction-interrupted" :
            !strcmp(phase, "reboot-failed") ? "reboot-command-failed" : "none";
        snprintf(r->reason, sizeof(r->reason), "%s", reason);
    }
    if (phase != r->phase) snprintf(r->phase, sizeof(r->phase), "%s", phase);
    if(ota_report_incident(r)||!strcmp(phase,"boot-testing")){
        char observed[64];
        int changed_boot=!boot_id(observed)&&strcmp(observed,r->boot);
        int boot_failure=!strcmp(phase,"rolled-back")||!strcmp(phase,"interrupted");
        fwctld_observe_failure(r->reason,changed_boot&&boot_failure);
        if(ota_report_incident(r)&&strncmp(r->reason,"data-",5)&&strncmp(r->reason,"linux-",6)&&
           (!strncmp(previous_reason,"data-",5)||!strncmp(previous_reason,"linux-",6)))
            snprintf(r->reason,sizeof(r->reason),"%s",previous_reason);
    }
    r->running = running;
    if (ota_report_format(text, r)) return -1;
    if (have_transaction && transaction.report.publication == r->publication) {
        struct fwctld_record next = transaction;
        next.report = *r;
        int changed = ota_report_format(previous, &transaction.report) || strcmp(previous, text);
        if (changed && fwctld_state_append(&journal, &next)) {
            fwctld_error("Cannot persist OTA phase %s; local action deferred", phase);
            return -1;
        }
        transaction = next;
        /* Reboot follows the durable local commit immediately. Replay this
         * event after boot; an HTTP acknowledgement is not a reboot gate. */
        if (strcmp(phase, "rebooting") && flush_reports(server))
            fwctld_log("OTA result queued locally; delivery will resume when the server is available");
    } else if (post_report(server, r)) return -1;
    /* Persist every phase, but local boot confirmation already has one owner. */
    if (!strcmp(phase, "boot-testing") || !strcmp(phase, "success") || !strcmp(phase, "up-to-date")) return 0;
    char label[128];
    if (have_transaction && transaction.report.publication == r->publication)
        snprintf(label, sizeof(label), "%s / %.8s", transaction.tag, transaction.identity);
    else if (fwctld_release_label(server, r->publication, label))
        snprintf(label, sizeof(label), "v%u.%u.%u / identity-unavailable", r->version >> 16, (r->version >> 8) & 255, r->version & 255);
    fwctld_log("OTA firmware %s: %s (running=v%u.%u.%u)",
        label, phase, running >> 16, (running >> 8) & 255, running & 255);
    return 0;
}

static int reboot_device(const char *server, struct ota_report *r, const struct fw_snapshot *s)
{
    if (reboot_requested || ota_stop) return 1;
    /* The device must durably record the transaction before reboot. */
    if (report(server, r, "rebooting", s->version[s->booted])) return -1;
    if (ota_stop) return 1;
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
int fwctld_auto_reconcile(const char *server, const char *device, const char *hwid)
{
    struct ota_report r;
    struct fw_snapshot s;
    char boot[64] = "", tag[32], expected_id[65];
    if (snapshot(&s) || boot_id(boot)) return -1;
    if (have_transaction) r = transaction.report;
    else {
        if (!network_online) return 1;
        if (read_report(server, device, hwid, &r)) return -1;
        /* Adopt an upgrade started by the previous agent on its first boot. */
        if (r.publication && (!strcmp(r.phase,"installing") || !strcmp(r.phase,"rebooting") ||
                              !strcmp(r.phase,"boot-testing"))) {
            if (fwctld_release_identity(server, r.publication, transaction.tag, transaction.identity)) return -1;
            transaction.report = r;
            if (fwctld_state_append(&journal, &transaction)) return -1;
            have_transaction = 1;
        }
    }
    if (!r.publication) return s.pending != 255 || !s.good[s.booted] ? 1 : 0;
    int in_progress = !strcmp(r.phase, "installing") || !strcmp(r.phase, "rebooting") ||
                      !strcmp(r.phase, "boot-testing") ||
                      (!strcmp(r.phase, "cancelled") &&
                       have_transaction && s.version[r.target] == r.version &&
                       !strcmp(s.identity[r.target], transaction.identity) &&
                       ((s.pending == r.target && s.state[r.target] == 2) || s.booted == r.target));
    if (!in_progress) {
        if (have_transaction && flush_reports(server)) return 1;
        return s.pending != 255 || !s.good[s.booted] ? 1 : 0;
    }
    if (have_transaction) {
        strcpy(tag, transaction.tag); strcpy(expected_id, transaction.identity);
    } else if (fwctld_release_identity(server, r.publication, tag, expected_id)) {
        fwctld_error("Cannot read target firmware tag and identity for transaction reconciliation");
        return -1;
    }
    if (s.booted == r.target && s.version[s.booted] == r.version &&
        !strcmp(s.identity[s.booted], expected_id)) {
        if (s.good[s.booted] && s.state[s.booted] == 4) {
            if (report(server, &r, "success", s.version[s.booted])) return -1;
            return flush_reports(server) ? 1 : 0;
        }
        if (strcmp(r.phase, "boot-testing") && report(server, &r, "boot-testing", s.version[s.booted])) return -1;
        return 1; /* S99fw-mark-good and the existing watchdog own health policy. */
    }
    if (!strcmp(boot, r.boot) && s.pending == r.target &&
        s.version[r.target] == r.version && !strcmp(s.identity[r.target], expected_id) &&
        s.state[r.target] == 2)
        return reboot_device(server, &r, &s);
    if (strcmp(boot, r.boot) && s.pending == r.target && s.version[r.target] == r.version &&
        !strcmp(s.identity[r.target], expected_id))
        return 1; /* Boot-once or another boot path: do not claim rollback yet. */
    if (report(server, &r, s.state[r.target] == 5 ? "rolled-back" : "interrupted",
               s.version[s.booted])) return -1;
    return flush_reports(server) ? 1 : 0;
}

int fwctld_auto_install(const char *server, const char *cache, const char *device,
                     const char *hwid)
{
    struct fw_snapshot s;
    struct ota_report r = {0};
    struct release release;
    char path[OTA_PATH], relative[128], url[OTA_PATH], info[128], boot[64] = "", extra;
    char expected_tag[32], expected_id[65];
    unsigned long long publication;
    unsigned version;
    if (snapshot(&s) || boot_id(boot) ||
        ota_path(path, cache, "current") || ota_read(path, &release)) return -1;
    if (s.pending != 255 || s.active != s.booted || !s.good[s.booted] || s.state[s.booted] != 4)
        return 0;
    snprintf(relative, sizeof(relative), "releases/%llu/info", release.version);
    if (ota_path(url, server, relative) || get(url, info, sizeof(info)) ||
        sscanf(info, "BSPOTAINFO1 %llu %u %c", &publication, &version, &extra) != 2 ||
        publication != release.version || !version) return -1;
    snprintf(r.device, sizeof(r.device), "%s", device);
    snprintf(r.hwid, sizeof(r.hwid), "%s", hwid);
    strcpy(r.boot, boot);
    r.publication = publication; r.version = version; r.target = 1 - s.booted;
    unsigned floor = s.version[s.booted];
    snprintf(relative, sizeof(relative), "%llu/release.itb", publication);
    if (ota_path(path, cache, relative)) return -1;
    char label[128];
    if (fwctld_release_label(server, publication, label)) return -1;
    if (fwctld_release_identity(server, publication, expected_tag, expected_id)) return -1;
    /* Network independence starts at the local write transaction. Check
     * cancellation/quarantine immediately before entering that boundary. */
    if (fwctld_retry_blocked(server, device, publication)) {
        fwctld_log("Installation deferred: publication policy does not authorize this firmware");
        return -1;
    }
    char active_manifest[768], active_hwid[17], extra_header;
    unsigned long long active_publication;
    snprintf(relative, sizeof(relative), "latest/%s", hwid);
    if (ota_path(url, server, relative) || get(url, active_manifest, sizeof(active_manifest))) return -1;
    char *line_end = strchr(active_manifest, '\n');
    if (!line_end) return -1;
    *line_end = 0;
    if (sscanf(active_manifest, "BSPOTA2 %llu %16s %c", &active_publication,
               active_hwid, &extra_header) != 2 || strcmp(active_hwid, hwid)) return -1;
    if (active_publication != publication) {
        fwctld_log("Publication changed during download; deferring the superseded firmware");
        return 0;
    }
    memset(&transaction, 0, sizeof(transaction));
    strcpy(transaction.tag, expected_tag); strcpy(transaction.identity, expected_id);
    transaction.report = r;
    have_transaction = 1;
    fwctld_log("Preparing installation: v%u.%u.%u -> %s, deployment %u",
        floor >> 16, (floor >> 8) & 255, floor & 255, label, r.target);
    if (report(server, &r, "installing", s.version[s.booted])) return -1;
    if (ota_stop) {
        strcpy(r.phase, "cancelled"); strcpy(r.reason, "agent-stopped-before-write");
        (void)report(server, &r, r.phase, s.version[s.booted]);
        return -1;
    }
    fwctld_log("Installing firmware into inactive deployment %u; do not power off", r.target);
    char *args[] = {"fwctl", "update", path, NULL};
    int update_rc = fwctld_run_logged(args);
    if (ota_stop) {
        strcpy(r.phase, "cancelled"); strcpy(r.reason, "agent-stopped-during-write");
        (void)report(server, &r, r.phase, s.version[s.booted]);
        return -1;
    }
    int status_rc = snapshot(&s);
    if (update_rc || status_rc || s.pending != r.target ||
        s.version[r.target] != version || strcmp(s.identity[r.target], expected_id) ||
        s.state[r.target] != 2) {
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

int fwctld_report_problem(const char *server, const char *device, const char *hwid, unsigned long long publication,
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
    snprintf(r.hwid, sizeof(r.hwid), "%s", hwid);
    snprintf(r.phase, sizeof(r.phase), "%s", phase);
    snprintf(r.reason, sizeof(r.reason), "%s", reason);
    r.publication = publication; r.target = 1 - s.booted;
    return report(server, &r, r.phase, s.version[s.booted]);
}
