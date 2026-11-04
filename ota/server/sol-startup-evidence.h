#ifndef SOL_STARTUP_EVIDENCE_H
#define SOL_STARTUP_EVIDENCE_H
#include <sqlite3.h>
#include <stdio.h>
struct ota_report;
int sol_startup_evidence_schema(sqlite3 *db);
/* Called inside the existing archive/event transaction. */
int sol_startup_evidence_commit(sqlite3 *db,const char *device,const char *capture,const char *manifest);
int sol_startup_evidence_link(sqlite3 *db,const struct ota_report *report);
/* Backfill an existing incident on its first metadata read. */
int sol_startup_evidence_reconcile(sqlite3 *db,const struct ota_report *report);
int sol_startup_evidence_summary(sqlite3 *db,const struct ota_report *report,long long *bytes);
int sol_startup_evidence_metadata(sqlite3 *db,const struct ota_report *report,FILE *out);
int sol_startup_evidence_chunks(sqlite3 *db,const struct ota_report *report,const char *capture,unsigned long long cursor,char **out);
#endif
