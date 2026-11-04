/* SPDX-License-Identifier: GPL-2.0+ */
#include "dm/hwid.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

int fwctl_run_hwid(int argc, char **argv)
{
    const char *config = DM_HWID_DEFAULT_CONFIG;
    if (argc >= 2 && !strcmp(argv[0], "--config")) { config = argv[1]; argc -= 2; argv += 2; }
    struct dm_hwid_status status; int rc;
    if (!argc || (argc == 1 && (!strcmp(argv[0], "show") || !strcmp(argv[0], "value"))))
        rc = dm_hwid_read(config, &status);
    else if (argc == 2 && !strcmp(argv[0], "set"))
        rc = dm_hwid_set(config, argv[1], &status);
    else return -EINVAL;
    if (rc) return rc;
    if (argc == 1 && !strcmp(argv[0], "value")) printf("%s\n", status.text);
    else {
        unsigned char raw[8]; dm_hwid_parse(status.text, raw);
        unsigned int pcb = ((raw[5] & 15U) << 4) | (raw[6] >> 4);
        printf("[fwctl] [INFO] hwid=%s pcb=v%u.%u source=uboot-env\n",
               status.text, pcb >> 4, pcb & 15U);
    }
    return 0;
}
