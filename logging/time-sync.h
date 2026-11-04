#ifndef DM_TIME_SYNC_H
#define DM_TIME_SYNC_H
/* Returns 0 when synchronized/not due, negative errno on a failed attempt.
 * Waiting for link/address is not a SOL or startup-health failure. */
void dm_time_sync_cancel(void);
int dm_time_sync(const char *server, const char *interface, int force);
#endif
