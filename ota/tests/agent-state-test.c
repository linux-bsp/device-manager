#define _POSIX_C_SOURCE 200809L
#include "fwctld-state.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned fail_sync_after;
int __real_fsync(int fd);
int __wrap_fsync(int fd)
{
    if (fail_sync_after && !--fail_sync_after) { errno = EIO; return -1; }
    return __real_fsync(fd);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    struct fwctld_journal journal, locked;
    assert(!fwctld_state_open(&journal, argv[1], "test-dev", "1010001000110000"));
    assert(fwctld_state_open(&locked, argv[1], "test-dev", "1010001000110000"));
    struct fwctld_record record = {0}, loaded;
    assert(fwctld_state_read(&journal, &loaded) == 1);
    strcpy(record.tag, "v0.0.1"); memset(record.identity, 'a', 64);
    strcpy(record.report.device, "test-dev"); strcpy(record.report.hwid, "1010001000110000");
    strcpy(record.report.boot, "origin-boot"); strcpy(record.report.phase, "installing");
    strcpy(record.report.reason, "none");
    record.report.publication = 7; record.report.version = 1;
    record.report.target = 1; record.report.running = 1;
    assert(!fwctld_state_append(&journal, &record)); assert(record.sequence == 1);
    fwctld_state_close(&journal);
    assert(!fwctld_state_open(&journal, argv[1], "test-dev", "1010001000110000"));
    assert(!fwctld_state_read(&journal, &loaded)); assert(loaded.sequence == 1);
    char path[OTA_PATH]; assert(!ota_path(path, journal.directory, "transaction"));
    assert(!unlink(path)); /* Crash after event sync, before snapshot rename. */
    assert(!fwctld_state_read(&journal, &loaded)); assert(loaded.sequence == 1);
    strcpy(record.report.phase, "rebooting");
    assert(!fwctld_state_append(&journal, &record)); assert(record.sequence == 2);
    assert(!fwctld_state_first(&journal, &loaded)); assert(loaded.sequence == 1);
    assert(!fwctld_state_ack(&journal, 1));
    assert(!fwctld_state_first(&journal, &loaded)); assert(loaded.sequence == 2);
    assert(!fwctld_state_ack(&journal, 2));
    assert(fwctld_state_first(&journal, &loaded) == 1);
    assert(!fwctld_state_read(&journal, &loaded)); assert(loaded.sequence == 2);
    int fd = open(path, O_RDWR); assert(fd >= 0);
    off_t end = lseek(fd, -2, SEEK_END); char original;
    assert(read(fd, &original, 1) == 1);
    char corrupt = original == '0' ? '1' : '0';
    assert(pwrite(fd, &corrupt, 1, end) == 1);
    assert(fwctld_state_read(&journal, &loaded) == -1);
    assert(pwrite(fd, &original, 1, end) == 1); close(fd);
    for (unsigned i = 0; i < 64; i++) assert(!fwctld_state_append(&journal, &record));
    assert(fwctld_state_append(&journal, &record) == -1 && errno == ENOSPC);
    while (!fwctld_state_first(&journal, &loaded))
        assert(!fwctld_state_ack(&journal, loaded.sequence));
    fail_sync_after = 1;
    assert(fwctld_state_append(&journal, &record) == -1);
    assert(!fwctld_state_read(&journal, &loaded)); assert(loaded.sequence == 66);
    fail_sync_after = 3; /* Durable event, failed snapshot sync. */
    assert(fwctld_state_append(&journal, &record) == -1);
    assert(!fwctld_state_read(&journal, &loaded)); assert(loaded.sequence == 67);
    assert(!fwctld_state_ack(&journal, 67));
    assert(!fwctld_state_read(&journal, &loaded)); assert(loaded.sequence == 67);
    fwctld_state_close(&journal);
    return 0;
}
