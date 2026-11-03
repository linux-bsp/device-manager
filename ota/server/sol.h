#ifndef DEVICE_SOL_H
#define DEVICE_SOL_H
#include <stdbool.h>
#include <stddef.h>
/* Return HTTP status and an allocated JSON/text response. No shell execution. */
int sol_request(const char *root, const char *method, const char *path,
                const char *authorization, const char *peer, const char *body,
                size_t length, char **response);
struct ota_report;
int sol_ota_event(const char *root, const struct ota_report *report, const char *observed_boot);
int sol_enroll(int argc, char **argv);
void sol_set_open_view(bool enabled);
void sol_set_debug_no_auth(bool enabled);
#endif
