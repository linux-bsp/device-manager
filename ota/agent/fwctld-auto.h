#ifndef FWCTLD_AUTO_H
#define FWCTLD_AUTO_H
/* Reconcile pending boot health before comparing or installing an image. */
int fwctld_auto_reconcile(const char *server, const char *device, const char *product, const char *layout);
int fwctld_report_problem(const char *server, const char *device, const char *product, const char *layout, unsigned long long publication, const char *phase, const char *reason);
int fwctld_release_label(const char *server, unsigned long long publication, char label[128]);
int fwctld_retry_blocked(const char *server, const char *device, unsigned long long publication);
int fwctld_image_matches(const char *server, unsigned long long publication);
int fwctld_auto_install(const char *server, const char *cache, const char *device,
                     const char *product, const char *layout);
#endif
