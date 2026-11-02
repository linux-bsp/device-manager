#!/usr/bin/env python3
"""Global quarantine, automatic confirmed rollback, durable audit and unique-device counts."""
import importlib.util
from pathlib import Path
import subprocess
import time
import unittest
spec = importlib.util.spec_from_file_location('admin', Path(__file__).with_name('test-admin.py'))
admin = importlib.util.module_from_spec(spec); spec.loader.exec_module(admin)
class Quarantine(unittest.TestCase):
    def setUp(self): admin.AdminHTTP.setUp(self)
    def report(self, publication, version, phase, device='board-a', reason='not-reported', boot='boot-one'):
        body=f'BSPOTASTATE2 {device} ti-am62x am62x-sd-ab-v1 {publication} {version} {boot} {phase} 1 65537 {reason}\n'
        status, response=self.request('POST','/api/devices/'+device,body,{'X-OTA-Request':'1'})
        self.assertEqual(status,200,response)
    def setup_releases(self):
        self.publish(self.draft())
        self.report(1,65537,'success',reason='none')
        self.bad=self.fixture(65538)
        self.publish(self.draft(self.bundle(self.bad)))
    def restart(self):
        self.server.terminate();self.server.wait(timeout=10)
        self.log.seek(0);self.log.truncate()
        self.server=subprocess.Popen([str(admin.SOURCE/'device-server'),'--root',str(self.feed),'--port',str(self.port)],env=admin.ENV,stdout=self.log,stderr=self.log)
        for _ in range(100):
            self.assertIsNone(self.server.poll(),(self.root/'server.log').read_text())
            if (self.root/'server.log').read_text().startswith('Device Manager: '):return
            time.sleep(.02)
        self.fail('restart timeout')
    def test_content_identity_is_stable_across_publications_and_matches_payloads(self):
        import hashlib
        self.setup_releases()
        status, identity = self.request('GET','/releases/1/identity')
        self.assertEqual(status,200)
        tokens=identity.decode().split()
        canonical=f'BSPFWCONTENT1\nkernel {tokens[3]} {tokens[4]}\nrootfs {tokens[5]} {tokens[6]}\n'
        expected=hashlib.sha256(canonical.encode()).hexdigest()
        status, label=self.request('GET','/releases/1/version')
        self.assertEqual(status,200)
        self.assertEqual(label.decode().strip(),f'BSPOTAVERSION1 1 v1.0.1 {expected}')
        self.report(2,65538,'rolled-back',reason='boot-failed-rolled-back')
        self.assertEqual(self.request('GET','/releases/3/version')[1].decode().strip(),f'BSPOTAVERSION1 3 v1.0.1 {expected}')
        self.assertNotEqual(self.request('GET','/releases/2/version')[1].decode().split()[-1],expected)

    def test_rollback_stops_all_downloads_and_republishes_confirmed_version(self):
        self.setup_releases()
        self.report(2,65538,'rolled-back',reason='boot-failed-rolled-back')
        state=self.state();self.assertEqual(state['latest'],3)
        self.assertEqual(next(r for r in state['releases'] if r['publication']==3)['firmware_version'],'1.0.1')
        self.assertTrue(next(r for r in state['releases'] if r['publication']==2)['quarantined'])
        self.assertEqual(self.request('GET','/releases/2/kernel.itb')[0],410)
        self.assertEqual(self.request('GET','/releases/2/rootfs.itb')[0],410)
        self.assertEqual(self.request('GET','/releases/3/kernel.itb')[0],200)
        draft=self.draft(self.bundle(self.bad))
        self.assertEqual(self.request('POST','/api/publish/'+draft,b'',{'X-OTA-Request':'1'})[0],409)
        events=state['audit_events']
        self.assertEqual({e['action'] for e in events},{'quarantined','distribution-stopped','fallback-selected','fallback-published'})
        self.assertTrue(all(len(e['kernel_sha256'])==64 and len(e['rootfs_sha256'])==64 for e in events))
        self.report(2,65538,'rolled-back',reason='boot-failed-rolled-back')
        self.restart()
        self.assertEqual(self.state()['latest'],3)
        self.assertEqual(len(self.state()['audit_events']),len(events))
    def test_restart_with_corrupt_old_quarantined_fit(self):
        self.setup_releases()
        self.report(2,65538,'rolled-back',reason='boot-failed-rolled-back')
        (self.feed/'releases/2/release.itb').write_bytes(b'corrupt old image')
        self.restart()
        self.assertEqual(self.request('GET','/healthz')[0],200)
        self.assertEqual(self.request('GET','/releases/3/kernel.itb')[0],200)
    def test_device_exception_has_separate_count_but_same_stop_and_fallback(self):
        self.setup_releases()
        self.report(2,65538,'success',device='board-success',reason='none')
        self.report(2,65538,'success',device='board-success',reason='none')
        self.report(2,65538,'device-error',device='board-error',reason='storage-io-error')
        self.report(2,65538,'rolled-back',device='board-rollback',reason='boot-failed-rolled-back')
        state=self.state();counts=next(r for r in state['release_stats'] if r['publication']==2)
        self.assertEqual(counts,{'publication':2,'total_devices':3,'success':1,'upgrade_failed':1,'device_errors':1,'in_progress':0,'up_to_date':0})
        self.assertEqual(state['latest'],3)
        self.assertEqual({e['category'] for e in state['audit_events']},{'upgrade-failure','device-error'})
        self.assertTrue(any(e['reason']=='storage-io-error' for e in state['audit_events']))
    def test_no_confirmed_version_leaves_distribution_stopped(self):
        self.publish(self.draft())
        self.report(1,65537,'install-failed',reason='fwctl-exit-74')
        self.assertEqual(self.state()['latest'],0)
        self.assertEqual(self.request('GET','/latest')[0],404)
        self.assertEqual(self.request('GET','/releases/1/release.itb')[0],410)
        self.assertIn('no-confirmed-fallback',{e['action'] for e in self.state()['audit_events']})
    def test_previous_quarantined_release_never_selected_for_fallback(self):
        self.setup_releases()
        self.report(1,65537,'device-error',device='late-device',reason='storage-io-error')
        self.assertEqual(self.state()['latest'],2)
        self.report(2,65538,'rolled-back',reason='boot-failed-rolled-back')
        self.assertEqual(self.state()['latest'],0)
    def test_legacy_reason_is_unknown_and_history_survives_restart(self):
        self.publish(self.draft())
        body='BSPOTASTATE1 old-board ti-am62x am62x-sd-ab-v1 1 65537 old-boot rolled-back 1 65536\n'
        self.assertEqual(self.request('POST','/api/devices/old-board',body,{'X-OTA-Request':'1'})[0],200)
        self.restart()
        self.assertTrue(all(e['reason']=='not-reported' for e in self.state()['audit_events']))
for name in ('tearDown','fixture','bundle','request','state','upload','draft','publish'):
    setattr(Quarantine,name,admin.AdminHTTP.__dict__[name])
if __name__=='__main__':unittest.main()
