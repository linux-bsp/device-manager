#include "ota-device.h"
#include <ctype.h>
#include <string.h>

int ota_report_parse(const char *text, struct ota_report *r)
{
    char extra;
    memset(r, 0, sizeof(*r));
    int extended = !strncmp(text, "BSPOTASTATE2 ", 13);
    if (extended) {
        if (sscanf(text, "BSPOTASTATE2 %63s %63s %63s %llu %u %63s %31s %u %u %63s %c",
            r->device, r->product, r->layout, &r->publication, &r->version, r->boot,
            r->phase, &r->target, &r->running, r->reason, &extra) != 10 || !ota_identifier(r->reason)) return -1;
    }
    if ((!extended && sscanf(text, "BSPOTASTATE1 %63s %63s %63s %llu %u %63s %31s %u %u %c",
        r->device, r->product, r->layout, &r->publication, &r->version,
        r->boot, r->phase, &r->target, &r->running, &extra) != 9) ||
        !ota_identifier(r->device) || !isalnum((unsigned char)r->device[0]) ||
        !ota_identifier(r->product) || !ota_identifier(r->layout) ||
        !ota_identifier(r->boot) || !r->publication || r->publication > 2147483647 ||
        !r->version || r->target > 1)
        return -1;
    char canonical[OTA_REPORT_SIZE];
    if (extended) snprintf(canonical, sizeof(canonical), "BSPOTASTATE2 %s %s %s %llu %u %s %s %u %u %s\n",
        r->device, r->product, r->layout, r->publication, r->version, r->boot, r->phase, r->target, r->running, r->reason);
    else snprintf(canonical, sizeof(canonical), "BSPOTASTATE1 %s %s %s %llu %u %s %s %u %u\n",
        r->device, r->product, r->layout, r->publication, r->version,
        r->boot, r->phase, r->target, r->running);
    if (strcmp(text, canonical)) return -1;
    const char *phases[] = {"installing", "rebooting", "boot-testing", "success",
        "install-failed", "rolled-back", "interrupted", "reboot-failed", "up-to-date", "device-error", "verify-failed"};
    for (unsigned i = 0; i < sizeof(phases) / sizeof(phases[0]); i++)
        if (!strcmp(r->phase, phases[i]))
            return 0;
    return -1;
}

int ota_report_format(char text[OTA_REPORT_SIZE], const struct ota_report *r)
{
    int n;
    if (r->reason[0]) n = snprintf(text, OTA_REPORT_SIZE, "BSPOTASTATE2 %s %s %s %llu %u %s %s %u %u %s\n",
        r->device, r->product, r->layout, r->publication, r->version, r->boot, r->phase, r->target, r->running, r->reason);
    else n = snprintf(text, OTA_REPORT_SIZE, "BSPOTASTATE1 %s %s %s %llu %u %s %s %u %u\n",
        r->device, r->product, r->layout, r->publication, r->version,
        r->boot, r->phase, r->target, r->running);
    struct ota_report check;
    return n < 0 || n >= OTA_REPORT_SIZE || ota_report_parse(text, &check) ? -1 : 0;
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
