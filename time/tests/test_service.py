#!/usr/bin/env python3
"""Time service ownership and configuration; never alter the host clock."""
import importlib.util
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest

spec=importlib.util.spec_from_file_location('clock_test',Path(__file__).with_name('test_time_sync.py'))
base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)
ROOT=Path(__file__).resolve().parents[1]

class Service(unittest.TestCase):
    setUpClass=classmethod(base.ClockSync.setUpClass.__func__)
    tearDownClass=classmethod(base.ClockSync.tearDownClass.__func__)
    start_worker=base.ClockSync.start_worker

    def setUp(self):
        base.ClockSync.setUp(self)
        self.disk=tempfile.TemporaryDirectory(prefix='.time-service-',dir=ROOT)
        self.state=Path(self.disk.name)/'source.conf'
        self.env['TIMECTL_STATE_FILE']=str(self.state)

    def tearDown(self):
        base.ClockSync.tearDown(self);self.disk.cleanup()

    def cli(self,*args):
        return subprocess.run([str(ROOT/'timectl'),*args],env=self.env,capture_output=True,text=True,timeout=16)

    def test_cli_never_sets_time_without_daemon(self):
        for command in ('status','sync'):
            self.assertNotEqual(self.cli(command).returncode,0)
        self.assertFalse(self.capture.exists())

    def test_single_owner_and_restart_status(self):
        self.start_worker()
        other=subprocess.run([str(ROOT/'timectld')],env=self.env,capture_output=True,text=True,timeout=3)
        self.assertNotEqual(other.returncode,0)
        self.assertIn('busy',other.stderr.lower())
        self.assertIn('daemon-state=running',self.cli('status').stdout)
        self.assertEqual(self.cli('sync').returncode,0)
        self.assertIn('clock-state=synchronized',self.cli('status').stdout)
        self.worker.terminate();self.worker.communicate(timeout=3);self.worker=None
        self.assertNotEqual(self.cli('status').returncode,0)

    def test_time_config_does_not_depend_on_sol_config_or_log_directories(self):
        missing=self.base/'missing-sol.conf'
        self.env['LOGCTL_SOL_CONFIG']=str(missing)
        self.env['LOGCTL_RUN_DIR']=str(self.base/'absent-log-run')
        self.start_worker()
        self.assertEqual(self.cli('sync').returncode,0)
        self.assertFalse((self.base/'absent-log-run').exists())

    def test_local_source_is_durable_and_validated(self):
        self.start_worker()
        self.assertEqual(self.cli('source','set',self.url).returncode,0)
        self.assertIn(self.url,self.state.read_text())
        before=self.state.read_bytes()
        for value in ('ntp://host:0','file:///tmp/clock','http://host/path','ntp://host\nextra=x'):
            self.assertNotEqual(self.cli('source','set',value).returncode,0)
            self.assertEqual(self.state.read_bytes(),before)
        self.worker.terminate();self.worker.communicate(timeout=3);self.worker=None
        self.start_worker()
        self.assertIn('clock-effective-source='+self.url,self.cli('status').stdout)

    def test_import_existing_source_once_without_removing_rollback_copy(self):
        old=Path(self.disk.name)/'old-source.conf'
        old.write_text(f'DMCLOCKLOCAL1 7 0 120 {self.url} 0\n')
        self.env['TIMECTL_IMPORT_FILE']=str(old)
        self.start_worker();self.assertEqual(self.cli('status').returncode,0)
        self.assertEqual(old.read_bytes(),self.state.read_bytes())
        original=self.state.read_bytes()
        self.worker.terminate();self.worker.communicate(timeout=3);self.worker=None
        old.write_text('corrupt old copy\n');self.start_worker()
        self.assertIn('clock-interval=120',self.cli('status').stdout)
        self.assertEqual(self.state.read_bytes(),original)

    def test_ram_storage_cannot_claim_durable_configuration(self):
        with tempfile.TemporaryDirectory(dir='/dev/shm',prefix='time-state-') as ram:
            self.env['TIMECTL_STATE_FILE']=ram+'/source.conf'
            self.start_worker()
            self.assertNotEqual(self.cli('source','set',self.url).returncode,0)
            self.assertFalse(Path(self.env['TIMECTL_STATE_FILE']).exists())
            self.assertEqual(self.cli('sync').returncode,0)

    def test_malformed_ipc_and_idle_client_do_not_control_clock(self):
        self.start_worker();self.assertEqual(self.cli('status').returncode,0)
        for request in (b'bad',b'sync\0',b'x'*700):
            with socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET) as sock:
                sock.settimeout(2);sock.connect(str(self.base/'run/control.sock'));sock.send(request)
                self.assertFalse(sock.recv(2048).startswith(b'0\n'))
        with socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET) as idle:
            idle.connect(str(self.base/'run/control.sock'))
            start=time.monotonic();self.assertEqual(self.cli('status').returncode,0)
            self.assertLess(time.monotonic()-start,1)
        self.assertFalse(self.capture.exists())

    def test_logging_binary_has_no_time_adjustment_or_time_command(self):
        binary=ROOT.parent/'logging/logctl'
        symbols=subprocess.check_output(['nm','-D',str(binary)],text=True)
        self.assertNotIn('clock_settime',symbols)
        self.assertNotIn('dm_clock_cycle',symbols)
        self.assertNotIn('sync-time',subprocess.run([str(binary)],env=dict(self.env,
                         LOGCTL_RUN_DIR=str(self.base/'log-run'),LOGCTL_ROOT=str(self.base/'log-root')),
                         capture_output=True,text=True).stderr)

if __name__=='__main__':unittest.main()
