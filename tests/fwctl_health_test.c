#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>
static char profile_path[256];
#define FWCTL_HEALTH_PROFILE profile_path
#include "../fwctl/src/fwctl_health.c"

static int link_absent;
int __real_ioctl(int fd, unsigned long request, ...);
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list args; va_start(args, request); void *arg = va_arg(args, void *); va_end(args);
    if (link_absent && request == SIOCGIFFLAGS) {
        ((struct ifreq *)arg)->ifr_flags = IFF_UP; /* no RUNNING / carrier */
        return 0;
    }
    return __real_ioctl(fd, request, arg);
}

static int ipc_state = OSA_IPC_HEALTH_READY;
static int ipc_unavailable;
static int optional_state = OSA_IPC_HEALTH_FAILED;
static int omit_critical;
static int bad_state;
static unsigned required_flags = OSA_IPC_HEALTH_FLAG_REQUIRED;

int osa_ipc_connect(const char *path, int timeout_ms)
{
    (void)path; (void)timeout_ms;
    return ipc_unavailable ? -EAGAIN : open("/dev/null", O_RDONLY);
}

int osa_ipc_send(int fd, enum osa_ipc_message_type type,
                enum osa_ipc_service_id service, uint16_t flags,
                uint32_t request, uint64_t instance, int32_t status,
                const void *payload, size_t length)
{
    (void)fd; (void)type; (void)service; (void)flags; (void)request;
    (void)instance; (void)status; (void)payload; (void)length;
    return 0;
}

int osa_ipc_receive(int fd, struct osa_ipc_message *message, int timeout_ms)
{
    (void)fd; (void)timeout_ms;
    memset(message, 0, sizeof(*message));
    struct osa_ipc_health_status status = {
        .service_id = OSA_IPC_SERVICE_PSATELLITED,
        .flags = required_flags,
        .state = bad_state ? 99 : ipc_state,
    };
    message->header.message_type = OSA_IPC_MESSAGE_HEALTH_SNAPSHOT;
    message->header.service_id = OSA_IPC_SERVICE_SYSTEM_MANAGER;
    message->header.payload_length = omit_critical ? 0 : sizeof(status);
    memcpy(message->payload, &status, sizeof(status));
    status.service_id = OSA_IPC_SERVICE_PTELEMETRYD;
    status.flags = 0; status.state = optional_state;
    memcpy(message->payload + message->header.payload_length, &status, sizeof(status));
    message->header.payload_length += sizeof(status);
    return 0;
}

int main(void)
{
    assert(health_check_pid("test", "/nonexistent/fwctl-health-pid") == -EAGAIN);
    assert(!health_check_manager(NULL));
    optional_state = OSA_IPC_HEALTH_EXPIRED;
    assert(!health_check_manager(NULL));
    omit_critical = 1; assert(health_check_manager(NULL) == -EIO); omit_critical = 0;
    bad_state = 1; assert(health_check_manager(NULL) == -EIO); bad_state = 0;
    ipc_state = OSA_IPC_HEALTH_PENDING;
    assert(health_check_manager(NULL) == -EAGAIN);
    ipc_state = OSA_IPC_HEALTH_RESTARTING;
    assert(health_check_manager(NULL) == -EAGAIN);
    ipc_state = OSA_IPC_HEALTH_EXPIRED;
    assert(health_check_manager(NULL) == -EIO);
    ipc_state = OSA_IPC_HEALTH_FAILED;
    assert(health_check_manager(NULL) == -EIO);
    required_flags = 0;
    assert(!health_check_manager(NULL)); /* CAN demo is optional in Ethernet product. */
    ipc_unavailable = 1;
    assert(health_check_manager(NULL) == -EAGAIN);
    char directory[] = "/tmp/fw-health-profile-XXXXXX";
    assert(mkdtemp(directory));
    snprintf(profile_path, sizeof(profile_path), "%s/profile", directory);
    assert(health_check_profile(NULL) == -EIO);
    FILE *file = fopen(profile_path, "w"); assert(file);
    fputs("# local stack only\nrequired-netdev=lo\n", file); fclose(file);
    assert(!health_check_profile(NULL));
    link_absent = 1; assert(!health_check_netdev("eth0")); link_absent = 0;
    assert(health_check_netdev("no-such-netdev") == -EAGAIN);
    file = fopen(profile_path, "w"); assert(file); fputs("unknown=lo\n", file); fclose(file);
    assert(health_check_profile(NULL) == -EIO);
    file = fopen(profile_path, "w"); assert(file); fclose(file);
    assert(health_check_profile(NULL) == -EINVAL);
    unlink(profile_path); rmdir(directory);
    puts("PASS optional peripheral health and mandatory local network profile");
    return 0;
}
