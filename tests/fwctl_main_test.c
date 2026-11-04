#define main fwctl_entry
#include "../fwctl/src/fwctl_main.c"
#undef main
#include <assert.h>

static int firmware_calls, recovery_calls;
int fwctl_run_firmware(const struct fwctl_options *o, const char *command, int argc, char **argv)
{
    assert(o->config && !strcmp(command, "factory-init"));
    assert(argc == 2 && (!strcmp(argv[0], "a") || !strcmp(argv[0], "b")) && !strcmp(argv[1], "--force"));
    firmware_calls++; return 0;
}
int fwctl_recovery_main(int argc, char **argv)
{ assert(!argc && !argv); recovery_calls++; return 0; }
int fwctl_run_hwid(int argc, char **argv)
{ (void)argc; (void)argv; assert(0); return 0; }
int fwctl_run_health_checks(const struct fwctl_options *o)
{ (void)o; assert(0); return 0; }

int main(void)
{
    char *help[] = {"fwctl", "--help"};
    char *cap[] = {"fwctl", "capabilities"};
    char *init[] = {"fwctl", "init", "a"};
    char *factory[] = {"fwctl", "factory-init", "a", "--force"};
    char *wrong[] = {"fwctl", "factory-init", "--force", "a"};
    assert(!fwctl_entry(2, help));
    assert(!fwctl_entry(2, cap));
    assert(!firmware_calls && !recovery_calls);
    assert(fwctl_entry(3, init) == 2);
    assert(fwctl_entry(3, factory) == 2);
    assert(fwctl_entry(4, wrong) == 2);
    assert(!firmware_calls && !recovery_calls);
    assert(!fwctl_entry(2, init));
    assert(!firmware_calls && recovery_calls == 1);
    assert(!fwctl_entry(4, factory));
    assert(firmware_calls == 1 && recovery_calls == 1);
    puts("fwctl dispatch: PID 1 init and explicit factory-force boundary passed");
    return 0;
}
