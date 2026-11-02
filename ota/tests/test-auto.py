#!/usr/bin/env python3
"""Real HTTP/agent integration; mock only fwctl storage and reboot. No hardware writes."""
import importlib.util
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import unittest

spec = importlib.util.spec_from_file_location('admin_tests', Path(__file__).with_name('test-admin.py'))
admin = importlib.util.module_from_spec(spec)
spec.loader.exec_module(admin)

class AutomaticOTA(unittest.TestCase):
    def setUp(self):
        admin.AdminHTTP.setUp(self)
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        self.snapshot = self.root / 'snapshot'
        self.snapshot.write_text('FWOTA1 0 0 255 65536 1 4 0 0 0\n')
        self.calls = self.root / 'calls'
        self.fail_marker = self.root / 'fail'
        fw = self.bin / 'fwctl'
        fw.write_text('#!/bin/sh\n' +
            'case "$1" in\n' +
            'ota-status) cat "$TEST_SNAPSHOT";;\n' +
            'ota-check) set -- "$2" $(cat "$TEST_SNAPSHOT"); target=$1; boot=$3; '
            'if [ "$boot" = 0 ]; then current=$6; else current=$9; fi; '
            'if [ "$target" = "$current" ] && [ ! -e "$TEST_CORRUPT" ]; then echo "FWCHECK1 match"; else echo "FWCHECK1 different"; fi;;\n' +
            'update) echo install >> "$TEST_CALLS"; [ ! -e "$TEST_FAIL" ] || exit 1; '
            'echo "FWOTA1 0 0 1 65536 1 4 65537 0 2" > "$TEST_SNAPSHOT";;\n' +
            '*) exit 2;;\nesac\n')
        fw.chmod(0o755)
        reboot = self.bin / 'reboot'
        reboot.write_text('#!/bin/sh\necho reboot >> "$TEST_CALLS"\n')
        reboot.chmod(0o755)
        self.env = dict(admin.ENV, PATH=str(self.bin)+':'+admin.ENV['PATH'],
            TEST_CORRUPT=str(self.root/'corrupt'), TEST_SNAPSHOT=str(self.snapshot), TEST_CALLS=str(self.calls), TEST_FAIL=str(self.fail_marker))
        self.publish(self.draft())

    def agent(self, ok=True, force=False):
        r = subprocess.run([str(admin.SOURCE / 'fwctld'), '--server', f'http://127.0.0.1:{self.port}',
            '--cache', str(self.root/'cache'), '--product', 'ti-am62x', '--layout', 'am62x-sd-ab-v1',
            '--auto-install', '--device', 'board-01', '--once'] + (['--force'] if force else []), env=self.env, capture_output=True, text=True)
        if ok: self.assertEqual(r.returncode, 0, r.stdout+r.stderr)
        else: self.assertNotEqual(r.returncode, 0, r.stdout+r.stderr)
        return r

    def test_logs_visible_during_download_and_install(self):
        console=self.root/'console';console.touch()
        log=self.root/'agent.log'
        download_wait=self.root/'download-wait';download_go=self.root/'download-go'
        install_wait=self.root/'install-wait';install_go=self.root/'install-go'
        curl=self.bin/'curl'
        real_curl=shutil.which('curl',path=admin.ENV['PATH'])
        curl.write_text('#!/bin/sh\ncase "$*" in *"/releases/1/kernel.itb"*)\n'
            f'touch "{download_wait}"\nwhile [ ! -e "{download_go}" ]; do sleep .05; done;;\nesac\n'
            f'exec "{real_curl}" "$@"\n')
        curl.chmod(0o755)
        fw=self.bin/'fwctl';text=fw.read_text().replace('update) echo install',
            f'update) echo FLASH_WRITE_BEGIN; echo FLASH_ERROR_DETAIL >&2; touch "{install_wait}"; '
            f'while [ ! -e "{install_go}" ]; do sleep .05; done; echo install')
        fw.write_text(text)
        def until(predicate):
            for _ in range(150):
                if predicate():return
                time.sleep(.02)
            self.fail('Expected progress before blocked operation completes')
        with log.open('w') as stream:
            proc=subprocess.Popen([str(admin.SOURCE/'fwctld'),'--server',f'http://127.0.0.1:{self.port}',
                '--cache',str(self.root/'cache'),'--product','ti-am62x','--layout','am62x-sd-ab-v1',
                '--auto-install','--device','board-01','--console',str(console),'--once'],env=self.env,stdout=stream,stderr=stream)
            try:
                until(download_wait.exists)
                for dest in (log,console):
                    text=dest.read_text();self.assertRegex(text, r'Discovered firmware v1\.0\.1 / [0-9a-f]{8};')
                    self.assertNotRegex(text, r'v1\.0\.1 / [0-9a-f]{9}')
                    self.assertIn('[fwctld] [INFO] Discovered firmware',text)
                    self.assertNotIn('====',text)
                    self.assertNotIn('publication #',text)
                    self.assertIn('Downloading kernel.itb',text)
                    self.assertNotIn('Downloaded and SHA256-verified',text)
                self.assertIsNone(proc.poll())
                download_go.touch();until(install_wait.exists)
                until(lambda:'FLASH_ERROR_DETAIL' in console.read_text())
                for dest in (log,console):
                    text=dest.read_text();self.assertIn('Installing firmware',text)
                    self.assertIn(': installing (',text)
                    self.assertNotIn(': success (',text)
                    self.assertIn('FLASH_WRITE_BEGIN',text);self.assertIn('FLASH_ERROR_DETAIL',text)
                    self.assertNotIn('rebooting now',text)
                self.assertIsNone(proc.poll())
                install_go.touch();self.assertEqual(proc.wait(timeout=10),0)
                self.assertIn('rebooting now',console.read_text())
            finally:
                download_go.touch();install_go.touch()
                if proc.poll() is None:proc.terminate()
                proc.wait(timeout=10)

    def phase(self):
        return self.state()['devices'][0]['phase']

    def reject_payload_requests(self):
        curl=self.bin/'curl';real=shutil.which('curl',path=admin.ENV['PATH'])
        self.unexpected_download=self.root/'unexpected-download'
        curl.write_text('#!/bin/sh\ncase "$*" in *".itb"*) echo unexpected > "'+str(self.unexpected_download)+'"; exit 22;; esac\nexec "'+real+'" "$@"\n')
        curl.chmod(0o755)

    def test_auto_success_waits_for_confirmation(self):
        self.agent()
        self.assertEqual(self.calls.read_text(), 'install\nreboot\n')
        self.assertEqual(self.phase(), 'rebooting')
        self.snapshot.write_text('FWOTA1 1 0 1 65536 1 4 65537 0 3\n')
        shutil.rmtree(self.root/'cache')
        boot_result=self.agent()
        self.assertEqual(self.phase(), 'boot-testing')
        self.assertNotIn(': boot-testing',boot_result.stdout)
        self.snapshot.write_text('FWOTA1 1 1 255 65536 1 4 65537 1 4\n')
        self.reject_payload_requests()
        result=self.agent()
        self.assertEqual(self.phase(), 'success')
        self.assertNotIn('SHA256 match',result.stdout)
        self.assertNotIn(': success',result.stdout)
        self.assertNotIn('Ready: fwctl update',result.stdout)
        self.agent()
        self.assertFalse(self.unexpected_download.exists())
        self.assertFalse((self.root/'cache/1').exists())
        self.assertEqual(self.calls.read_text(), 'install\nreboot\n')

    def test_request_context_survives_reboot_and_health_confirmation(self):
        import socket
        self.agent()
        rows=self.state()['device_history'][0]['network']
        self.assertIn('requested', [r['phase'] for r in rows])
        self.assertIn('installing', [r['phase'] for r in rows])
        for row in rows:
            self.assertEqual(row['hostname'],socket.gethostname())
            self.assertEqual(row['peer_ip'],'127.0.0.1')
            self.assertNotEqual(row['ip'],'unknown')
            self.assertRegex(row['mac'],r'^(?:[0-9a-f]{2}:){5}[0-9a-f]{2}$')
        self.snapshot.write_text('FWOTA1 1 1 255 65536 1 4 65537 1 4\n')
        self.agent()
        rows=self.state()['device_history'][0]['network']
        self.assertIn('success',[r['phase'] for r in rows])
        self.assertIn('requested',[r['phase'] for r in rows])

    def test_storage_error_reported_separately(self):
        fw = self.bin/'fwctl'
        fw.write_text(fw.read_text().replace('|| exit 1;', '|| exit 5;'))
        self.fail_marker.touch()
        self.agent(ok=False)
        self.assertEqual(self.phase(), 'device-error')
        history = self.state()['device_history'][0]
        self.assertEqual(history['category'], 'device-error')
        self.assertEqual(history['reason'], 'fwctl-exit-5')
        self.assertEqual(self.state()['latest'], 0)

    def test_auto_failure_blocks_same_publication(self):
        self.fail_marker.touch()
        self.agent(ok=False)
        self.assertEqual(self.phase(), 'install-failed')
        self.fail_marker.unlink()
        shutil.rmtree(self.root/'cache')
        self.agent()
        self.assertEqual(self.calls.read_text(), 'install\n')
        self.assertEqual(self.phase(), 'install-failed')
        self.agent(force=True)
        self.assertEqual(self.calls.read_text(), 'install\n')
        self.assertEqual(self.state()['latest'], 0)

    def test_auto_rollback_report(self):
        self.agent()
        self.snapshot.write_text('FWOTA1 0 0 255 65536 1 4 65537 0 5\n')
        shutil.rmtree(self.root/'cache')
        self.agent()
        self.assertEqual(self.phase(), 'rolled-back')
        self.assertEqual(self.calls.read_text(), 'install\nreboot\n')
        self.reject_payload_requests()
        self.agent()
        self.assertFalse(self.unexpected_download.exists())
        history = list((self.feed/'history/board-01').iterdir())[0].read_text()
        self.assertIn('rolled-back', history)

    def test_absent_publication_is_normal_waiting(self):
        (self.feed/'latest').unlink()
        result = self.agent()
        self.assertIn('No active OTA publication; waiting', result.stdout)
        self.assertNotIn('failed', result.stdout + result.stderr)
        self.assertFalse(self.calls.exists())

    def test_failed_content_globally_blocked_even_when_reuploaded(self):
        self.agent()
        self.snapshot.write_text('FWOTA1 0 0 255 65536 1 4 65537 0 5\n')
        self.agent()
        draft = self.draft(self.bundle(self.files))
        self.assertEqual(self.request('POST', '/api/publish/'+draft, b'', {'X-OTA-Request':'1'})[0], 409)
        self.assertEqual(self.state()['latest'], 0)
        status, body = self.request('GET', '/api/policy/another-board/1')
        self.assertEqual(status, 200)
        self.assertIn(b'blocked', body)
        self.assertEqual(self.request('GET', '/releases/1/kernel.itb')[0], 410)

    def test_different_content_allowed_after_failure_and_history_retained(self):
        self.agent()
        self.snapshot.write_text('FWOTA1 0 0 255 65536 1 4 65537 0 5\n')
        self.agent()
        self.publish(self.draft(self.bundle(self.fixture(65538))))
        fw = self.bin/'fwctl'
        fw.write_text(fw.read_text().replace('65537 0 2', '65538 0 2'))
        self.agent()
        self.assertEqual(self.phase(), 'rebooting')
        self.assertEqual(self.state()['devices'][0]['publication'], 2)
        status, body = self.request('GET', '/api/policy/board-01/1')
        self.assertEqual(status, 200)
        self.assertIn(b'blocked', body)

    def test_new_publication_downloads_after_success_with_empty_cache(self):
        self.agent()
        self.snapshot.write_text('FWOTA1 1 1 255 65536 1 4 65537 1 4\n')
        shutil.rmtree(self.root/'cache')
        self.agent()
        self.assertEqual(self.phase(),'success')
        self.assertFalse((self.root/'cache/1').exists())
        self.publish(self.draft(self.bundle(self.fixture(65538))))
        fw=self.bin/'fwctl'
        fw.write_text(fw.read_text().replace('FWOTA1 0 0 1 65536 1 4 65537 0 2','FWOTA1 1 1 0 65538 0 2 65537 1 4'))
        self.agent()
        self.assertEqual(self.phase(),'rebooting')
        self.assertEqual(self.state()['devices'][0]['publication'],2)
        self.assertTrue((self.root/'cache/2/rootfs.itb').exists())
        self.assertEqual(self.calls.read_text(),'install\nreboot\ninstall\nreboot\n')

    def test_auto_existing_version_not_installed(self):
        self.snapshot.write_text('FWOTA1 0 0 255 65537 1 4 0 0 0\n')
        self.agent()
        self.assertFalse(self.state()['devices'])
        self.assertFalse((self.root/'cache/1').exists())
        self.assertFalse(self.calls.exists())

    def test_matching_version_corrupt_contents_reinstalled(self):
        self.snapshot.write_text('FWOTA1 0 0 255 65537 1 4 0 0 0\n')
        (self.root/'corrupt').touch()
        self.agent()
        self.assertEqual(self.calls.read_text(), 'install\nreboot\n')

    def test_force_reinstalls_matching_image_and_refreshes_cache(self):
        self.snapshot.write_text('FWOTA1 0 0 255 65537 1 4 0 0 0\n')
        # Seed a corrupt payload cache to ensure forced recovery downloads again.
        cache = self.root/'cache/1'
        cache.mkdir(parents=True)
        (cache/'rootfs.itb').write_bytes(b'broken cache')
        result = self.agent(force=True)
        self.assertIn('Force recovery requested', result.stdout)
        self.assertIn('Downloading rootfs.itb', result.stdout)
        self.assertEqual(self.calls.read_text(), 'install\nreboot\n')
        self.assertEqual((cache/'rootfs.itb').read_bytes(), self.files['rootfs.itb'])

    def test_force_cannot_overwrite_pending_deployment(self):
        self.snapshot.write_text('FWOTA1 0 0 1 65536 1 4 65538 0 2\n')
        self.agent(force=True)
        self.assertFalse(self.calls.exists())

    def test_server_identity_hashes_written_payloads(self):
        status, body = self.request('GET', '/releases/1/identity')
        self.assertEqual(status, 200, body)
        fields = body.decode().split()
        self.assertEqual(fields[:3], ['BSPOTAIMAGE1', '1', '65537'])
        self.assertEqual(int(fields[3]), len(self.files['kernel.itb']))
        self.assertEqual(fields[4], hashlib.sha256(self.files['kernel.itb']).hexdigest())
        self.assertEqual(int(fields[5]), 256 * 1100)
        self.assertEqual(fields[6], hashlib.sha256(bytes(range(256)) * 1100).hexdigest())

    def test_auto_server_offline_blocks_install(self):
        self.server.terminate(); self.server.wait(timeout=10)
        result=self.agent(ok=False)
        self.assertIn('network/HTTP request failed',result.stdout)
        self.assertFalse(self.calls.exists())

    def test_auto_bad_snapshot_blocks_install(self):
        self.snapshot.write_text('FWOTA1 2 0 255 65536 1 4 0 0 0\n')
        result=self.agent(ok=False)
        self.assertIn('local firmware status',result.stdout)
        self.assertFalse(self.calls.exists())

    def test_auto_journal_read_error_blocks_install(self):
        (self.feed/'devices').mkdir()
        (self.feed/'devices/board-01').write_text('corrupt state')
        self.agent(ok=False)
        self.assertFalse(self.calls.exists())

    def test_auto_pending_manual_update_not_overwritten(self):
        self.snapshot.write_text('FWOTA1 0 0 1 65536 1 4 65538 0 2\n')
        self.agent()
        self.assertFalse(self.calls.exists())

    def test_auto_interrupted_before_write(self):
        boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        body = f'BSPOTASTATE1 board-01 ti-am62x am62x-sd-ab-v1 1 65537 {boot} installing 1 65536\n'
        self.assertEqual(self.request('POST', '/api/devices/board-01', body, {'X-OTA-Request':'1'})[0], 200)
        self.agent()
        self.assertEqual(self.phase(), 'interrupted')
        self.assertFalse(self.calls.exists())
        self.assertEqual(self.state()['latest'], 0)
        self.assertIn('interrupted', list((self.feed/'history/board-01').iterdir())[0].read_text())

    def test_report_validation_and_persistence(self):
        self.assertEqual(self.request('GET', '/api/devices/new-board')[0], 204)
        for ident, body in [('..','bad'), ('board-01','bad'),
            ('board-01','BSPOTASTATE1 other ti-am62x am62x-sd-ab-v1 1 65537 boot success 1 65537\n'),
            ('board-01','BSPOTASTATE1 board-01 ti-am62x am62x-sd-ab-v1 1 4294967297 boot success 1 65537\n')]:
            self.assertEqual(self.request('POST', '/api/devices/'+ident, body, {'X-OTA-Request':'1'})[0], 400)
        self.agent()
        self.assertTrue((self.feed/'devices/board-01').is_file())
        self.assertEqual(self.request('GET','/api/devices/board-01')[0], 200)

    def kernel_bundle(self, version, payload='kernel'):
        dts = f'/dts-v1/; / {{ description="{payload}"; configurations {{ default="c"; c {{ fw,version=<{version}>; }}; }}; }};'
        kernel = subprocess.run(['dtc','-I','dts','-O','dtb'], input=dts.encode(),capture_output=True,check=True,env=admin.ENV).stdout
        files = self.fixture(version=version)
        oldhash = hashlib.sha256(files['kernel.itb']).hexdigest().encode()
        newhash = hashlib.sha256(kernel).hexdigest().encode()
        files['release.itb'] = files['release.itb'].replace(oldhash, newhash)
        files['kernel.itb'] = kernel
        return self.bundle(files), kernel

    def test_rootfs_recovery_uses_exact_payload_hash(self):
        digest = hashlib.sha256(bytes(range(256)) * 1100).hexdigest()
        path = f'/recovery/ti-am62x/am62x-sd-ab-v1/rootfs/{digest}/rootfs.itb'
        self.assertEqual(self.request('GET', path), (200, self.files['rootfs.itb']))
        self.assertEqual(self.request('GET', path.replace(digest, '0'*64))[0], 404)
        self.assertEqual(self.request('GET', path.replace('ti-am62x', 'wrong'))[0], 404)
        self.assertEqual(self.request('GET', path+'/extra')[0], 404)
        self.assertEqual(self.request('GET', path.replace(digest, 'x'*64))[0], 404)

    def test_recovery_exact_version_and_ambiguity(self):
        bundle, kernel = self.kernel_bundle(65538)
        self.assertEqual(self.publish(self.draft(bundle))[0], 200)
        path='/recovery/ti-am62x/am62x-sd-ab-v1/kernel/65538/kernel.itb'
        self.assertEqual(self.request('GET',path), (200,kernel))
        self.assertEqual(self.request('GET',path.replace('65538','65539'))[0],404)
        self.assertEqual(self.request('GET',path.replace('ti-am62x','wrong'))[0],404)
        self.assertEqual(self.request('GET','/releases/2/info'), (200,b'BSPOTAINFO1 2 65538\n'))
        self.assertEqual(self.request('GET',path+'/extra')[0],404)
        # Same version with different bytes must not be selected arbitrarily.
        other, _ = self.kernel_bundle(65538, 'changed')
        self.assertEqual(self.publish(self.draft(other))[0],200)
        self.assertEqual(self.request('GET',path)[0],404)

for name in ('tearDown', 'fixture', 'bundle', 'request', 'upload', 'state', 'publish', 'draft'):
    setattr(AutomaticOTA, name, admin.AdminHTTP.__dict__[name])

if __name__ == '__main__':
    unittest.main()
