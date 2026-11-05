#!/usr/bin/env python3
"""Real HTTP/agent integration; mock only fwctl storage and reboot. No hardware writes."""
import importlib.util
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import unittest
from transport_fixture import FaultProxy

spec = importlib.util.spec_from_file_location('admin_tests', Path(__file__).with_name('test-admin.py'))
admin = importlib.util.module_from_spec(spec)
spec.loader.exec_module(admin)

class AutomaticOTA(unittest.TestCase):
    def setUp(self):
        admin.AdminHTTP.setUp(self)
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        self.snapshot = self.root / 'snapshot'
        self.firmware_ids = {}
        self.write_snapshot('FWOTA1 0 0 255 65536 1 4 0 0 0\n')
        self.calls = self.root / 'calls'
        self.fail_marker = self.root / 'fail'
        fw = self.bin / 'fwctl'
        fw.write_text('#!/bin/sh\n' +
            'case "$1" in\n' +
            'ota-status) cat "$TEST_SNAPSHOT";;\n' +
            'ota-check) set -- "$2" $(cat "$TEST_SNAPSHOT"); target=$1; boot=$3; '
            'if [ "$boot" = 0 ]; then current=$6; else current=${10}; fi; '
            'if [ "$target" = "$current" ] && [ ! -e "$TEST_CORRUPT" ]; then echo "FWCHECK1 match"; else echo "FWCHECK1 different"; fi;;\n' +
            'update) echo install >> "$TEST_CALLS"; [ ! -e "$TEST_FAIL" ] || exit 1; '
            'set -- $(cat "$TEST_SNAPSHOT"); if [ "$2" = 0 ]; then '
            'echo "FWOTA2 0 0 1 $5 $6 $7 $8 $TEST_TARGET_VERSION 0 2 $TEST_TARGET_ID" > "$TEST_SNAPSHOT"; '
            'else echo "FWOTA2 1 1 0 $TEST_TARGET_VERSION 0 2 $TEST_TARGET_ID ${9} ${10} ${11} ${12}" > "$TEST_SNAPSHOT"; fi;;\n' +
            '*) exit 2;;\nesac\n')
        fw.chmod(0o755)
        reboot = self.bin / 'reboot'
        reboot.write_text('#!/bin/sh\necho reboot >> "$TEST_CALLS"\n')
        reboot.chmod(0o755)
        self.env = dict(admin.ENV, PATH=str(self.bin)+':'+admin.ENV['PATH'],
            TEST_CORRUPT=str(self.root/'corrupt'), TEST_SNAPSHOT=str(self.snapshot), TEST_CALLS=str(self.calls), TEST_FAIL=str(self.fail_marker),
            TEST_ACTIVE_ID='a'*64, TEST_TARGET_ID='b'*64, TEST_TARGET_VERSION='65537')
        self.publish(self.draft())
        self.proxy = FaultProxy(self.port)
        self.addCleanup(self.proxy.close)

    def write_snapshot(self, status):
        if status.startswith('FWOTA1 '):
            fields = status.split()
            identities = []
            for index, fallback in ((4, 'a'), (7, 'b')):
                version = int(fields[index])
                identities.append('-' if not version else self.firmware_ids.get(version, fallback*64))
            status = 'FWOTA2 ' + ' '.join(fields[1:7] + [identities[0]] + fields[7:10] + [identities[1]]) + '\n'
        self.snapshot.write_text(status)

    def publish(self, ident):
        status, body = admin.AdminHTTP.publish(self, ident)
        if status == 200:
            publication = json.loads(body)['publication']
            version = self.request('GET', f'/releases/{publication}/info')[1].decode().split()[2]
            firmware_id = self.request('GET', f'/releases/{publication}/version')[1].decode().split()[3]
            self.firmware_ids[int(version)] = firmware_id
            self.env['TEST_TARGET_ID'] = firmware_id
            self.env['TEST_TARGET_VERSION'] = version
        return status, body

    def agent(self, ok=True, force=False, extra=()):
        r = subprocess.run([str(admin.SOURCE / 'fwctld'), '--server', f'http://127.0.0.1:{self.proxy.port}',
            '--cache', str(self.root/'cache'), '--hwid', '1010001000110000',
            '--state-dir', str(self.root/'state'),
            '--auto-install', '--device', 'board-01', '--once'] + (['--force'] if force else []) + list(extra), env=self.env, capture_output=True, text=True)
        if ok: self.assertEqual(r.returncode, 0, r.stdout+r.stderr)
        else: self.assertNotEqual(r.returncode, 0, r.stdout+r.stderr)
        return r

    def test_logs_visible_during_download_and_install(self):
        console=self.root/'console';console.touch()
        log=self.root/'agent.log'
        download_wait=self.root/'download-wait';download_go=self.root/'download-go'
        install_wait=self.root/'install-wait';install_go=self.root/'install-go'
        self.proxy.add(self.root/'pause-download', suffix='/releases/1/kernel.itb',
                       seen=download_wait, wait=download_go)
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
            proc=subprocess.Popen([str(admin.SOURCE/'fwctld'),'--server',f'http://127.0.0.1:{self.proxy.port}',
                '--cache',str(self.root/'cache'),'--hwid', '1010001000110000',
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

    def journal_directory(self):
        return self.root/'state/1010001000110000/board-01'

    def assert_reboot_queued(self):
        self.assertEqual(self.phase(), 'installing')
        record=(self.journal_directory()/'transaction').read_text()
        self.assertIn(' rebooting ',record)
        self.assertTrue(list(self.journal_directory().glob('*.event')))

    def disconnect_server(self):
        return self.proxy.add(self.root/'offline', drop=True)

    def reject_payload_requests(self):
        self.unexpected_download=self.root/'unexpected-download'
        self.proxy.add(self.root/'reject-payload',suffix='.itb',seen=self.unexpected_download,drop=True)

    def test_auto_success_waits_for_confirmation(self):
        self.agent()
        self.assertEqual(self.calls.read_text(), 'install\nreboot\n')
        self.assert_reboot_queued()
        self.write_snapshot('FWOTA1 1 0 1 65536 1 4 65537 0 3\n')
        shutil.rmtree(self.root/'cache')
        boot_result=self.agent()
        self.assertEqual(self.phase(), 'boot-testing')
        self.assertNotIn(': boot-testing',boot_result.stdout)
        self.write_snapshot('FWOTA1 1 1 255 65536 1 4 65537 1 4\n')
        self.reject_payload_requests()
        result=self.agent()
        self.assertEqual(self.phase(), 'success')
        self.assertNotIn('SHA256 match',result.stdout)
        self.assertNotIn(': success',result.stdout)
        self.assertNotIn('Ready: fwctl update',result.stdout)
        self.agent()
        self.assertFalse(self.unexpected_download.exists())
        self.assertFalse((self.root/'cache/1010001000110000/1').exists())
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
        self.write_snapshot('FWOTA1 1 1 255 65536 1 4 65537 1 4\n')
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

    def inject_download_failure(self, mode):
        if mode == 'storage':
            # Fail a real fdatasync in the checkpoint path; production has no test hook.
            preload=self.root/'fault.so'
            subprocess.run(['cc','-Wall','-Wextra','-Werror','-shared','-fPIC',
                str(admin.SOURCE.parent/'tests/fixtures/sync_fault.c'),'-ldl','-o',str(preload)],check=True)
            marker=self.root/'storage-fail';marker.touch()
            self.env.update(LD_PRELOAD=str(preload),DM_TEST_SYNC_PATH=str(self.root/'cache'),
                DM_TEST_SYNC_SUFFIX='kernel.itb.part',DM_TEST_SYNC_MARKER=str(marker),DM_TEST_SYNC_ERRNO='28')
            return marker
        return self.proxy.add(self.root/'download-fail',suffix='/releases/1/kernel.itb',
                              **{mode:True})

    def test_interrupted_download_is_retryable_and_never_quarantines_firmware(self):
        wrapper = self.inject_download_failure('partial')
        self.agent(ok=False)
        self.assertEqual(self.phase(), 'transport-error')
        history = self.state()['device_history'][0]
        self.assertEqual(history['category'], 'transport-error')
        self.assertEqual(history['reason'], 'download-interrupted')
        self.assertEqual(self.state()['latest'], 1)
        self.assertEqual(self.state()['audit_events'], [])
        self.assertFalse(self.calls.exists())
        partial = self.root/'cache/1010001000110000/.download-1/kernel.itb.part'
        self.assertEqual(partial.read_bytes(), self.files['kernel.itb'][:len(self.files['kernel.itb'])//2])
        wrapper.unlink()
        result = self.agent()
        self.assertIn('Resuming kernel.itb at byte', result.stdout)
        self.assert_reboot_queued()
        self.assertEqual(self.calls.read_text(), 'install\nreboot\n')
        self.assertEqual(self.state()['latest'], 1)

    def test_download_storage_failure_remains_a_device_error(self):
        self.inject_download_failure('storage')
        self.agent(ok=False)
        self.assertEqual(self.phase(), 'device-error')
        self.assertEqual(self.state()['device_history'][0]['reason'], 'cache-storage-failed')
        self.assertEqual(self.state()['latest'], 0)
        self.assertFalse(self.calls.exists())

    def test_download_hash_mismatch_is_an_image_failure(self):
        self.inject_download_failure('corrupt')
        self.agent(ok=False)
        self.assertEqual(self.phase(), 'verify-failed')
        self.assertEqual(self.state()['device_history'][0]['reason'], 'download-sha256-mismatch')
        self.assertEqual(self.state()['latest'], 0)
        self.assertFalse(self.calls.exists())

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
        self.write_snapshot('FWOTA1 0 0 255 65536 1 4 65537 0 5\n')
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
        (self.feed/'channels/1010001000110000').unlink()
        result = self.agent()
        self.assertIn('No active OTA publication; waiting', result.stdout)
        self.assertNotIn('failed', result.stdout + result.stderr)
        self.assertFalse(self.calls.exists())

    def test_failed_content_globally_blocked_even_when_reuploaded(self):
        self.agent()
        self.write_snapshot('FWOTA1 0 0 255 65536 1 4 65537 0 5\n')
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
        self.write_snapshot('FWOTA1 0 0 255 65536 1 4 65537 0 5\n')
        self.agent()
        self.publish(self.draft(self.bundle(self.fixture(65538))))
        fw = self.bin/'fwctl'
        fw.write_text(fw.read_text().replace('65537 0 2', '65538 0 2'))
        self.agent()
        self.assert_reboot_queued()
        self.assertEqual(self.state()['devices'][0]['publication'], 2)
        status, body = self.request('GET', '/api/policy/board-01/1')
        self.assertEqual(status, 200)
        self.assertIn(b'blocked', body)

    def test_new_publication_downloads_after_success_with_empty_cache(self):
        self.agent()
        self.write_snapshot('FWOTA1 1 1 255 65536 1 4 65537 1 4\n')
        shutil.rmtree(self.root/'cache')
        self.agent()
        self.assertEqual(self.phase(),'success')
        self.assertFalse((self.root/'cache/1010001000110000/1').exists())
        self.publish(self.draft(self.bundle(self.fixture(65538))))
        fw=self.bin/'fwctl'
        self.agent()
        self.assert_reboot_queued()
        self.assertEqual(self.state()['devices'][0]['publication'],2)
        self.assertTrue((self.root/'cache/1010001000110000/2/rootfs.itb').exists())
        self.assertEqual(self.calls.read_text(),'install\nreboot\ninstall\nreboot\n')

    def test_auto_existing_version_not_installed(self):
        self.write_snapshot('FWOTA1 0 0 255 65537 1 4 0 0 0\n')
        self.agent()
        self.assertFalse(self.state()['devices'])
        self.assertFalse((self.root/'cache/1010001000110000/1').exists())
        self.assertFalse(self.calls.exists())

    def test_offline_boot_and_confirmation_replay_all_events_after_cache_loss(self):
        self.agent();self.assert_reboot_queued()
        wrapper=self.disconnect_server()
        shutil.rmtree(self.root/'cache')
        self.write_snapshot('FWOTA1 1 0 1 65536 1 4 65537 0 3\n')
        self.agent();self.assertIn(' boot-testing ',(self.journal_directory()/'transaction').read_text())
        self.write_snapshot('FWOTA1 1 1 255 65536 1 4 65537 1 4\n')
        self.agent();self.assertIn(' success ',(self.journal_directory()/'transaction').read_text())
        self.assertEqual(self.calls.read_text(),'install\nreboot\n')
        wrapper.unlink();self.agent()
        self.assertEqual(self.phase(),'success')
        self.assertFalse(list(self.journal_directory().glob('*.event')))
        phases=[e['phase'] for e in self.state()['device_history'][0]['events']]
        self.assertEqual(phases,['installing','rebooting','boot-testing','success'])

    def test_offline_rollback_is_queued_and_never_reinstalled(self):
        self.agent();wrapper=self.disconnect_server()
        self.write_snapshot('FWOTA1 0 0 255 65536 1 4 65537 0 5\n')
        self.agent();self.agent()
        self.assertIn(' rolled-back ',(self.journal_directory()/'transaction').read_text())
        self.assertEqual(self.calls.read_text(),'install\nreboot\n')
        wrapper.unlink();self.agent()
        self.assertEqual(self.phase(),'rolled-back');self.assertEqual(self.state()['latest'],0)

    def test_persistent_cache_cleanup_waits_for_health_and_report_ack(self):
        self.agent()
        store=self.root/'persistent';cache=store/'1010001000110000';cache.mkdir(parents=True)
        for number in range(20,26):
            directory=cache/str(number);directory.mkdir()
            manifest=f'BSPOTA2 {number} 1010001000110000\n'
            for name in ('release.itb','kernel.itb','rootfs.itb'):
                (directory/name).write_text('data')
                manifest+=f'{name} 4 '+ 'a'*64+'\n'
            (directory/'manifest').write_text(manifest)
        shutil.copyfile(cache/'25/manifest',cache/'current')
        partial=cache/'.download-26';partial.mkdir();(partial/'kernel.itb').write_text('partial')
        extra=['--persistent-cache',str(store)]
        self.write_snapshot('FWOTA1 1 0 1 65536 1 4 65537 0 3\n')
        self.agent(extra=extra);self.assertTrue((cache/'20').exists())
        wrapper=self.disconnect_server()
        self.write_snapshot('FWOTA1 1 1 255 65536 1 4 65537 1 4\n')
        self.agent(extra=extra);self.assertTrue((cache/'20').exists())
        wrapper.unlink();result=self.agent(extra=extra)
        self.assertIn('Reclaimed',result.stdout)
        self.assertEqual(sorted(p.name for p in cache.iterdir()),['.download-26','.lock','24','25','current'])
        self.assertEqual((partial/'kernel.itb').read_text(),'partial')
        self.assertIn(' success ',(self.journal_directory()/'transaction').read_text())

    def test_corrupt_local_journal_blocks_writes_and_preserves_evidence(self):
        self.agent();path=self.journal_directory()/'transaction'
        original=path.read_bytes();path.write_bytes(original[:-2]+b'Z\n')
        events=list(self.journal_directory().glob('*.event'))
        result=self.agent(ok=False);self.assertIn('damaged or unreadable',result.stdout+result.stderr)
        self.assertEqual(self.calls.read_text(),'install\nreboot\n')
        self.assertEqual(list(self.journal_directory().glob('*.event')),events)
        self.assertEqual(self.state()['latest'],0)
        self.assertEqual(self.state()['device_history'][0]['reason'],'data-ota-state-corrupt')

    def test_unavailable_local_state_never_starts_flash_write(self):
        (self.root/'state').write_text('not a directory')
        result=self.agent(ok=False);self.assertIn('automatic writes deferred',result.stdout+result.stderr)
        self.assertFalse(self.calls.exists());self.assertEqual(self.state()['latest'],1)

    def test_lost_state_store_reports_only_a_data_failure_for_known_transaction(self):
        self.agent();(self.root/'state').rename(self.root/'saved-state')
        (self.root/'state').write_text('not a directory')
        self.agent(ok=False)
        self.assertEqual(self.phase(),'device-error')
        self.assertEqual(self.state()['device_history'][0]['reason'],'data-ota-state-unavailable')
        self.assertEqual(self.calls.read_text(),'install\nreboot\n')
        self.assertEqual(self.state()['latest'],0)

    def test_report_endpoint_failure_does_not_gate_install_or_reboot(self):
        wrapper=self.proxy.add(self.root/'report-fail',suffix='/api/devices/board-01',method='POST',drop=True)
        self.agent();self.assertEqual(self.calls.read_text(),'install\nreboot\n')
        self.assertFalse(self.state()['devices'])
        self.assertIn(' rebooting ',(self.journal_directory()/'transaction').read_text())
        self.write_snapshot('FWOTA1 1 1 255 65536 1 4 65537 1 4\n')
        wrapper.unlink();self.agent();self.assertEqual(self.phase(),'success')
        self.assertEqual([e['phase'] for e in self.state()['device_history'][0]['events']],
                         ['installing','rebooting','success'])

    def test_withdrawal_after_download_prevents_flash_transaction(self):
        self.proxy.add(self.root/'withdraw',suffix='/rootfs.itb',withdraw=self.feed/'channels/1010001000110000')
        self.agent(ok=False);self.assertFalse(self.calls.exists())

    def test_stopping_during_flash_is_recorded_without_quarantining_firmware(self):
        waiting=self.root/'flash-waiting'
        fw=self.bin/'fwctl';fw.write_text(fw.read_text().replace('update) echo install',
            f'update) touch "{waiting}"; while :; do sleep .1; done; echo install'))
        process=subprocess.Popen([str(admin.SOURCE/'fwctld'),'--server',f'http://127.0.0.1:{self.proxy.port}',
            '--cache',str(self.root/'cache'),'--state-dir',str(self.root/'state'),
            '--hwid','1010001000110000','--auto-install','--device','board-01','--once'],
            env=self.env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        try:
            for _ in range(150):
                if waiting.exists():break
                time.sleep(.02)
            self.assertTrue(waiting.exists())
            process.terminate();self.assertEqual(process.wait(timeout=10),0)
        finally:
            if process.poll() is None:process.kill();process.wait()
        self.assertIn(' cancelled ',(self.journal_directory()/'transaction').read_text())
        self.assertEqual(self.state()['latest'],1)
        self.assertEqual(self.state()['audit_events'],[])
        self.assertFalse(self.calls.exists())

    def test_report_replay_refuses_a_replaced_server_content_identity(self):
        self.agent()
        self.write_snapshot('FWOTA1 1 1 255 65536 1 4 65537 1 4\n')
        wrapper=self.proxy.add(self.root/'wrong-identity',suffix='/releases/1/version',
            response=b'BSPOTAVERSION1 1 v1.0.1 '+b'c'*64+b'\n')
        self.agent();self.assertEqual(self.phase(),'installing')
        self.assertTrue(list(self.journal_directory().glob('*.event')))
        wrapper.unlink();self.agent();self.assertEqual(self.phase(),'success')

    def test_matching_version_corrupt_contents_reinstalled(self):
        self.write_snapshot('FWOTA1 0 0 255 65537 1 4 0 0 0\n')
        (self.root/'corrupt').touch()
        self.agent()
        self.assertEqual(self.calls.read_text(), 'install\nreboot\n')

    def test_force_reinstalls_matching_image_and_refreshes_cache(self):
        self.write_snapshot('FWOTA1 0 0 255 65537 1 4 0 0 0\n')
        # Seed a corrupt payload cache to ensure forced recovery downloads again.
        cache = self.root/'cache/1010001000110000/1'
        cache.mkdir(parents=True)
        (cache/'rootfs.itb').write_bytes(b'broken cache')
        result = self.agent(force=True)
        self.assertIn('Force recovery requested', result.stdout)
        self.assertIn('Downloading rootfs.itb', result.stdout)
        self.assertEqual(self.calls.read_text(), 'install\nreboot\n')
        self.assertEqual((cache/'rootfs.itb').read_bytes(), self.files['rootfs.itb'])

    def test_force_cannot_overwrite_pending_deployment(self):
        self.write_snapshot('FWOTA1 0 0 1 65536 1 4 65538 0 2\n')
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
        self.write_snapshot('FWOTA1 2 0 255 65536 1 4 0 0 0\n')
        result=self.agent(ok=False)
        self.assertIn('local firmware status',result.stdout)
        self.assertFalse(self.calls.exists())

    def test_auto_journal_read_error_blocks_install(self):
        (self.feed/'devices').mkdir()
        (self.feed/'devices/board-01').write_text('corrupt state')
        self.agent(ok=False)
        self.assertFalse(self.calls.exists())

    def test_auto_pending_manual_update_not_overwritten(self):
        self.write_snapshot('FWOTA1 0 0 1 65536 1 4 65538 0 2\n')
        self.agent()
        self.assertFalse(self.calls.exists())

    def test_auto_interrupted_before_write(self):
        boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        body = f'BSPOTASTATE3 board-01 1010001000110000 1 65537 {boot} installing 1 65536 not-reported\n'
        self.assertEqual(self.request('POST', '/api/devices/board-01', body, {'X-OTA-Request':'1'})[0], 200)
        self.agent()
        self.assertEqual(self.phase(), 'interrupted')
        self.assertFalse(self.calls.exists())
        self.assertEqual(self.state()['latest'], 0)
        self.assertIn('interrupted', list((self.feed/'history/board-01').iterdir())[0].read_text())

    def test_report_validation_and_persistence(self):
        self.assertEqual(self.request('GET', '/api/devices/new-board')[0], 204)
        for ident, body in [('..','bad'), ('board-01','bad'),
            ('board-01','BSPOTASTATE3 other 1010001000110000 1 65537 boot success 1 65537 not-reported\n'),
            ('board-01','BSPOTASTATE3 board-01 1010001000110000 1 4294967297 boot success 1 65537 not-reported\n')]:
            self.assertEqual(self.request('POST', '/api/devices/'+ident, body, {'X-OTA-Request':'1'})[0], 400)
        self.agent()
        self.assertTrue((self.feed/'devices/board-01').is_file())
        self.assertEqual(self.request('GET','/api/devices/board-01')[0], 200)

    def kernel_bundle(self, version, payload='kernel'):
        dts = f'/dts-v1/; / {{ description="{payload}"; configurations {{ default="c"; c {{ fw,hwids="1010001000110000"; fw,version=<{version}>; }}; }}; }};'
        kernel = subprocess.run(['dtc','-I','dts','-O','dtb'], input=dts.encode(),capture_output=True,check=True,env=admin.ENV).stdout
        files = self.fixture(version=version)
        oldhash = hashlib.sha256(files['kernel.itb']).hexdigest().encode()
        newhash = hashlib.sha256(kernel).hexdigest().encode()
        files['release.itb'] = files['release.itb'].replace(oldhash, newhash)
        files['kernel.itb'] = kernel
        return self.bundle(files), kernel

    def test_rootfs_recovery_uses_exact_payload_hash(self):
        digest = hashlib.sha256(bytes(range(256)) * 1100).hexdigest()
        publication = self.state()['latest']
        image = self.request('GET', f'/releases/{publication}/identity')[1].decode().split()
        version = int(image[2]); kernel_bytes, kernel_hash = image[3], image[4]
        root_bytes, root_hash = image[5], image[6]
        firmware_id = hashlib.sha256(
            f'BSPFWCONTENT1\nkernel {kernel_bytes} {kernel_hash}\nrootfs {root_bytes} {root_hash}\n'.encode()
        ).hexdigest()
        path = f'/recovery/1010001000110000/rootfs/{version}/{firmware_id}/{digest}/rootfs.itb'
        self.assertEqual(self.request('GET', path), (200, self.files['rootfs.itb']))
        self.assertEqual(self.request('GET', path.replace(digest, '0'*64))[0], 404)
        self.assertEqual(self.request('GET', path.replace('1010001000110000', '1010002000110000'))[0], 404)
        self.assertEqual(self.request('GET', path+'/extra')[0], 404)
        self.assertEqual(self.request('GET', path.replace(digest, 'x'*64))[0], 404)

    def test_recovery_exact_version_and_ambiguity(self):
        bundle, kernel = self.kernel_bundle(65538)
        self.assertEqual(self.publish(self.draft(bundle))[0], 200)
        image = self.request('GET', '/releases/2/identity')[1].decode().split()
        firmware_id = hashlib.sha256(
            f'BSPFWCONTENT1\nkernel {image[3]} {image[4]}\nrootfs {image[5]} {image[6]}\n'.encode()
        ).hexdigest()
        path=f'/recovery/1010001000110000/kernel/65538/{firmware_id}/kernel.itb'
        self.assertEqual(self.request('GET',path), (200,kernel))
        self.assertEqual(self.request('GET',path.replace('/65538/','/65539/'))[0],404)
        self.assertEqual(self.request('GET',path.replace('1010001000110000','1010002000110000'))[0],404)
        self.assertEqual(self.request('GET','/releases/2/info'), (200,b'BSPOTAINFO1 2 65538\n'))
        self.assertEqual(self.request('GET',path+'/extra')[0],404)
        # Same version with different bytes gets a different content ID.
        other, _ = self.kernel_bundle(65538, 'changed')
        self.assertEqual(self.publish(self.draft(other))[0],200)
        self.assertEqual(self.request('GET',path), (200,kernel))

for name in ('tearDown', 'fixture', 'bundle', 'request', 'upload', 'state', 'draft'):
    setattr(AutomaticOTA, name, admin.AdminHTTP.__dict__[name])

if __name__ == '__main__':
    unittest.main()
