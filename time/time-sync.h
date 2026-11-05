#ifndef DM_TIME_SYNC_H
#define DM_TIME_SYNC_H
#include <stdint.h>
#include <signal.h>
struct dm_time_sample { int64_t wall,end,rtt; };
int dm_ntp_sample(const char *source, struct dm_time_sample *sample, const volatile sig_atomic_t *cancel);
void dm_time_interval(unsigned seconds);
void dm_time_disabled(const char *source);
/* Returns 0 when synchronized/not due, negative errno on a failed attempt.
 * Waiting for link/address is not a logging or startup-health failure. */
void dm_time_sync_cancel(void);
int dm_time_sync(const char *server, const char *interface, int force);
#endif
