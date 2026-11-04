#ifndef DM_TIME_CONTROL_H
#define DM_TIME_CONTROL_H
void dm_clock_cancel(void);
void dm_clock_cycle(const char *server,const char *interface,const char *device,const char *key);
int dm_clock_local(char source[512],int *enabled,unsigned *interval);
#endif
