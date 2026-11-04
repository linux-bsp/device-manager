#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dm-ioctl.h>
#include <linux/watchdog.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

#define DT "/sys/firmware/devicetree/base/chosen/fw-verity"
#define ROOT_LIMIT (128ULL * 1024 * 1024)
struct identity {
    unsigned blocks, start, tree, bytes;
    char root[65], salt[65], payload[65], firmware_id[65];
};
struct settings {
    char server[256], hwid[17], interface[32], root[2][128];
};
struct alternate_rootfs {
    unsigned deployment, slot, version;
    uint64_t bytes;
    char firmware_id[65], payload[65];
};
static int watchdog_fd = -1;
static atomic_int watchdog_stop;
/* /run moves across switch_root. Keep the fd open until exec so the final
 * handoff messages are retained even after /run moved to /newroot/run. */
static int boot_log_fd = -1;
static int pmsg_fd = -1;
static void log_bytes(const char *s, size_t n)
{
    if (pmsg_fd >= 0) {
        ssize_t written = write(pmsg_fd, s, n);
        if (written < 0) { close(pmsg_fd); pmsg_fd = -1; }
    }
    if (boot_log_fd < 0) return;
    struct stat st;
    if (!fstat(boot_log_fd, &st) && st.st_size < 256 * 1024) {
        while (n) {
            ssize_t written = write(boot_log_fd, s, n);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) break;
            s += written; n -= (size_t)written;
        }
    }
}
static void log_message(const char *s)
{
    char line[1024];
    int len = snprintf(line, sizeof(line), "[fwctl] [INFO] %s\n", s);

    if (len < 0 || len >= (int)sizeof(line)) return;
    /* One console write keeps kernel printk from splitting a status line. */
    size_t offset = 0;
    while (offset < (size_t)len) {
        ssize_t written = write(STDOUT_FILENO, line + offset,
                                (size_t)len - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) break;
        offset += (size_t)written;
    }
    log_bytes(line, (size_t)len);
}

static int property(const char *dir, const char *name, void *data, size_t size)
{
    char path[512];
    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path)) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, data, size);
    char extra;
    int rc = n == (ssize_t)size && read(fd, &extra, 1) == 0 ? 0 : -1;
    close(fd); return rc;
}
static int number(const char *dir, const char *name, unsigned *value)
{
    uint32_t raw;
    if (property(dir, name, &raw, sizeof(raw))) return -1;
    *value = ntohl(raw); return 0;
}
static int digest(const char *dir, const char *name, char value[65])
{
    if (property(dir, name, value, 65) || value[64]) return -1;
    return strspn(value, "0123456789abcdef") == 64 ? 0 : -1;
}
static int identity_read(const char *dir, struct identity *id)
{
    memset(id, 0, sizeof(*id));
    unsigned version; char algorithm[7];
    if (number(dir, "version", &version) || version != 1 ||
        property(dir, "algorithm", algorithm, sizeof(algorithm)) || memcmp(algorithm, "sha256", 7) ||
        number(dir, "data-blocks", &id->blocks) || !id->blocks ||
        number(dir, "hash-start-block", &id->start) || id->start != id->blocks ||
        number(dir, "hash-blocks", &id->tree) || !id->tree ||
        number(dir, "payload-bytes", &id->bytes) || id->bytes > ROOT_LIMIT ||
        digest(dir, "root-hash", id->root) || digest(dir, "salt", id->salt) ||
        digest(dir, "payload-sha256", id->payload)) return -1;
    uint64_t level = id->blocks, tree = 0;
    while (level > 1) { level = (level + 127) / 128; tree += level; }
    return tree == id->tree && ((uint64_t)id->start + id->tree) * 4096 == id->bytes ? 0 : -1;
}
static int firmware_id_read(char id[65])
{
    if (property("/sys/firmware/devicetree/base/chosen", "fw,id", id, 65) || id[64])
        return -1;
    return strspn(id, "0123456789abcdef") == 64 ? 0 : -1;
}
static int check_root(const char *device, const struct identity *id)
{
    unsigned char buffer[65536], hash[32]; char hex[65]; unsigned length;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    int fd = open(device, O_RDONLY | O_CLOEXEC), rc = -1;
    if (!ctx || fd < 0 || !EVP_DigestInit_ex(ctx, EVP_sha256(), NULL)) goto out;
    uint64_t left = id->bytes;
    while (left) {
        size_t want = left > sizeof(buffer) ? sizeof(buffer) : (size_t)left;
        ssize_t n = read(fd, buffer, want);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || !EVP_DigestUpdate(ctx, buffer, (size_t)n)) goto out;
        left -= n;
    }
    if (!EVP_DigestFinal_ex(ctx, hash, &length) || length != 32) goto out;
    for (unsigned i = 0; i < 32; i++) snprintf(hex + i*2, 3, "%02x", hash[i]);
    rc = strcmp(hex, id->payload) ? -1 : 0;
out:
    if (fd >= 0) close(fd);
    EVP_MD_CTX_free(ctx); return rc;
}
static int device_name(const char *name)
{
    return !strncmp(name, "/dev/", 5) && name[5] &&
        strspn(name + 5, "abcdefghijklmnopqrstuvwxyz0123456789-_") == strlen(name + 5);
}
static int verity_table(const struct identity *id, const char *device, char *out, size_t capacity)
{
    if (!device_name(device)) return -1;
    int n = snprintf(out, capacity, "1 %s %s 4096 4096 %u %u sha256 %s %s 2 restart_on_corruption restart_on_error",
        device, device, id->blocks, id->start, id->root, id->salt);
    return n > 0 && (size_t)n < capacity ? 0 : -1;
}
static void dm_header(struct dm_ioctl *io, size_t size)
{
    memset(io, 0, size);
    io->version[0] = DM_VERSION_MAJOR;
    io->version[1] = DM_VERSION_MINOR;
    io->version[2] = DM_VERSION_PATCHLEVEL;
    io->data_size = size; io->data_start = sizeof(*io);
    strcpy(io->name, "fw-root");
}
static int dm_create(const struct identity *id, const char *device)
{
    unsigned char buffer[2048] __attribute__((aligned(8)));
    struct dm_ioctl *io = (void *)buffer;
    int fd = open("/dev/mapper/control", O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    dm_header(io, sizeof(buffer)); io->flags = DM_READONLY_FLAG;
    if (ioctl(fd, DM_DEV_CREATE, io)) { close(fd); return -1; }
    dev_t dev = makedev((io->dev >> 8) & 0xfff, (io->dev & 0xff) | ((io->dev >> 12) & 0xfff00));
    dm_header(io, sizeof(buffer)); io->flags = DM_READONLY_FLAG; io->target_count = 1;
    struct dm_target_spec *target = (void *)(buffer + io->data_start);
    target->sector_start = 0; target->length = (uint64_t)id->blocks * 8;
    strcpy(target->target_type, "verity");
    char *params = (char *)(target + 1);
    if (verity_table(id, device, params, sizeof(buffer) - (params - (char *)buffer)) ||
        ioctl(fd, DM_TABLE_LOAD, io)) goto fail;
    dm_header(io, sizeof(buffer)); io->flags = DM_READONLY_FLAG;
    if (ioctl(fd, DM_DEV_SUSPEND, io)) goto fail;
    unlink("/dev/fw-root");
    if (mknod("/dev/fw-root", S_IFBLK | 0600, dev)) goto fail;
    close(fd); return 0;
fail:
    dm_header(io, sizeof(buffer)); (void)ioctl(fd, DM_DEV_REMOVE, io);
    close(fd); return -1;
}
static void dm_remove(void)
{
    struct dm_ioctl io; int fd = open("/dev/mapper/control", O_RDWR | O_CLOEXEC);
    if (fd >= 0) { dm_header(&io, sizeof(io)); (void)ioctl(fd, DM_DEV_REMOVE, &io); close(fd); }
    unlink("/dev/fw-root");
}
static void *feed_watchdog(void *unused)
{
    (void)unused;
    while (!atomic_load(&watchdog_stop)) {
        if (ioctl(watchdog_fd, WDIOC_KEEPALIVE, 0)) log_message("watchdog keepalive failed");
        for (unsigned i = 0; i < 5 && !atomic_load(&watchdog_stop); i++)
            sleep(1);
    }
    return NULL;
}
static int run(char *const args[], char *output, size_t capacity)
{
    int fds[2] = {-1,-1};
    if (pipe2(fds, O_CLOEXEC)) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (!pid) {
        dup2(fds[1], STDOUT_FILENO);
        if (!output) dup2(fds[1], STDERR_FILENO);
        else if (boot_log_fd >= 0) dup2(boot_log_fd, STDERR_FILENO);
        close(fds[0]); close(fds[1]);
        execv(args[0], args); _exit(127);
    }
    {
        close(fds[1]); size_t used = 0; int overflow = 0; char chunk[512]; ssize_t n;
        while ((n = read(fds[0], chunk, sizeof(chunk))) != 0) {
            if (n < 0) { if (errno == EINTR) continue; overflow = 1; break; }
            if (output) {
                if ((size_t)n >= capacity - used) overflow = 1;
                if (!overflow) { memcpy(output + used, chunk, n); used += n; }
            } else {
                log_bytes(chunk, (size_t)n);
                (void)fwrite(chunk, 1, (size_t)n, stdout); fflush(stdout);
            }
        }
        if (output) output[used] = 0;
        close(fds[0]);
        if (overflow) { int status; while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {} return -1; }
    }
    int status;
    while (waitpid(pid, &status, 0) < 0) { if (errno != EINTR) return -1; }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}
/* devtmpfs is mounted before MMC probing has necessarily completed. */
static int read_boot_status(const struct settings *s, unsigned *booted,
                            unsigned *slot, unsigned *version)
{
    char status[128], extra;
    char *args[] = {"/usr/sbin/fwctl", "recovery", "status", NULL};

    if (access(s->root[0], R_OK) && access(s->root[1], R_OK))
        return -EAGAIN;
    if (run(args, status, sizeof(status)))
        return -EIO;
    if (sscanf(status, "FWRECOVERY1 %u %u %u %c", booted, slot, version, &extra) != 3 ||
        *booted > 1 || *slot > 1)
        return -EINVAL;
    if (access(s->root[*slot], R_OK))
        return errno == ENOENT ? -EAGAIN : -errno;
    return 0;
}

static int settings_read(struct settings *s)
{
    memset(s, 0, sizeof(*s));
    FILE *file = fopen("/etc/fw-recovery.conf", "r"); char line[512];
    if (!file) return -1;
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        char *value = strchr(line, '='); if (!value) { fclose(file); return -1; }
        *value++ = 0;
        char *out = NULL; size_t size = 0;
#define SETTING(k, member) if (!strcmp(line, k)) { out=s->member; size=sizeof(s->member); }
        SETTING("server", server)
        else SETTING("interface", interface) else SETTING("root-a", root[0]) else SETTING("root-b", root[1])
#undef SETTING
        if (!out || strlen(value) >= size || out[0]) { fclose(file); return -1; }
        strcpy(out, value);
    }
    int rc = ferror(file); fclose(file);
    if (rc || !device_name(s->root[0]) || !device_name(s->root[1]) || !s->interface[0] ||
        strspn(s->interface, "abcdefghijklmnopqrstuvwxyz0123456789-_") != strlen(s->interface)) return -1;
    return strncmp(s->server,"http://",7) && strncmp(s->server,"https://",8) ? -1 : 0;
}
static void reboot_board(void)
{
    sync(); reboot(RB_AUTOBOOT);
    log_message("reboot syscall failed; staying in recovery");
    for (;;) sleep(30);
}
static void try_fallback(void)
{
    char *args[]={"/usr/sbin/fwctl","recovery","fallback",NULL};
    if (!run(args,NULL,0)) {
        log_message("selected alternate confirmed deployment; rebooting its kernel/rootfs pair");
        reboot_board();
    }
}
static int alternate_rootfs_identity(unsigned deployment, struct alternate_rootfs *id)
{
    char text[256], extra;
    char number[8];
    unsigned long long bytes;
    snprintf(number, sizeof(number), "%u", deployment);
    char *args[] = {"/usr/sbin/fwctl", "recovery", "info-slot", number, NULL};
    if (run(args, text, sizeof(text)) ||
        sscanf(text, "FWROOTFS1 %u %u %u %64s %llu %64s %c",
            &id->deployment, &id->slot, &id->version, id->firmware_id,
            &bytes, id->payload, &extra) != 6 ||
        id->deployment != deployment || id->slot > 1 || !id->version || !bytes ||
        strlen(id->firmware_id) != 64 || strspn(id->firmware_id, "0123456789abcdef") != 64 ||
        strlen(id->payload) != 64 || strspn(id->payload, "0123456789abcdef") != 64)
        return -1;
    id->bytes = bytes;
    return 0;
}

/* Best-effort repair the other, unmounted rootfs using its own metadata identity.
 * This runs only after the selected rootfs is verified, so a network/server
 * failure cannot prevent a valid deployment from booting. */
static void repair_alternate_rootfs(const struct settings *settings,
                                    unsigned deployment, unsigned current_slot,
                                    int *network)
{
    struct alternate_rootfs id = { .deployment = deployment };
    if (alternate_rootfs_identity(deployment, &id)) {
        log_message("alternate rootfs has no complete version+hash identity; skipping repair");
        return;
    }
    if (id.slot == current_slot) {
        log_message("alternate deployment references the currently selected rootfs slot; refusing write");
        return;
    }
    char number[8], result[64];
    snprintf(number, sizeof(number), "%u", deployment);
    char *check[] = {"/usr/sbin/fwctl", "recovery", "check-slot", number, NULL};
    if (run(check, result, sizeof(result))) {
        log_message("cannot verify alternate rootfs; continuing with selected verified rootfs");
        return;
    }
    if (!strcmp(result, "FWROOTFSCHECK1 match\n")) {
        log_message("alternate rootfs SHA256 verified; no repair needed");
        return;
    }
    if (strcmp(result, "FWROOTFSCHECK1 different\n")) {
        log_message("alternate rootfs check returned an invalid result; skipping repair");
        return;
    }
    log_message("alternate rootfs is damaged; attempting exact network recovery");
    if (!*network) {
        char interface[sizeof(settings->interface)];
        strcpy(interface, settings->interface);
        char *up[] = {"/sbin/ifup", interface, NULL};
        if (run(up, NULL, 0)) {
            log_message("network unavailable; booting selected verified rootfs and leaving alternate damaged");
            return;
        }
        *network = 1;
    }
    char url[768], bytes[32], limit[32];
    snprintf(url, sizeof(url), "%s/recovery/%s/rootfs/%u/%s/%s/rootfs.itb",
        settings->server, settings->hwid,
        id.version, id.firmware_id, id.payload);
    snprintf(bytes, sizeof(bytes), "%llu", (unsigned long long)id.bytes);
    snprintf(limit, sizeof(limit), "%llu", (unsigned long long)id.bytes + 1024 * 1024);
    unlink("/tmp/recovery-alternate-rootfs.itb");
    char *get[] = {"/usr/bin/curl", "--disable", "--fail", "--silent", "--show-error",
        "--noproxy", "*", "--proto", "=http,https", "--connect-timeout", "10",
        "--max-time", "900", "--speed-limit", "1024", "--speed-time", "30",
        "--max-filesize", limit, "--output", "/tmp/recovery-alternate-rootfs.itb",
        "--url", url, NULL};
    if (run(get, NULL, 0)) {
        log_message("alternate rootfs download failed; selected verified rootfs will still boot");
        unlink("/tmp/recovery-alternate-rootfs.itb");
        return;
    }
    char *restore[] = {"/usr/sbin/fwctl", "recovery", "restore-slot", number,
        "/tmp/recovery-alternate-rootfs.itb", bytes, id.payload, NULL};
    if (!run(restore, NULL, 0))
        log_message("alternate rootfs restored and readback verified; deployment remains failed until health policy confirms it");
    else
        log_message("alternate rootfs repair failed; selected verified rootfs will still boot");
    unlink("/tmp/recovery-alternate-rootfs.itb");
}
static void halt_recovery(const char *why)
{
    fprintf(stderr, "[fwctl] [ERROR] %s\n", why);
    log_message("automatic boot stopped; serial recovery shell available (exit to restart shell)");
    for (;;) {
        char *shell[]={"/bin/sh",NULL};
        (void)run(shell,NULL,0);
        sleep(5);
    }
}
static int handoff(pthread_t thread, const char *rootfs_message)
{
    /* The real root owns /run contents; leave a marker without claiming health. */
    FILE *file = fopen("/run/fw-init.verified", "w");
    if (!file) return -1;
    fputs("rootfs payload and dm-verity verified\n", file); fclose(file);
    const char *mounts[] = {"/run", "/dev", "/sys", "/proc"};
    for (unsigned i=0; i<4; i++) {
        char dest[64]; snprintf(dest, sizeof(dest), "/newroot%s", mounts[i]);
        if (mount(mounts[i], dest, NULL, MS_MOVE, NULL)) return -1;
    }
    /* nowayout keeps the watchdog armed. Drain its close diagnostics before
     * printing the verified rootfs result on the shared serial console. */
    atomic_store(&watchdog_stop, 1);
    if (pthread_join(thread, NULL)) return -1;
    if (close(watchdog_fd)) return -1;
    watchdog_fd = -1;
    log_message(rootfs_message);
    log_message("rootfs verification complete; boot health check still pending");
    log_message("watchdog remains armed; system_manager will resume servicing after switch_root");
    log_message("switch_root: starting verified rootfs /sbin/init");
    char *args[] = {"/bin/busybox", "switch_root", "-c", "/dev/console", "/newroot", "/sbin/init", NULL};
    execv(args[0], args); return -1;
}
int fwctl_recovery_main(int argc, char **argv)
{
    struct identity id;
    /* Safe host/target diagnostics: reads only, never creates a mapping. */
    if (argc == 3 && (!strcmp(argv[0], "check-root") || !strcmp(argv[0], "table"))) {
        if (identity_read(argv[2], &id)) return 2;
        if (!strcmp(argv[0], "check-root")) return check_root(argv[1], &id) ? 1 : 0;
        char table[768]; if (verity_table(&id, argv[1], table, sizeof(table))) return 2;
        puts(table); return 0;
    }
    if (argc != 0 || getpid() != 1) { fprintf(stderr,"fwctl init must run as initramfs PID 1\n"); return 2; }
    setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
    umask(022); setvbuf(stdout, NULL, _IONBF, 0);
    if (mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, NULL) ||
        mount("proc", "/proc", "proc", MS_NOSUID|MS_NOEXEC|MS_NODEV, NULL) ||
        mount("sysfs", "/sys", "sysfs", MS_NOSUID|MS_NOEXEC|MS_NODEV, NULL) ||
        mount("tmpfs", "/run", "tmpfs", MS_NOSUID|MS_NODEV, "mode=0755"))
        halt_recovery("cannot mount recovery runtime filesystems");
    int console = open("/dev/console", O_RDWR);
    if (console >= 0) { dup2(console,0); dup2(console,1); dup2(console,2); if (console>2) close(console); }
    pmsg_fd = open("/dev/pmsg0", O_WRONLY|O_CLOEXEC);
    boot_log_fd = open("/run/logctl-initramfs.log", O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC, 0600);
    log_message("independent initramfs recovery environment started");
    mkdir("/run/network",0755); mkdir("/run/lock",0755);
    watchdog_fd = open("/dev/watchdog0", O_WRONLY|O_CLOEXEC);
    pthread_t thread; int timeout = 60;
    if (watchdog_fd < 0) halt_recovery("watchdog unavailable; refusing unattended repair");
    (void)ioctl(watchdog_fd, WDIOC_SETTIMEOUT, &timeout);
    if (pthread_create(&thread,NULL,feed_watchdog,NULL)) halt_recovery("cannot start watchdog servicing");
    struct settings s;
    if (settings_read(&s) || identity_read(DT,&id)) halt_recovery("invalid recovery configuration or verified DT identity");
    unsigned booted, slot, version;
    int ready = 0;
    for (unsigned i=0; i<30; i++) {
        int status_ret = read_boot_status(&s, &booted, &slot, &version);
        if (!status_ret) { ready=1; break; }
        if (!i && status_ret == -EAGAIN)
            log_message("waiting for root storage device discovery");
        sleep(1);
    }
    if (!ready) halt_recovery("root device or trusted A/B metadata unavailable; no writes attempted");
    char hardware[128];
    char *hardware_command[] = {"/usr/sbin/fwctl", "hwid", "value", NULL};
    if (run(hardware_command, hardware, sizeof(hardware)))
        halt_recovery("HWID storage missing, corrupt or inconsistent; no recovery writes attempted");
    hardware[strcspn(hardware, "\r\n")] = 0;
    if (strlen(hardware) != 16) halt_recovery("invalid hardware identity");
    memcpy(s.hwid, hardware, 17);
    char hardware_message[192];
    snprintf(hardware_message, sizeof(hardware_message), "stage=initramfs hwid=%s", hardware);
    log_message(hardware_message);
    char identity_text[256];
    char *identity_command[] = {"/usr/sbin/fwctl", "identity", "initramfs", NULL};
    if (!run(identity_command, identity_text, sizeof(identity_text))) {
        identity_text[strcspn(identity_text, "\r\n")] = 0;
        puts(identity_text); fflush(stdout);
        log_bytes(identity_text, strlen(identity_text)); log_bytes("\n", 1);
    }
    else log_message("firmware identity unavailable or inconsistent; inspect boot metadata");
    int fallback_attempted=0, network=0, alternate_checked=0;
    for (;;) {
        log_message("checking all rootfs data and hash-tree bytes against kernel-bound SHA256");
        if (!check_root(s.root[slot],&id)) {
            if (!dm_create(&id,s.root[slot])) {
                if (!mount("/dev/fw-root","/newroot","squashfs",MS_RDONLY,NULL)) {
                    if (access("/newroot/sbin/init",X_OK)) {
                        umount("/newroot"); dm_remove();
                        try_fallback();
                        halt_recovery("verified image lacks /sbin/init; content is not a bootable rootfs");
                    }
                    if (!alternate_checked) {
                        alternate_checked = 1;
                        repair_alternate_rootfs(&s, 1U - booted, slot, &network);
                    }
                    char message[192];
                    snprintf(message, sizeof(message),
                        "stage=initramfs rootfs SHA256 verified: "
                        "slot=%c version=v%u.%u.%u payload_sha256=%.8s "
                        "dm_verity=active",
                        'A' + slot, version >> 16,
                        (version >> 8) & 255, version & 255,
                        id.payload);
                    if (network) { char *down[]={"/sbin/ifdown",s.interface,NULL}; (void)run(down,NULL,0); }
                    if (handoff(thread, message)) halt_recovery("switch_root failed; rootfs was not marked good");
                }
                dm_remove();
            }
            try_fallback();
            halt_recovery("verified payload cannot be mapped/mounted; refusing pointless reflashing");
        }
        log_message("rootfs damaged or unreadable; remaining in initramfs");
        if (!fallback_attempted) {
            fallback_attempted=1;
            try_fallback();
        }
        if (!network) {
            char *up[]={"/sbin/ifup",s.interface,NULL};
            if (run(up,NULL,0)) { log_message("network not ready; retrying in 30 seconds"); sleep(30); continue; }
            network=1;
        }
        if (!id.firmware_id[0] && firmware_id_read(id.firmware_id))
            halt_recovery("complete version+hash identity unavailable; refusing ambiguous network recovery");
        char url[640], size[32], limit[32];
        snprintf(url,sizeof(url),"%s/recovery/%s/rootfs/%u/%s/%s/rootfs.itb",
            s.server,s.hwid,version,id.firmware_id,id.payload);
        snprintf(size,sizeof(size),"%u",id.bytes);
        snprintf(limit,sizeof(limit),"%llu",(unsigned long long)id.bytes + 1024*1024);
        unlink("/tmp/recovery-rootfs.itb");
        log_message("requesting exact rootfs payload from OTA server");
        char *get[]={"/usr/bin/curl","--disable","--fail","--show-error","--noproxy","*",
            "--proto","=http,https","--connect-timeout","10","--max-time","900",
            "--speed-limit","1024","--speed-time","30","--max-filesize",limit,
            "--output","/tmp/recovery-rootfs.itb","--url",url,NULL};
        if (!run(get,NULL,0)) {
            char *restore[]={"/usr/sbin/fwctl","recovery","restore","/tmp/recovery-rootfs.itb",size,id.payload,NULL};
            log_message("validating package and repairing unmounted rootfs; watchdog servicing remains active");
            if (!run(restore,NULL,0)) {
                unlink("/tmp/recovery-rootfs.itb");
                log_message("rootfs restored and readback verified; rechecking before switch_root");
                continue;
            }
        }
        unlink("/tmp/recovery-rootfs.itb");
        log_message("exact recovery failed/unavailable; no boot into damaged rootfs; retrying in 30 seconds");
        sleep(30);
    }
}
