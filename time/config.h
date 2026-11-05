#ifndef DM_TIME_CONFIG_H
#define DM_TIME_CONFIG_H
struct time_config {
    char server[512], source[512], interface[32], device[64], key[65];
    unsigned interval;
    int enabled;
};
int time_config_load(struct time_config *config);
const char *time_run_directory(void);
#endif
