#!/usr/bin/env python3
"""Runtime supervision must distinguish update contention from local failure."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[2] / 'integration/systemd/fw-health-monitor'


class HealthMonitor(unittest.TestCase):
    def run_monitor(self, values):
        with tempfile.TemporaryDirectory(prefix='health-monitor-') as name:
            base = Path(name)
            fwctl = base / 'fwctl'
            fwctl.write_text('''#!/usr/bin/env python3
import os
from pathlib import Path
root=Path(os.environ['FIXTURE'])
counter=root/'count'
n=int(counter.read_text()) if counter.exists() else 0
values=os.environ['VALUES'].split(',')
counter.write_text(str(n+1))
(root/'uptime').write_text(str((n+1)*5)+'.00 0.00\\n')
raise SystemExit(int(values[n]) if n<len(values) else 1)
''')
            fwctl.chmod(0o755)
            notify = base / 'systemd-notify'
            notify.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$FIXTURE/notifications"\n')
            notify.chmod(0o755)
            (base / 'cmdline').write_text('')
            script = SOURCE.read_text().replace('/usr/sbin/fwctl', str(fwctl))
            for path in ('/proc/cmdline', '/proc/uptime', '/run/fwctl-health.last'):
                script = script.replace(path, str(base / Path(path).name))
            script = script.replace('sleep 5', ':')
            (base / 'monitor').write_text(script)
            p = subprocess.run(['sh', str(base / 'monitor')], capture_output=True, text=True,
                               env=dict(os.environ, FIXTURE=name, VALUES=','.join(map(str, values)),
                                        PATH=name+':'+os.environ['PATH']), timeout=8)
            return p, int((base / 'count').read_text()), (base / 'notifications').read_text()

    def test_update_contention_longer_than_old_grace_is_allowed(self):
        _, count, notifications = self.run_monitor([0]+[76]*5+[0, 1])
        self.assertEqual(count, 8)
        self.assertEqual(notifications.count('--ready'), 1)
        self.assertEqual(notifications.count('WATCHDOG=1'), 7)

    def test_contention_does_not_grant_startup_readiness(self):
        _, count, notifications = self.run_monitor([76, 76, 1])
        self.assertEqual(count, 3)
        self.assertNotIn('--ready', notifications)

    def test_contention_has_an_absolute_monotonic_budget(self):
        p, count, _ = self.run_monitor([0]+[76]*100)
        self.assertEqual(count, 38)
        self.assertIn('180 seconds', p.stderr)

    def test_local_failure_and_readiness_loss_are_not_hidden_by_lock(self):
        _, count, _ = self.run_monitor([0, 76, 1, 0])
        self.assertEqual(count, 3)
        _, count, _ = self.run_monitor([0, 76, 75, 75, 75, 0])
        self.assertEqual(count, 5)


if __name__ == '__main__':
    unittest.main()
