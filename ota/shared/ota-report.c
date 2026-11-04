#include "ota-device.h"
#include <ctype.h>
#include <string.h>

static int format_report(char text[OTA_REPORT_SIZE], const struct ota_report *r);

int ota_report_parse(const char *text, struct ota_report *r)
{
    char extra;
    memset(r, 0, sizeof(*r));
    if (sscanf(text, "BSPOTASTATE3 %63s %16s %llu %u %63s %31s %u %u %63s %c",
        r->device, r->hwid, &r->publication, &r->version, r->boot,
        r->phase, &r->target, &r->running, r->reason, &extra) != 9 ||
        !ota_identifier(r->device) || !isalnum((unsigned char)r->device[0]) ||
        !ota_hwid_valid(r->hwid) || !ota_identifier(r->reason) ||
        !ota_identifier(r->boot) || !r->publication || r->publication > 2147483647 ||
        !r->version || r->target > 1) return -1;
    char canonical[OTA_REPORT_SIZE];
    if (format_report(canonical, r) || strcmp(text, canonical)) return -1;
    const char *phases[] = {"installing", "rebooting", "boot-testing", "success",
        "install-failed", "rolled-back", "interrupted", "reboot-failed", "up-to-date", "device-error", "verify-failed"};
    for (unsigned i = 0; i < sizeof(phases) / sizeof(phases[0]); i++)
        if (!strcmp(r->phase, phases[i]))
            return 0;
    return -1;
}

static int format_report(char text[OTA_REPORT_SIZE], const struct ota_report *r)
{
    int n = snprintf(text, OTA_REPORT_SIZE, "BSPOTASTATE3 %s %s %llu %u %s %s %u %u %s\n",
        r->device, r->hwid, r->publication, r->version, r->boot,
        r->phase, r->target, r->running, r->reason[0] ? r->reason : "not-reported");
    return n < 0 || n >= OTA_REPORT_SIZE ? -1 : 0;
}

int ota_report_format(char text[OTA_REPORT_SIZE], const struct ota_report *r)
{
    struct ota_report check;
    return format_report(text, r) || ota_report_parse(text, &check) ? -1 : 0;
}

const char *ota_report_category(const struct ota_report *r)
{
    if (!strcmp(r->phase, "rolled-back") || !strcmp(r->phase, "install-failed") || !strcmp(r->phase, "verify-failed")) return "upgrade-failure";
    if (!strcmp(r->phase, "device-error") || !strcmp(r->phase, "interrupted") || !strcmp(r->phase, "reboot-failed")) return "device-error";
    if (!strcmp(r->phase, "success")) return "success";
    if (!strcmp(r->phase, "up-to-date")) return "up-to-date";
    return "in-progress";
}
int ota_report_incident(const struct ota_report *r)
{
    const char *category = ota_report_category(r);
    return !strcmp(category, "upgrade-failure") || !strcmp(category, "device-error");
}
