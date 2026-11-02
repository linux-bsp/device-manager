#ifndef FWCTLD_LOG_H
#define FWCTLD_LOG_H
void fwctld_log_init(const char *console);
void fwctld_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
void fwctld_error(const char *format, ...) __attribute__((format(printf, 1, 2)));
int fwctld_run_logged(char *const argv[]);
#endif
