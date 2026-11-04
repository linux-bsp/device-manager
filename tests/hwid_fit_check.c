/* SPDX-License-Identifier: GPL-2.0+ */
/* Read-only FIT compatibility probe for native/target integration tests. */
#include "dm_fw_fit_priv.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    struct fw_layout layout = {0};
    struct fw_fit fit;
    struct fw_release release;
    if (argc != 3 || strlen(argv[1]) >= sizeof(layout.hwid_config)) return 2;
    strcpy(layout.product, "ti-am62x");
    strcpy(layout.layout_id, "am62x-sd-ab-v1");
    strcpy(layout.hwid_config, argv[1]);
    int rc = fw_fit_open(&fit, argv[2], &layout);
    if (!rc) { rc = fw_fit_verify_release(&layout, &fit, &release); fw_fit_close(&fit); }
    printf("%d\n", rc);
    return 0;
}
