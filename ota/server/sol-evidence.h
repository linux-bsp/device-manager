#ifndef SOL_EVIDENCE_H
#define SOL_EVIDENCE_H
#include <sqlite3.h>
struct ota_report;
int sol_evidence_schema(sqlite3 *db);
int sol_evidence_event(sqlite3 *db, const struct ota_report *report, const char *observed_boot);
int sol_evidence_capture(sqlite3 *db, const char *device, const char *boot);
int sol_evidence_query(sqlite3 *db, const char *path, char **out);
#endif
