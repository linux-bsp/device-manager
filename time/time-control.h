#ifndef DM_TIME_CONTROL_H
#define DM_TIME_CONTROL_H
int dm_clock_init(int enabled, unsigned interval, const char *source);
void dm_clock_cancel(void);
void dm_clock_cycle(const char *server,const char *interface,const char *device,const char *key);
int dm_clock_sync_now(const char *server, const char *interface);
int dm_clock_set_source(const char *source);
int dm_clock_status(const char *server, char *body, unsigned capacity);
#endif
