#ifndef FWCTLD_AUTO_H
#define FWCTLD_AUTO_H
int fwctld_auto_prepare(const char *directory, const char *device,
                       const char *hwid, int online);
void fwctld_auto_close(void);
void fwctld_auto_report_state_problem(const char *server, const char *device,
                                     const char *hwid);
/* Reconcile pending boot health before comparing or installing an image. */
int fwctld_auto_reconcile(const char *server, const char *device, const char *hwid);
int fwctld_report_problem(const char *server, const char *device, const char *hwid, unsigned long long publication, const char *phase, const char *reason);
int fwctld_release_label(const char *server, unsigned long long publication, char label[128]);
int fwctld_release_identity(const char *server, unsigned long long publication,
			   char tag[32], char hash[65]);
int fwctld_retry_blocked(const char *server, const char *device, unsigned long long publication);
int fwctld_image_matches(const char *server, unsigned long long publication);
int fwctld_auto_install(const char *server, const char *cache, const char *device,
                     const char *hwid);
#endif
