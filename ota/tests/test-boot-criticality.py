#!/usr/bin/env python3
"""Exercise the native foreground health loop without touching host services."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
SOURCE=Path(__file__).resolve().parents[2]/'integration/systemd/fw-health-monitor'
class Criticality(unittest.TestCase):
    def check(self, states):
        with tempfile.TemporaryDirectory(prefix='systemd-health-loop-') as d:
            root=Path(d); calls=root/'calls'; count=root/'count'
            fw=root/'fwctl'
            fw.write_text('#!/bin/sh\nn=$(cat "$COUNT" 2>/dev/null || echo 0); n=$((n+1)); echo "$n" > "$COUNT"\ncase "$n" in\n'+''.join(f'{i+1}) exit {v} ;;\n' for i,v in enumerate(states))+'*) exit 1 ;;\nesac\n');fw.chmod(0o755)
            notify=root/'systemd-notify';notify.write_text('#!/bin/sh\necho "$*" >> "$CALLS"\n');notify.chmod(0o755)
            script=root/'monitor';script.write_text(SOURCE.read_text().replace('/run/fwctl-health.last',str(root/'last')).replace('/usr/sbin/fwctl',str(fw)).replace('/proc/cmdline',str(root/'cmdline')).replace('sleep 5','sleep 0'))
            (root/'cmdline').write_text('fw.rollback_test=0 fw.deployment=0\n')
            env=dict(os.environ,PATH=str(root)+':'+os.environ['PATH'],COUNT=str(count),CALLS=str(calls))
            result=subprocess.run(['sh',str(script)],env=env,capture_output=True,timeout=4)
            self.assertNotEqual(result.returncode,0)
            return int(count.read_text()),calls.read_text() if calls.exists() else ''
    def test_pending_start_never_announces_ready(self):
        count,calls=self.check([75,75,1]);self.assertEqual(count,3);self.assertNotIn('--ready',calls)
    def test_readiness_announced_once(self):
        count,calls=self.check([75,0,0,1]);self.assertEqual(count,4);self.assertEqual(calls.count('--ready'),1)
    def test_runtime_loss_has_bounded_grace(self):
        count,calls=self.check([0,75,75,75,0]);self.assertEqual(count,4);self.assertEqual(calls.count('--ready'),1)
    def test_runtime_loss_can_recover(self):
        count,calls=self.check([0,75,0,75,75,1]);self.assertEqual(count,6);self.assertEqual(calls.count('--ready'),1)
if __name__=='__main__': unittest.main()
