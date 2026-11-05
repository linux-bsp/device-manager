#!/usr/bin/env python3
"""Exercise real daemon/CLI with isolated runtime directories, no device writes."""
import os
import json
import re
from pathlib import Path
import socket
import subprocess
import tarfile
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]

class Logging(unittest.TestCase):
    def test_retention_journal_rotation_and_sync_failure(self):
        binary=self.base/'retention-test'
        subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror',
            '-Wl,--wrap=fsync',str(ROOT/'tests/retention_test.c'),str(ROOT/'retention.c'),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,capture_output=True)
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='logctl-')
        self.base = Path(self.tmp.name)
        self.store = self.base/'boots'; self.store.mkdir()
        self.services = self.base/'services'; self.services.mkdir()
        self.run = self.base/'run'
        self.env = dict(os.environ, LOGCTL_RUN_DIR=str(self.run),
                        LOGCTL_BOOT_ID='test-boot-1', LOGCTL_SERVICE_DIR=str(self.services),
                        LOGCTL_FILE_LIMIT='4096')
        self.process = None
    def tearDown(self):
        self.stop(); self.tmp.cleanup()
    def cli(self, *args, **kw):
        return subprocess.run([str(ROOT/'logctl'), *args], env=self.env,
                              capture_output=True, text=True, timeout=5, **kw)
    def start(self):
        self.process = subprocess.Popen([str(ROOT/'logctld'), '--root', str(self.store)],
            env=self.env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.wait(lambda: (self.run/'socket').exists())
    def stop(self):
        if self.process:
            self.process.terminate()
            self.process.communicate(timeout=5)
            self.assertEqual(self.process.returncode, 0)
            self.process = None
    def wait(self, predicate):
        for _ in range(100):
            if predicate(): return
            if self.process and self.process.poll() is not None:
                self.fail(self.process.stderr.read().decode())
            time.sleep(.02)
        self.fail('timed out')
    def text(self, name='services'):
        p=self.store/self.env['LOGCTL_BOOT_ID']/(name+'.log')
        return p.read_text() if p.exists() else ''
    def test_storage_marker_and_boot_stage_checks(self):
        self.start()
        result=self.cli('verify-storage')
        self.assertIn(result.returncode,(0,2))
        if result.returncode==0:
            marker=self.store/self.env['LOGCTL_BOOT_ID']/'storage-check'
            self.assertEqual(marker.read_text(),self.env['LOGCTL_BOOT_ID'])
            self.stop();self.env['LOGCTL_BOOT_ID']='test-boot-2';self.start()
            self.assertIn('cross-boot=verified',self.cli('verify-storage').stdout)
        handoff=self.base/'handoff';handoff.mkdir()
        self.env['LOGCTL_HANDOFF_DIR']=str(handoff)
        self.assertEqual(self.cli('verify-boot').returncode,1)
        for stage in ('r5-spl','a53-spl','uboot'):(handoff/stage).write_text('verified fixture\n')
        self.assertEqual(self.cli('verify-boot').returncode,0)
    def test_early_capture_and_exit_status(self):
        result=self.cli('run','services','/bin/sh','-c','echo early-message; exit 7')
        self.assertEqual(result.returncode,7)
        self.assertIn('early-message',result.stdout)
        self.start();self.wait(lambda:'early-message' in self.text())
    def test_stop_confirms_final_sync_and_collector_exit(self):
        self.start()
        self.cli('write','services','last-record-before-stop',check=True)
        self.wait(lambda:'last-record-before-stop' in self.text())
        self.stop()
        status=dict(line.split('=',1) for line in (self.run/'sync-status').read_text().splitlines())
        self.assertEqual(status['collector-state'],'stopped')
        self.assertEqual(status['sync-state'],'ok')
        self.assertEqual(status['sync-errno'],'0')
        self.assertEqual(status['synced-sequence'],status['observed-sequence'])
        self.assertFalse((self.run/'socket').exists())
    def test_partial_console_bytes_are_preserved(self):
        self.start()
        fragments = [b'Starting CAN: ', b'OK\n\n   kernel\r\n',
                     '[fwctl] [INFO] 中文'.encode(), b'\n# ']
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sock:
            for fragment in fragments:
                sock.sendto(b'services'.ljust(16, b'\0') + fragment, str(self.run/'socket'))
        self.wait(lambda: '# ' in self.text())
        self.stop()
        raw = (self.store/'test-boot-1/services.log').read_bytes()
        body = b''
        while raw:
            header = re.match(rb'\[seq=\d+ mono=[0-9.]+ wall=\d+ bytes=(\d+) source=console\] ', raw)
            self.assertIsNotNone(header)
            length = int(header[1]); raw = raw[header.end():]
            self.assertGreaterEqual(len(raw), length)
            body += raw[:length]; raw = raw[length:]
        self.assertEqual(body, b''.join(fragments))

    def test_socket_rotation_and_export(self):
        self.start()
        for i in range(12):
            self.cli('write','services',str(i)+'x'*1000,check=True)
        self.wait(lambda:'11xxx' in self.text());self.stop()
        files=list((self.store/'test-boot-1').glob('services.log*'))
        self.assertEqual(len(files),2)
        self.assertTrue(all(p.stat().st_size<=4096 for p in files))
        losses=[json.loads(line) for line in self.cli('losses',check=True).stdout.splitlines()]
        self.assertTrue(any(e['reason']=='stream-limit' and e['stage']=='services' and e['bytes']>0 for e in losses))
        output=self.base/'bundle.tar.gz'
        self.cli('export','--output',str(output),check=True)
        with tarfile.open(output) as archive:
            self.assertIn('test-boot-1/services.log',archive.getnames())
    def test_service_tail_truncation_and_replacement(self):
        self.start();p=self.services/'test.log';p.write_text('first-message\n')
        self.wait(lambda:'first-message' in self.text())
        p.write_text('x\n');self.wait(lambda:'x\n' in self.text())
        p.unlink();p.write_text('replacement-message\n')
        self.wait(lambda:'replacement-message' in self.text())
    def test_retention_and_previous(self):
        for i in range(15):
            self.env['LOGCTL_BOOT_ID']=f'test-boot-{i}'
            self.start();self.stop()
        self.assertEqual(len([p for p in self.store.iterdir() if p.is_dir()]),12)
        self.assertIn('test-boot-14 current',self.cli('boots').stdout)
        losses=[json.loads(line) for line in self.cli('losses',check=True).stdout.splitlines()]
        self.assertEqual({e['boot'] for e in losses if e['reason']=='boot-limit'}, {'test-boot-0','test-boot-1','test-boot-2'})
        self.assertIn('reason=boot-limit', self.text('events'))
        self.assertEqual(self.cli('show','--boot','previous').returncode,0)
    def test_retention_cannot_delete_without_durable_audit(self):
        for i in range(12):
            boot=self.store/f'old-{i}';boot.mkdir();(boot/'order').write_text(str(i+1))
            (boot/'services.log').write_text('retained evidence')
        victim=self.base/'outside';victim.write_text('unchanged')
        (self.store/'.retention.log').symlink_to(victim)
        self.env['LOGCTL_BOOT_ID']='test-boot-new';self.start();self.stop()
        self.assertTrue((self.store/'old-0/services.log').exists())
        self.assertEqual(victim.read_text(),'unchanged')
    def test_byte_quota_evicts_oldest_and_preserves_current(self):
        for i in range(5):
            boot=self.store/f'old-{i}';boot.mkdir();(boot/'order').write_text(str(i+1))
            (boot/'kernel.log').write_bytes(b'x'*8192)
        self.env['LOGCTL_TOTAL_LIMIT']='24576';self.start();self.stop()
        self.assertTrue((self.store/'test-boot-1').is_dir())
        self.assertFalse((self.store/'old-0').exists())
        self.assertFalse((self.store/'old-1').exists())
        self.assertTrue((self.store/'old-4/kernel.log').exists())
        losses=[json.loads(line) for line in self.cli('losses',check=True).stdout.splitlines()]
        self.assertTrue(all(e['reason']=='capacity-limit' for e in losses))
    def test_single_daemon_and_malformed_packets(self):
        self.start()
        other=subprocess.run([str(ROOT/'logctld'),'--root',str(self.store)],env=self.env,capture_output=True)
        self.assertNotEqual(other.returncode,0)
        with socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM) as sock:
            sock.sendto(b'../bad\0'+b'x'*40,str(self.run/'socket'))
        self.cli('write','services','still-alive',check=True)
        self.wait(lambda:'still-alive' in self.text())
        self.assertFalse((self.base/'bad.log').exists())
    def test_symlink_and_traversal_rejected(self):
        self.start();victim=self.base/'victim';victim.write_text('protected')
        (self.store/'test-boot-1/services.log').symlink_to(victim)
        self.cli('write','services','overwrite')
        time.sleep(.1);self.assertEqual(victim.read_text(),'protected')
        self.assertNotEqual(self.cli('show','--boot','../victim').returncode,0)
        self.assertNotEqual(self.cli('export','--output',str(self.store/'bad.tar.gz')).returncode,0)

if __name__=='__main__':unittest.main()
