# Boot evidence and software reboot intent

`logctl` records observations; firmware recovery and watchdog policy remain owned
by fwctl/system_manager. A software reboot request is not proof of the hardware
reset cause. In this first stage `reset_cause` remains `unknown`.

Each Linux boot directory contains:

- `boot-state`: the latest versioned, CRC32-protected state, committed through a
  temporary file, file sync, rename and directory sync.
- `boot-events` and `boot-events.1`: sequence-numbered event history, at most
  64 KiB each. Events also have CRC32. Invalid records and sequence gaps are
  reported when reading; valid surrounding records remain usable.
- `boot-record.lock`: an exclusive nonblocking writer lock. Reader operations
  do not change an archived historical boot.

The state includes the Linux boot ID, phase, health-confirmation flag, requested
reboot intent, monotonic and wall timestamps, U-Boot's observed reset label,
previous **recorded Linux boot**, previous health/intent and integrity status.
There may have been intervening failed SPL/kernel boots without a Linux record;
therefore the previous record is not asserted to be the immediately preceding
hardware reset. The existing early-buffer/pstore archives retain their own IDs.

Wall-clock integers are UTC Unix seconds. Human-readable calendar fields use
UTC+8 explicitly; implausible/unset dates display `unknown`. A plausible timestamp
is not proof that NTP has synchronized it. Monotonic time and event sequence remain
available before calibration. No changes are made to NTP packets or RTC UTC storage.

```sh
logctl boot-state
logctl boot-state --boot previous
logctl boot-events --boot previous
logctl boot-event operator
reboot
```

Normal operation records booting, confirmed health, OTA/health-rejection intent
and shutdown entry. The shutdown hook preserves a more specific existing intent;
a failed reboot command cancels its pending intent. Unspecified shutdown does not
claim whether the user requested reboot, halt or poweroff. Restarting logctld in
one boot preserves the original record rather than creating a new boot.

Evidence failure warns and leaves firmware health/reboot decisions unchanged.
The optional fwctld intent helper has a three-second deadline plus at most 100 ms
for reap attempts. A process stuck in uninterruptible kernel I/O cannot be forced
to exit by userspace; at most one unreaped helper is retained. The authoritative
firmware transaction is committed before calling the optional evidence helper.

Current-state commit precedes journal append. An append/sync failure is reported
although the latest state may already be visible; event history is not claimed
complete after that failure. Corrupt existing state is preserved, never silently
reinitialized. A new boot records an invalid predecessor and continues collecting.
CRC detects accidental damage; it does not authenticate records against an attacker.

Boot records travel in the closed-boot archive introduced in S3b2. SOL metadata
also exposes the current evidence fields in its detail panel. Historical record
retention follows boot-directory quotas; neither the journal nor server storage
is unlimited. Journals do not independently survive loss of the data filesystem.

Next stage: preserve raw AM625 reset bits before U-Boot clears them, correlate
pstore/early-buffer evidence, then validate hardware-cause classification and
cross-reboot recovery budgets. The current U-Boot label alone is insufficient to
separate all watchdog, ESM, panic and power-loss cases.

Tests: `python3 logging/tests/test_boot_record.py`,
`python3 ota/tests/test-boot-health.py`, and `python3 ota/tests/test-auto.py`.
