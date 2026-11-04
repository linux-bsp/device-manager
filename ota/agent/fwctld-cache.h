#ifndef FWCTLD_CACHE_H
#define FWCTLD_CACHE_H
/* Caller owns the HWID cache lock and permits cleanup only when OTA is idle.
 * Retains newest completed packages plus current; never touches partials. */
int fwctld_cache_prune(const char *directory, const char *hwid, unsigned keep,
                       unsigned *removed, unsigned long long *bytes);
#endif
