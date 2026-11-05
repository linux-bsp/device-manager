#!/usr/bin/env python3
"""Deletion must preserve active firmware, transactions and historical evidence."""
import importlib.util
import json
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
            self.assertFalse((self.feed/'drafts'/ident).exists())
            self.assertFalse((self.feed/'drafts'/('.deleted-'+ident)/'kernel.itb').exists())
        with sqlite3.connect(self.feed/'firmware.db') as db:
            self.assertEqual(db.execute('select count(*) from firmware_deletions where status="deleted"').fetchone()[0],2)
        self.assertEqual(self.delete(first)[0],404)
    def test_active_cannot_be_deleted(self):
        ident=self.draft();self.assertEqual(self.publish(ident)[0],200)
        self.assertEqual(self.delete(ident)[0],409)
        self.assertTrue((self.feed/'drafts'/ident/'kernel.itb').exists())
        self.assertEqual(self.state()['latest'],1)
    def test_inflight_protected_then_history_retained(self):
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
        self.assertEqual(self.request('GET','/releases/1/kernel.itb'),(200,self.files['kernel.itb']))
    def test_csrf_and_path_rejected(self):
        ident=self.draft()
        self.assertEqual(self.request('DELETE','/api/firmware/'+ident)[0],403)
        self.assertEqual(self.delete(ident,{'Origin':'http://other-host'})[0],403)
        self.assertNotEqual(self.delete('../releases/1')[0],200)
        self.assertTrue((self.feed/'drafts'/ident).exists())
if __name__=='__main__':unittest.main()
