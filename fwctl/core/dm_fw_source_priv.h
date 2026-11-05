// SPDX-License-Identifier: GPL-2.0+
#ifndef DM_FW_SOURCE_PRIV_H
#define DM_FW_SOURCE_PRIV_H

#include "dm_fw_layout_priv.h"

#include <stddef.h>

struct fw_artifact {
    char path[FW_PATH_SIZE];
    int temporary;
};

DM_FW_INTERNAL int fw_source_build_component(
    const char *release_source, const char *component_file,
    char *result, size_t result_size);
DM_FW_INTERNAL int fw_source_get_release(const struct fw_layout *layout,
                                          const char *source,
                                          struct fw_artifact *artifact);
DM_FW_INTERNAL int fw_source_get_component(const struct fw_layout *layout,
                                            const char *release_source,
                                            const char *component_file,
                                            struct fw_artifact *artifact);
DM_FW_INTERNAL void fw_source_put(struct fw_artifact *artifact);

#endif
