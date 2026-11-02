#!/usr/bin/env python3
"""Persistent request snapshots, transaction isolation and untrusted header validation."""
import importlib.util
import sqlite3
import unittest
from pathlib import Path
spec=importlib.util.spec_from_file_location('admin',Path(__file__).with_name('test-admin.py'))
admin=importlib.util.module_from_spec(spec);spec.loader.exec_module(admin)
class Context(unittest.TestCase):
    setUp=admin.AdminHTTP.setUp
    tearDown=admin.AdminHTTP.tearDown
    fixture=admin.AdminHTTP.fixture
    bundle=admin.AdminHTTP.bundle
    upload=admin.AdminHTTP.upload
    request=admin.AdminHTTP.request
    state=admin.AdminHTTP.state
    draft=admin.AdminHTTP.draft
    publish=admin.AdminHTTP.publish
    def headers(self,ip='192.0.2.10',hostname='am625-board',boot='boot-a'):
        return {'X-OTA-Device':'audit-board','X-OTA-IP':ip,'X-OTA-MAC':'1c:63:49:22:c4:f7',
                'X-OTA-Hostname':hostname,'X-OTA-Boot':boot,'X-OTA-Request':'1'}
    def report(self,phase,headers,boot='boot-a',publication=1):
        body=f'BSPOTASTATE2 audit-board ti-am62x am62x-sd-ab-v1 {publication} 65537 {boot} {phase} 1 65537 none\n'
        return self.request('POST','/api/devices/audit-board',body,headers)[0]
    def test_snapshots_and_repeated_requests_are_durable(self):
        self.publish(self.draft())
        first=self.headers()
        for _ in range(2):self.assertEqual(self.request('GET','/latest',headers=first)[0],200)
        self.assertEqual(self.report('installing',first),200)
        second=self.headers('2001:db8::20','renamed-board','boot-b')
        self.assertEqual(self.report('success',second),200)
        h=self.state()['device_history'][0];rows=h['network']
        self.assertEqual(len(rows),3)
        self.assertEqual(rows[0]['requests'],2)
        self.assertEqual(rows[0]['hostname'],'am625-board')
        self.assertEqual(rows[-1]['hostname'],'renamed-board')
        self.assertEqual(rows[-1]['ip'],'2001:db8::20')
        self.assertEqual(rows[-1]['request_boot'],'boot-b')
        self.assertTrue(all(r['peer_ip']=='127.0.0.1' for r in rows))
        # A separate transaction never rewrites or inherits the first one's facts.
        self.assertEqual(self.report('success',self.headers(hostname='other-board'),boot='other-transaction'),200)
        histories=self.state()['device_history']
        old=next(x for x in histories if x['source_boot']=='boot-a')
        self.assertEqual(old['network'],rows)
        with sqlite3.connect(self.feed/'firmware.db') as db:
            self.assertEqual(db.execute('select count(*) from device_requests').fetchone()[0],4)
    def test_legacy_report_has_explicitly_empty_context(self):
        self.assertEqual(self.report('success',{'X-OTA-Request':'1'}),200)
        self.assertEqual(self.state()['device_history'][0]['network'],[])
    def test_malformed_and_mismatched_context_rejected(self):
        for key,value in [('X-OTA-IP','not-an-ip'),('X-OTA-MAC','1:2:3'),('X-OTA-Hostname','<script>'),('X-OTA-Hostname','a'*254),('X-OTA-Boot','bad boot')]:
            with self.subTest(key=key):
                h=self.headers();h[key]=value
                self.assertEqual(self.request('GET','/latest',headers=h)[0],400)
        h=self.headers();del h['X-OTA-MAC']
        self.assertEqual(self.request('GET','/latest',headers=h)[0],400)
        h=self.headers();h['X-OTA-Device']='wrong-board'
        self.assertEqual(self.report('success',h),400)
    def test_request_without_active_release_is_recorded(self):
        self.assertEqual(self.request('GET','/latest',headers=self.headers())[0],404)
        with sqlite3.connect(self.feed/'firmware.db') as db:
            self.assertEqual(db.execute('select publication,hostname from device_requests').fetchone(),(0,'am625-board'))
if __name__=='__main__':unittest.main()
