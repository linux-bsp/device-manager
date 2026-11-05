#!/usr/bin/env python3
"""Exercise board shutdown scripts with fake modules/CAN, never host hardware."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
OVERLAY=Path(__file__).resolve().parents[3]/'bsp-base-external/board/ti/am625/rootfs-overlay'
class Shutdown(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='board-stop-');self.root=Path(self.tmp.name)
        self.bin=self.root/'bin';self.bin.mkdir();self.module=self.root/'module';self.module.mkdir()
        (self.module/'refcnt').write_text('2\n');self.calls=self.root/'calls'
        self.env=dict(os.environ,PATH=str(self.bin),CALLS=str(self.calls),FAKE_MODULE=str(self.module),FAIL='0')
        for name in ('cat','readlink','ls'): (self.bin/name).symlink_to(shutil.which(name))
        self.stub('sleep','exit 0')
        self.stub('modprobe','echo modprobe >> "$CALLS"; [ "$FAIL" = 0 ] || exit 1; /bin/rm -rf "$FAKE_MODULE"')
        self.stub('ip','echo ip >> "$CALLS"')
    def tearDown(self): self.tmp.cleanup()
    def stub(self,name,body):
        f=self.bin/name;f.write_text('#!/bin/sh\n'+body+'\n');f.chmod(0o755)
    def run_script(self,name):
        s=(OVERLAY/'usr/libexec/bsp'/name).read_text().replace('PATH=/sbin:/bin:/usr/sbin:/usr/bin','')
        s=s.replace('SYS_MODULE=/sys/module/$MODULE','SYS_MODULE="'+str(self.module)+'"')
        s=s.replace('/sys/module/pdm',str(self.module)).replace('/proc/[0-9]*/fd/*',str(self.root/'proc/[0-9]*/fd/*'))
        s=s.replace('/etc/default/can',str(self.root/'missing')).replace('/sbin/ip',str(self.bin/'ip'))
        f=self.root/name;f.write_text(s)
        return subprocess.run([shutil.which('busybox'),'ash',str(f),'stop'],env=self.env,text=True,capture_output=True)
    def test_pdm_stop_unloads_once(self):
        r=self.run_script('pdm');self.assertEqual(r.returncode,0,r.stderr)
        self.assertEqual(self.calls.read_text(),'modprobe\n');self.assertFalse(self.module.exists())
    def test_pdm_failure_is_not_ok_and_can_stays_up(self):
        self.env['FAIL']='1';r=self.run_script('pdm')
        self.assertNotEqual(r.returncode,0);self.assertIn('FAIL',r.stdout);self.assertNotIn('OK',r.stdout)
        self.assertIn('reference count: 2',r.stdout)
        r=self.run_script('can');self.assertNotEqual(r.returncode,0)
        self.assertNotIn('ip',self.calls.read_text())

if __name__=='__main__': unittest.main()
