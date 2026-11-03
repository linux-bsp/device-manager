#ifndef FWCTLD_DIAGNOSTICS_H
#define FWCTLD_DIAGNOSTICS_H
/* Observations are not proof that a particular firmware caused the fault. */
int fwctld_diagnostic_reason(const char *storage_path,const char *crash_directory,char reason[64]);
void fwctld_observe_failure(char reason[64],int after_reboot);
#endif
