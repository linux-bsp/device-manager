#ifndef FWCTLD_NETWORK_H
#define FWCTLD_NETWORK_H
#include <net/if.h>
#include "dm/transfer.h"
void fwctld_network_context(const char *interface, const char *device);
void fwctld_hardware_context(const char *hwid);
void fwctld_request_init(struct dm_transfer_request *request);
int fwctld_network_ready(const char *interface);
#endif
