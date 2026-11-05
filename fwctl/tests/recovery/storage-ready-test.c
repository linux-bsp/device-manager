/* SPDX-License-Identifier: GPL-2.0+ */
#include "../../recovery/fw-init.c"
#include <assert.h>

static int ready_mask, fork_calls, child_error;
static const char *reply = "FWRECOVERY1 0 0 65550\n";

int __wrap_access(const char *path, int mode)
{
    assert(mode == R_OK);
    int mask = !strcmp(path, "/dev/testa") ? 1 : 2;
    assert(mask == 1 || !strcmp(path, "/dev/testb"));
    if (ready_mask & mask)
        return 0;
    errno = ENOENT;
    return -1;
}
pid_t __real_fork(void);
pid_t __wrap_fork(void)
{
    fork_calls++;
    return __real_fork();
}
int __wrap_execv(const char *path, char *const args[])
{
    assert(!strcmp(path, "/usr/sbin/fwctl"));
    assert(!strcmp(args[1], "recovery"));
    assert(!strcmp(args[2], "status"));
    if (!child_error)
        assert(write(STDOUT_FILENO, reply, strlen(reply)) == (ssize_t)strlen(reply));
    _exit(child_error);
}
int main(void)
{
    struct settings settings = {.root = {"/dev/testa", "/dev/testb"}};
    unsigned booted, slot, version;
    assert(read_boot_status(&settings, &booted, &slot, &version) == -EAGAIN);
    assert(fork_calls == 0); /* No query or misleading stderr before device discovery. */
    ready_mask = 1;
    assert(!read_boot_status(&settings, &booted, &slot, &version));
    assert(fork_calls == 1 && booted == 0 && slot == 0 && version == 65550);
    reply = "FWRECOVERY1 1 1 65550\n";
    assert(read_boot_status(&settings, &booted, &slot, &version) == -EAGAIN);
    ready_mask = 3;
    assert(!read_boot_status(&settings, &booted, &slot, &version));
    assert(booted == 1 && slot == 1);
    reply = "FWRECOVERY1 0 2 65550\n";
    assert(read_boot_status(&settings, &booted, &slot, &version) == -EINVAL);
    child_error = 1;
    assert(read_boot_status(&settings, &booted, &slot, &version) == -EIO);
    puts("PASS: delayed devices skip fwctl; selected device checked; actual failures preserved");
    return 0;
}
