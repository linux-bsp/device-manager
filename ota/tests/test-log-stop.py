#!/usr/bin/env python3
"""Validate stop ordering without signalling any host processes or devices."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[3]/'bsp-base-external/package/device-manager/S01logctld'


class LogStop(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='logctld-stop-')
        self.root = Path(self.tmp.name)
        self.bin = self.root/'bin';self.bin.mkdir()
        self.pid = self.root/'pid';self.pid.write_text('123\n')
        self.state = self.root/'remaining';self.state.write_text('3')
        self.status = self.root/'sync-status'
        self.status.write_text('collector-state=running\nsync-state=ok\n')
        self.calls = self.root/'calls'
        self.env = dict(os.environ, PATH=str(self.bin), STATE=str(self.state),
                        STATUS=str(self.status), CALLS=str(self.calls), FINAL_STATE='stopped', FINAL_SYNC='ok')
        (self.bin/'rm').symlink_to(shutil.which('rm'))
        self.stub('start-stop-daemon', '''
case " $* " in
    *" -t "*) read -r remaining < "$STATE"; [ "$remaining" != 0 ] ;;
    *) echo signal >> "$CALLS" ;;
esac''')
        self.stub('sleep', '''
echo wait >> "$CALLS"
read -r remaining < "$STATE"
if [ "$remaining" -gt 0 ]; then
    remaining=$((remaining - 1))
    printf '%s\\n' "$remaining" > "$STATE"
    if [ "$remaining" = 0 ]; then
        printf 'collector-state=%s\\nsync-state=%s\\n' "$FINAL_STATE" "$FINAL_SYNC" > "$STATUS"
    fi
fi''')
        self.script = self.root/'S01logctld'
        self.script.write_text(SCRIPT.read_text().replace('PIDFILE=/run/logctld.pid', f'PIDFILE={self.pid}')
                               .replace('SYNC_STATUS=/run/logctl/sync-status', f'SYNC_STATUS={self.status}'))

    def stub(self, name, body):
        path = self.bin/name;path.write_text('#!/bin/sh\n'+body+'\n');path.chmod(0o755)

    def tearDown(self):
        self.tmp.cleanup()

    def run_stop(self):
        return subprocess.run([shutil.which('busybox'), 'ash', str(self.script), 'stop'],
                              env=self.env, capture_output=True, text=True, timeout=3)

    def test_waits_for_exit_and_final_sync(self):
        result = self.run_stop()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls.read_text().splitlines(), ['signal', 'wait', 'wait', 'wait'])
        self.assertFalse(self.pid.exists())
        self.assertIn('stopped after final synchronization', result.stdout)

    def test_missing_process_is_idempotent(self):
        self.state.write_text('0\n')
        self.assertEqual(self.run_stop().returncode, 0)
        self.assertFalse(self.calls.exists())
        self.assertFalse(self.pid.exists())

    def test_timeout_preserves_pid_and_does_not_claim_sync(self):
        self.state.write_text('-1\n')
        result = self.run_stop()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.calls.read_text().splitlines().count('wait'), 10)
        self.assertTrue(self.pid.exists())
        self.assertIn('stop timed out', result.stderr)
        self.assertNotIn('stopped after final synchronization', result.stdout)

    def test_failed_sync_is_an_error(self):
        self.env['FINAL_SYNC'] = 'error'
        result = self.run_stop()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('without confirmed final synchronization', result.stderr)
        self.assertFalse(self.pid.exists())

    def test_stale_running_snapshot_is_not_final_confirmation(self):
        self.env['FINAL_STATE'] = 'running'
        result = self.run_stop()
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('stopped after final synchronization', result.stdout)


if __name__ == '__main__':
    unittest.main()
