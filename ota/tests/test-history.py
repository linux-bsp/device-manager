#!/usr/bin/env python3
"""Persistent upgrade transactions: HTTP retries, restart, migration and corruption."""
import importlib.util
import os
import subprocess
import time
import unittest
from pathlib import Path

spec=importlib.util.spec_from_file_location('admin', Path(__file__).with_name('test-admin.py'))
admin=importlib.util.module_from_spec(spec); spec.loader.exec_module(admin)

class History(unittest.TestCase):
    def setUp(self): admin.AdminHTTP.setUp(self)
    def report(self, phase, publication=1, boot='boot-one', device='board-one', version=65538):
        running=version if phase=='success' else 65537
        body=f'BSPOTASTATE3 {device} 1010001000110000 {publication} {version} {boot} {phase} 1 {running} not-reported\n'
        return body, self.request('POST','/api/devices/'+device,body,{'X-OTA-Request':'1'})
    def restart(self):
        self.server.terminate();self.server.wait(timeout=10)
        self.log.seek(0);self.log.truncate()
        self.server=subprocess.Popen([str(admin.SOURCE/'device-server'),'--root',str(self.feed),'--port',str(self.port)],env=admin.ENV,stdout=self.log,stderr=self.log)
        for _ in range(100):
            self.assertIsNone(self.server.poll(),(self.root/'server.log').read_text())
            if (self.root/'server.log').read_text().startswith('Device Manager: '): return
            time.sleep(.02)
        self.fail('Server did not restart')
    def test_multiple_upgrades_and_retries(self):
        for phase in ('installing','rebooting','boot-testing','success','success'):
            self.assertEqual(self.report(phase)[1][0],200)
        for phase in ('installing','install-failed'):
            self.assertEqual(self.report(phase,2,version=65539)[1][0],200)
        histories=sorted(self.state()['device_history'],key=lambda h:h['publication'])
        self.assertEqual(len(histories),2)
        self.assertEqual([e['phase'] for e in histories[0]['events']],['installing','rebooting','boot-testing','success'])
        self.assertEqual(histories[1]['phase'],'install-failed')
        self.assertFalse(histories[0]['imported'])
        self.assertEqual(self.state()['devices'][0]['publication'],2)
        self.restart(); self.assertEqual(sorted(self.state()['device_history'],key=lambda h:h['publication']),histories)
    def test_transaction_identity_and_rollback(self):
        for device,boot in [('board-one','boot-one'),('board-one','boot-two'),('board-two','boot-one')]:
            for phase in ('installing','rebooting','rolled-back'):
                self.assertEqual(self.report(phase,device=device,boot=boot)[1][0],200)
        histories=self.state()['device_history'];self.assertEqual(len(histories),3)
        self.assertTrue(all(h['phase']=='rolled-back' for h in histories))
    def test_migration_only_preserves_existing_snapshot(self):
        d=self.feed/'devices';d.mkdir()
        body='BSPOTASTATE3 legacy 1010001000110000 3 65538 original-boot rebooting 1 65537 not-reported\n'
        (d/'legacy').write_text(body);os.utime(d/'legacy',(1234567890,1234567890))
        self.restart();h=self.state()['device_history'][0]
        self.assertTrue(h['imported']);self.assertEqual(h['started'],1234567890)
        self.assertEqual(len(h['events']),1)
        self.assertEqual(self.report('success',3,'original-boot','legacy')[1][0],200)
        self.restart();h=self.state()['device_history'][0]
        self.assertEqual([e['phase'] for e in h['events']],['rebooting','success'])
    def test_replay_after_history_saved_before_latest(self):
        old,_=self.report('installing');new,_=self.report('rebooting')
        (self.feed/'devices/board-one').write_text(old)
        self.restart()
        self.assertEqual(self.report('rebooting')[1][0],200)
        self.assertEqual(len(self.state()['device_history'][0]['events']),2)
        self.assertEqual((self.feed/'devices/board-one').read_text(),new)
    def test_corruption_refuses_ack_and_preserves_latest(self):
        old,_=self.report('installing')
        (self.feed/'history/board-one/1-boot-one').write_text('broken history\n')
        self.assertEqual(self.report('rebooting')[1][0],500)
        self.assertEqual((self.feed/'devices/board-one').read_text(),old)
    def test_changed_target_version_cannot_rewrite_transaction(self):
        old,_=self.report('installing')
        self.assertEqual(self.report('rebooting',version=65540)[1][0],500)
        self.assertEqual((self.feed/'devices/board-one').read_text(),old)

for name in ('tearDown','fixture','bundle','request','state'):
    setattr(History,name,admin.AdminHTTP.__dict__[name])
if __name__=='__main__': unittest.main()
