#ifndef FWCTLD_STATE_H
#define FWCTLD_STATE_H
#include "ota-device.h"

struct fwctld_record {
    unsigned long long sequence;
    char tag[32], identity[65];
    struct ota_report report;
};
struct fwctld_journal {
    char directory[OTA_PATH], device[64], hwid[17];
    int lock;
};
int fwctld_state_open(struct fwctld_journal *journal, const char *base,
                     const char *device, const char *hwid);
void fwctld_state_close(struct fwctld_journal *journal);
/* Read returns 1 if empty, 0 if valid, -1 for damaged/unavailable state. */
int fwctld_state_read(struct fwctld_journal *journal, struct fwctld_record *record);
int fwctld_state_append(struct fwctld_journal *journal, struct fwctld_record *record);
int fwctld_state_first(struct fwctld_journal *journal, struct fwctld_record *record);
int fwctld_state_ack(struct fwctld_journal *journal, unsigned long long sequence);
#endif
