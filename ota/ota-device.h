#ifndef OTA_DEVICE_H
#define OTA_DEVICE_H
#include "ota-common.h"
#include <stdio.h>
#define OTA_REPORT_SIZE 768
struct ota_report {
    char device[64], product[64], layout[64], boot[64], phase[32], reason[64];
    unsigned long long publication;
    unsigned version, target, running;
};
int ota_report_parse(const char *text, struct ota_report *r);
int ota_report_format(char text[OTA_REPORT_SIZE], const struct ota_report *r);
int ota_device_read(const char *root, const char *device, char text[OTA_REPORT_SIZE]);
int ota_device_write(const char *root, const struct ota_report *r);
int ota_report_incident(const struct ota_report *r);
const char *ota_report_category(const struct ota_report *r);
int ota_history_summary(const char *root, FILE *out);
unsigned long long ota_history_last_good(const char *root, unsigned long long before, const char *product, const char *layout);
int ota_history_incidents(const char *root);
int ota_history_seed(const char *root);
int ota_history_record(const char *root, const struct ota_report *r);
int ota_history_json(const char *root, FILE *out);
int ota_devices_json(const char *root, FILE *out);
int ota_rootfs_recovery_path(const char *root, const char *request, char directory[OTA_PATH]);
int ota_recovery_path(const char *root, const char *request, char directory[OTA_PATH]);
#endif
