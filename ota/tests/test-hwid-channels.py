#!/usr/bin/env python3
"""Exact hardware channels, artifact isolation and global content quarantine."""
import importlib.util
import json
from pathlib import Path
import subprocess
import time
import unittest

spec = importlib.util.spec_from_file_location('admin', Path(__file__).with_name('test-admin.py'))
admin = importlib.util.module_from_spec(spec)
spec.loader.exec_module(admin)
A, B = '1010001000110000', '1010002000110000'


class Channels(admin.AdminHTTP):
    def package(self, version=65537):
        return self.draft(self.bundle(self.fixture(version=version, hwid=[A, B])))

    def channel(self, hwid):
        return self.request('GET', '/latest/' + hwid, headers={'X-OTA-HWID': hwid})

    def report(self, pub, version, hwid, phase, device):
        text = f'BSPOTASTATE3 {device} {hwid} {pub} {version} boot-{pub} {phase} 1 65537 none\n'
        return self.request('POST', '/api/devices/' + device, text,
                            {'X-OTA-Request': '1', 'X-OTA-HWID': hwid})

    def restart(self):
        self.server.terminate()
        self.server.wait(timeout=10)
        self.log.seek(0)
        self.log.truncate()
        self.server = subprocess.Popen([str(admin.SOURCE/'device-server'), '--root', str(self.feed), '--port', str(self.port)], env=admin.ENV, stdout=self.log, stderr=self.log)
        for _ in range(100):
            self.assertIsNone(self.server.poll(), (self.root/'server.log').read_text())
            if (self.root/'server.log').read_text().startswith('Device Manager: '): return
            time.sleep(.02)
        self.fail('restart timeout')

    def test_package_declarations_drive_all_channels_and_other_packages_stay_independent(self):
        self.assertEqual(self.publish(self.package())[0], 200)
        self.assertTrue(self.channel(A)[1].startswith(f'BSPOTA2 1 {A}\n'.encode()))
        self.assertTrue(self.channel(B)[1].startswith(f'BSPOTA2 2 {B}\n'.encode()))
        two = self.draft(self.bundle(self.fixture(version=65538, hwid=A)))
        self.assertEqual(self.publish(two)[0], 200)
        state = self.state()
        self.assertEqual({x['hwid']: x['publication'] for x in state['channels']}, {A: 3, B: 2})
        self.assertEqual({x['publication'] for x in state['releases'] if x['published']}, {2, 3})
        self.assertNotIn('"product":', json.dumps(state))
        self.assertNotIn('"layout":', json.dumps(state))
        self.restart()
        self.assertEqual({x['hwid']: x['publication'] for x in self.state()['channels']}, {A: 3, B: 2})

    def test_manual_scope_override_is_rejected(self):
        draft = self.draft()
        self.assertEqual(self.request('POST', '/api/publish/'+draft+'/'+B, b'', {'X-OTA-Request':'1'})[0], 400)
        self.assertEqual(self.state()['releases'], [])
        self.assertEqual(self.request('POST', '/api/publish/'+draft, b'', {'X-OTA-Request':'1', 'X-OTA-HWID':B})[0], 200)
        self.assertEqual(self.channel(B)[0], 204)
        self.assertEqual(self.state()['channels'], [{'hwid': A, 'publication': 1}])

    def test_multi_hardware_publish_is_idempotent_for_the_whole_package(self):
        draft = self.package()
        self.assertEqual(json.loads(self.publish(draft)[1])['publication'], 1)
        self.assertEqual(json.loads(self.publish(draft)[1])['publication'], 1)
        self.assertEqual(len(self.state()['releases']), 2)
        # New payload copies shared among newly prepared HWID snapshots only.
        self.assertEqual((self.feed/'releases/1/kernel.itb').stat().st_ino,
                         (self.feed/'releases/2/kernel.itb').stat().st_ino)

    def test_failed_preparation_never_changes_any_channel(self):
        self.assertEqual(self.publish(self.package())[0], 200)
        newer = self.package(65538)
        (self.feed/'releases/4').mkdir()
        result = subprocess.run([str(admin.SOURCE/'device-server'),'publish','--root',str(self.feed),'--source',str(self.feed/'drafts'/newer),'--version','3'],env=admin.ENV,capture_output=True)
        self.assertNotEqual(result.returncode,0)
        self.assertEqual({x['hwid']:x['publication'] for x in self.state()['channels']}, {A:1,B:2})
        self.restart()
        self.assertTrue(self.channel(A)[1].startswith(b'BSPOTA2 1 '))
        self.assertTrue(self.channel(B)[1].startswith(b'BSPOTA2 2 '))

    def test_channel_commit_failure_preserves_all_previous_targets(self):
        self.publish(self.package())
        # Refuse an unexpected object instead of replacing a corrupt channel set.
        (self.feed/'channels/invalid').write_text('invalid')
        self.assertEqual(self.publish(self.package(65538))[0],409)
        self.assertTrue(self.channel(A)[1].startswith(b'BSPOTA2 1 '))
        self.assertTrue(self.channel(B)[1].startswith(b'BSPOTA2 2 '))
        (self.feed/'channels/invalid').unlink()
        self.restart()
        self.assertEqual({x['hwid']:x['publication'] for x in self.state()['channels']},{A:1,B:2})
        self.assertEqual(list(self.feed.glob('.channels-*')),[])

    def test_partial_current_package_can_be_published_to_its_complete_fixed_scope(self):
        draft = self.package()
        self.publish(draft)
        self.publish(self.draft(self.bundle(self.fixture(version=65538,hwid=A))))
        self.assertEqual(json.loads(self.publish(draft)[1])['publication'],4)
        self.assertEqual({x['hwid']:x['publication'] for x in self.state()['channels']}, {A:4,B:5})
        self.assertEqual(list(self.feed.glob('.channels-*')),[])

    def test_cross_hardware_queries_downloads_and_reports_are_rejected(self):
        self.publish(self.draft())
        self.assertEqual(self.request('GET', '/latest/' + B, headers={'X-OTA-HWID':''})[0], 400)
        self.assertEqual(self.request('GET', '/latest/' + A, headers={'X-OTA-HWID': B})[0], 403)
        self.assertEqual(self.request('GET', '/api/policy/wrong-board/1', headers={'X-OTA-HWID': B})[0], 403)
        for suffix in ('manifest', 'version', 'identity', 'info', 'kernel.itb', 'rootfs.itb', 'release.itb'):
            for method in ('GET', 'HEAD'):
                self.assertEqual(self.request(method, '/releases/1/' + suffix, headers={'X-OTA-HWID': B})[0], 403)
        self.assertEqual(self.report(1, 65537, B, 'success', 'wrong-board')[0], 403)
        self.assertEqual(self.state()['devices'], [])

    def test_release_cannot_claim_hardware_unsupported_by_its_components(self):
        files = self.fixture()
        fit = self.root/'unsupported.itb'
        fit.write_bytes(files['release.itb'])
        subprocess.run(['fdtput','-t','s',str(fit),'/configurations/conf-1','fw,hwids',A,B],env=admin.ENV,check=True)
        files['release.itb'] = fit.read_bytes()
        self.assertEqual(self.upload(self.bundle(files))[0],422)
        self.assertEqual(self.state()['drafts'],[])

    def test_bad_content_stops_all_matching_channels_and_uses_each_channels_good_history(self):
        good = self.package()
        self.publish(good)
        self.assertEqual(self.report(1, 65537, A, 'success', 'board-a')[0], 200)
        self.assertEqual(self.report(2, 65537, B, 'success', 'board-b')[0], 200)
        bad = self.package(65538)
        self.publish(bad)
        self.assertEqual(self.report(3, 65538, A, 'rolled-back', 'board-a')[0], 200)
        state = self.state()
        self.assertEqual({x['hwid'] for x in state['channels']}, {A, B})
        active = [x for x in state['releases'] if x['published']]
        self.assertEqual(len(active), 2)
        self.assertTrue(all(x['firmware_version'] == '1.0.1' for x in active))
        self.assertEqual({x['hwid'] for x in state['audit_events'] if x['action'] == 'fallback-published'}, {A, B})
        self.assertEqual(self.request('GET', '/releases/3/kernel.itb')[0], 410)
        self.assertEqual(self.request('GET', '/releases/4/kernel.itb', headers={'X-OTA-HWID': B})[0], 410)
        self.restart()
        self.assertEqual(len(self.state()['channels']), 2)

    def test_interrupted_withdrawal_resumes_fallback_after_restart(self):
        self.publish(self.draft())
        self.report(1, 65537, A, 'success', 'board-a')
        self.publish(self.draft(self.bundle(self.fixture(version=65538))))
        self.assertEqual(self.report(2, 65538, A, 'rolled-back', 'board-a')[0], 200)
        self.server.terminate()
        self.server.wait(timeout=10)
        # The durable withdrawal journal remains, but its channel switch was lost.
        (self.feed/'channels'/A).unlink()
        self.restart()
        self.assertEqual(self.channel(A)[0], 200)
        active = next(x for x in self.state()['releases'] if x['published'])
        self.assertEqual(active['firmware_version'], '1.0.1')


if __name__ == '__main__':
    unittest.main()
