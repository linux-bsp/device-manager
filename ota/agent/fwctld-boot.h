#ifndef FWCTLD_BOOT_H
#define FWCTLD_BOOT_H
/* Optional evidence must not indefinitely delay an already committed reboot. */
int fwctld_boot_event(const char *event);
#endif
