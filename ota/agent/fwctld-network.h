#ifndef FWCTLD_NETWORK_H
#define FWCTLD_NETWORK_H
#include <net/if.h>
void fwctld_network_context(const char *interface, const char *device);
int fwctld_request(char *const args[], char *output, unsigned size);
int fwctld_network_ready(const char *interface);
#endif
