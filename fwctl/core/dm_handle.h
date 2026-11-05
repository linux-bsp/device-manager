// SPDX-License-Identifier: MIT
#ifndef DM_HANDLE_H
#define DM_HANDLE_H
#include <stdint.h>
#include <stdbool.h>
#define DM_HANDLE_FW  0x40000000U
#define DM_HANDLE_GENERATION_MAX 0x1fffffU
/* Retire exhausted slots rather than ever accepting a stale token. */
static inline int32_t dm_handle_allocate(uint32_t domain, uint32_t slot,
                                          uint32_t *generation)
{
    if (*generation == DM_HANDLE_GENERATION_MAX)
        return 0;
    ++*generation;
    return (int32_t)(domain | (*generation << 8) | (slot + 1U));
}
static inline bool dm_handle_matches(int32_t token, uint32_t domain,
                                      uint32_t slot, uint32_t generation)
{
    return token > 0 && generation && (uint32_t)token ==
        (domain | (generation << 8) | (slot + 1U));
}
static inline uint32_t dm_handle_slot(int32_t token)
{
    return ((uint32_t)token & 0xffU) - 1U;
}
#endif
