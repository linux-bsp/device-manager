#!/usr/bin/env python3
"""Only a stable, confirmed boot may authorize startup-buffer export/clear."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
WORKSPACE=Path(__file__).resolve().parents[3]
SOURCE=WORKSPACE/'bsp-base-external/board/ti/am625/rootfs-overlay/etc/init.d/S99fw-mark-good'
class BootHealth(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='boot-health-');self.base=Path(self.tmp.name)
        self.marker=self.base/'confirmed';self.calls=self.base/'calls'
        fw=self.base/'fwctl'
        fw.write_text('''#!/bin/sh
for arg do last=$arg; done
printf '%s\\n' "$last" >> "$CALLS"
case "$last" in
health-check) exit "${HEALTH_EXIT:-0}" ;;
status) echo "deployment 1: state=confirmed successful=${CONFIRMED:-0} tries=3" ;;
mark-good) exit "${MARK_EXIT:-0}" ;;
esac
''');fw.chmod(0o755)
        text=SOURCE.read_text().replace('/usr/sbin/fwctl',str(fw)).replace('interval=5','interval=0').replace('/run/fwctl-boot-confirmed',str(self.marker)).replace('/run/rcS.failed',str(self.base/'failed')).replace('/var/run/fw-mark-good.pid',str(self.base/'pid'))
        self.script=self.base/'init';self.script.write_text(text)
    def tearDown(self):self.tmp.cleanup()
    def run_worker(self,**values):
        env=dict(os.environ,CALLS=str(self.calls),**values)
        return subprocess.run([shutil.which('busybox'),'ash',str(self.script),'worker','1'],env=env,capture_output=True,text=True,timeout=5)
    def test_pending_success_requires_six_checks_and_mark_good(self):
        p=self.run_worker();self.assertEqual(p.returncode,0,p.stderr)
        self.assertEqual(self.marker.read_text(),'1\n');self.assertEqual(self.calls.read_text().count('health-check'),6)
        self.assertIn('mark-good',self.calls.read_text())
    def test_previously_confirmed_still_requires_this_boot_health(self):
        p=self.run_worker(CONFIRMED='1');self.assertEqual(p.returncode,0,p.stderr)
        self.assertEqual(self.marker.read_text(),'1\n');self.assertEqual(self.calls.read_text().count('health-check'),6)
        self.assertNotIn('mark-good',self.calls.read_text())
    def test_bad_health_never_authorizes_clear(self):
        self.run_worker(HEALTH_EXIT='1',CONFIRMED='1');self.assertFalse(self.marker.exists())
    def test_failed_metadata_confirmation_never_authorizes_clear(self):
        self.run_worker(MARK_EXIT='1');self.assertFalse(self.marker.exists());self.assertTrue((self.base/'failed').exists())
if __name__=='__main__':unittest.main()
