#!/usr/bin/env python3
"""Deletion must preserve active firmware, transactions and historical evidence."""
import importlib.util
import json
import subprocess
import time
from pathlib import Path
import sqlite3
import unittest
spec=importlib.util.spec_from_file_location('admin',Path(__file__).with_name('test-admin.py'))
admin=importlib.util.module_from_spec(spec);spec.loader.exec_module(admin)
class DeleteFirmware(unittest.TestCase):
    setUp=admin.AdminHTTP.setUp
    tearDown=admin.AdminHTTP.tearDown
    fixture=admin.AdminHTTP.fixture
    bundle=admin.AdminHTTP.bundle
    request=admin.AdminHTTP.request
    upload=admin.AdminHTTP.upload
    state=admin.AdminHTTP.state
    publish=admin.AdminHTTP.publish
    draft=admin.AdminHTTP.draft
    def delete(self,ident,headers=None):
        return self.request('DELETE','/api/firmware/'+ident,headers={'X-OTA-Request':'1',**(headers or {})})
    def test_delete_duplicate_uploads_and_audit(self):
        first=self.draft();second=self.draft()
        self.assertEqual(self.delete(first)[0],200)
        self.assertEqual(self.state()['drafts'],[])
        for ident in (first,second):
            self.assertEqual({p.name for p in (self.feed/'drafts'/ident).iterdir()}, {'manifest'})
            self.assertFalse((self.feed/'drafts'/('.deleted-'+ident)/'kernel.itb').exists())
        with sqlite3.connect(self.feed/'firmware.db') as db:
            self.assertEqual(db.execute('select count(*) from firmware_deletions where status="deleted"').fetchone()[0],2)
        self.assertEqual(self.delete(first)[0],200)
    def test_active_cannot_be_deleted(self):
        ident=self.draft();self.assertEqual(self.publish(ident)[0],200)
        self.assertEqual(self.delete(ident)[0],409)
        self.assertTrue((self.feed/'drafts'/ident/'kernel.itb').exists())
        self.assertEqual(self.state()['latest'],1)
    def test_inflight_protected_then_only_audit_retained(self):
        ident=self.draft();self.publish(ident)
        newer=self.draft(self.bundle(self.fixture(version=65538)));self.publish(newer)
        body='BSPOTASTATE3 board-one 1010001000110000 1 65537 boot-one installing 1 65536 not-reported\n'
        self.assertEqual(self.request('POST','/api/devices/board-one',body,{'X-OTA-Request':'1'})[0],200)
        self.assertEqual(self.delete(ident)[0],409)
        body=body.replace('installing','success').replace('1 65536','1 65537')
        self.assertEqual(self.request('POST','/api/devices/board-one',body,{'X-OTA-Request':'1'})[0],200)
        self.assertEqual(self.delete(ident)[0],200)
        self.assertEqual(len(self.state()['releases']),2)
        self.assertEqual(len(self.state()['device_history']),1)
        deleted=next(x for x in self.state()['releases'] if x['publication']==1)
        self.assertTrue(deleted['deleted']);self.assertFalse(deleted['valid'])
        self.assertEqual({p.name for p in (self.feed/'releases/1').iterdir()}, {'manifest'})
        for name in ('manifest','kernel.itb','rootfs.itb','release.itb','identity','info','version'):
            self.assertEqual(self.request('GET','/releases/1/'+name)[0],410,name)
        self.assertEqual(self.request('GET','/api/download/release/1')[0],404)
        self.assertEqual(self.request('GET','/api/download/draft/'+ident)[0],404)
        self.assertEqual(self.publish(ident)[0],410)
        self.assertEqual(self.request('GET',f'/recovery/1010001000110000/kernel/65537/{deleted["firmware_id"]}/kernel.itb')[0],404)
        # Late reports remain auditable, and deleted successes cannot be fallback.
        failed='BSPOTASTATE3 board-two 1010001000110000 2 65538 boot-two rolled-back 0 65537 boot-failed-rolled-back\n'
        self.assertEqual(self.request('POST','/api/devices/board-two',failed,{'X-OTA-Request':'1'})[0],200)
        self.assertEqual(self.state()['channels'],[])
        self.assertTrue(any(e['action']=='no-confirmed-fallback' for e in self.state()['audit_events']))
        late=body.replace('success','rolled-back').replace('not-reported','boot-failed-rolled-back')
        self.assertEqual(self.request('POST','/api/devices/board-one',late,{'X-OTA-Request':'1'})[0],200)
        self.restart()
        self.assertEqual(len(self.state()['device_history']),2)
        self.assertTrue(next(x for x in self.state()['releases'] if x['publication']==1)['deleted'])

    def restart(self):
        self.server.terminate();self.server.wait(timeout=10)
        self.server=subprocess.Popen([str(admin.SOURCE/'device-server'),'--root',str(self.feed),'--port',str(self.port)],env=admin.ENV,stdout=self.log,stderr=self.log)
        for _ in range(100):
            try:
                if self.request('GET','/healthz')[0]==200:return
            except OSError:pass
            self.assertIsNone(self.server.poll(),(self.root/'server.log').read_text());time.sleep(.03)
        self.fail('server restart timed out')
    def test_all_hwid_and_republished_copies_removed_by_release_id(self):
        ids=['1010001000110000','1010002000110000']
        old=self.draft(self.bundle(self.fixture(hwid=ids)));self.publish(old)
        self.publish(self.draft(self.bundle(self.fixture(version=65538,hwid=ids))))
        self.publish(old) # old package published a second time: releases 5 and 6
        self.publish(self.draft(self.bundle(self.fixture(version=65539,hwid=ids))))
        before=self.state();old_hash=next(x for x in before['drafts'] if x['id']==old)['firmware_id']
        copies=[x for x in before['releases'] if x['firmware_id']==old_hash]
        self.assertEqual(len(copies),4)
        self.assertEqual(self.delete('release-1')[0],200)
        for r in copies:
            self.assertEqual({p.name for p in (self.feed/'releases'/r['id']).iterdir()},{'manifest'})
        self.assertEqual(len([r for r in self.state()['releases'] if r['deleted']]),4)
        self.assertEqual(len(self.state()['channels']),2)
        # A fresh upload is a new artifact; old audit tombstones stay deleted.
        fresh=self.draft(self.bundle(self.fixture(hwid=ids)))
        self.assertEqual(self.publish(fresh)[0],200)
        self.assertEqual(self.state()['latest'],10)
        self.assertEqual(len([r for r in self.state()['releases'] if r['deleted']]),4)
    def test_restart_resumes_persisted_deletion_without_serving_remaining_bytes(self):
        ident=self.draft();self.publish(ident)
        self.publish(self.draft(self.bundle(self.fixture(version=65538))))
        self.assertEqual(self.delete(ident)[0],200)
        # Emulate a crash after durable intent, before unlink and completion.
        paths=[self.feed/'drafts'/ident,self.feed/'releases/1']
        for directory in paths:
            for name,data in self.files.items():(directory/name).write_bytes(data)
        with sqlite3.connect(self.feed/'firmware.db') as db:
            db.execute('UPDATE firmware_removals SET completed_at=0')
            db.execute('UPDATE firmware_deletions SET status="requested"')
        self.assertEqual(self.request('GET','/releases/1/rootfs.itb')[0],410)
        self.restart()
        for directory in paths:self.assertEqual({p.name for p in directory.iterdir()},{'manifest'})
        with sqlite3.connect(self.feed/'firmware.db') as db:
            self.assertEqual(db.execute('SELECT count(*) FROM firmware_removals WHERE completed_at>0').fetchone()[0],2)
            self.assertEqual(db.execute('SELECT DISTINCT peer_ip FROM firmware_removals').fetchall(),[('127.0.0.1',)])
    def test_csrf_and_path_rejected(self):
        ident=self.draft()
        self.assertEqual(self.request('DELETE','/api/firmware/'+ident)[0],403)
        self.assertEqual(self.delete(ident,{'Origin':'http://other-host'})[0],403)
        self.assertNotEqual(self.delete('../releases/1')[0],200)
        self.assertTrue((self.feed/'drafts'/ident).exists())
if __name__=='__main__':unittest.main()
