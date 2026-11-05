#!/usr/bin/env python3
"""Persistent catalog reads must avoid payload work without hiding mutations."""
import importlib.util
import http.client
import io
import tarfile
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import time
import unittest

spec=importlib.util.spec_from_file_location('admin',Path(__file__).with_name('test-admin.py'))
admin=importlib.util.module_from_spec(spec);spec.loader.exec_module(admin)

class Catalog(admin.AdminHTTP):
    def setUp(self):
        super().setUp()
        self.counter=self.root/'expensive-calls'
        wrappers=self.root/'tools';wrappers.mkdir()
        for name in ('sha256sum','fdtget','dumpimage'):
            actual=shutil.which(name,path=admin.ENV['PATH'])
            p=wrappers/name;p.write_text(f'#!/bin/sh\necho {name} >> "{self.counter}"\nexec "{actual}" "$@"\n');p.chmod(0o755)
        self.env=dict(admin.ENV,PATH=str(wrappers)+':'+admin.ENV['PATH'])
        self.restart()

    def restart(self):
        self.server.terminate();self.server.wait(timeout=10)
        self.log.seek(0);self.log.truncate()
        self.server=subprocess.Popen([str(admin.SOURCE/'device-server'),'--root',str(self.feed),'--port','0'],env=self.env,stdout=self.log,stderr=self.log)
        for _ in range(200):
            text=(self.root/'server.log').read_text()
            if text.startswith('Device Manager: '):
                self.port=int(text.split()[2].split(':')[2].split('/')[0]);return
            self.assertIsNone(self.server.poll(),text);time.sleep(.02)
        self.fail('Server did not restart')

    def seed_catalog(self):
        ident=self.draft();self.assertEqual(self.publish(ident)[0],200)
        self.state();self.counter.write_text('')
        return ident

    def test_refresh_and_restart_do_not_rehash(self):
        self.seed_catalog()
        for _ in range(3):self.assertEqual(self.state()['latest'],1)
        for endpoint in ('version','identity','info'):
            self.assertEqual(self.request('GET','/releases/1/'+endpoint)[0],200)
        self.assertEqual(self.counter.read_text(),'')
        self.restart();self.counter.write_text('')
        self.assertEqual(self.state()['latest'],1)
        self.assertEqual(self.counter.read_text(),'')
        with sqlite3.connect(self.feed/'firmware.db') as db:
            rows=db.execute('select tag,length(firmware_id),length(kernel_sha256),length(rootfs_sha256) from firmware_identity').fetchall()
        self.assertEqual(rows,[('v1.0.1',64,64,64),('v1.0.1',64,64,64)])

    def test_quarantine_is_live_with_warm_catalog(self):
        self.seed_catalog()
        with sqlite3.connect(self.feed/'firmware.db') as db:
            kernel,rootfs=db.execute('select kernel_sha256,rootfs_sha256 from firmware_identity limit 1').fetchone()
        quarantine=self.feed/'quarantine';quarantine.mkdir()
        (quarantine/(kernel+'-'+rootfs)).write_text('incident')
        state=self.state()
        self.assertTrue(all(x['quarantined'] for x in state['drafts']+state['releases']))
        self.assertEqual(self.counter.read_text(),'')
        self.assertEqual(self.publish(self.state()['drafts'][0]['id'])[0],409)

    def test_replaced_payload_invalidates_catalog_even_with_same_mtime(self):
        self.seed_catalog();payload=self.feed/'releases/1/kernel.itb'
        before=payload.stat();data=payload.read_bytes()
        payload.write_bytes(data[:-1]+bytes([data[-1]^1]))
        os.utime(payload,ns=(before.st_atime_ns,before.st_mtime_ns))
        self.assertEqual(self.request('GET','/api/state')[0],500)
        self.assertNotIn('sha256sum',self.counter.read_text())
        payload.write_bytes(data)
        self.assertEqual(self.state()['latest'],1)
        self.counter.write_text('');self.state();self.assertEqual(self.counter.read_text(),'')

    def test_single_active_publication_and_republish(self):
        first=self.seed_catalog()
        second=self.draft(self.bundle(self.fixture(version=65538)))
        self.assertEqual(self.publish(second)[0],200)
        state=self.state()
        self.assertEqual([x['publication'] for x in state['releases'] if x['published']],[2])
        self.assertFalse(next(x for x in state['drafts'] if x['id']==first)['published'])
        self.assertTrue(all(x['valid'] for x in state['releases']))
        self.assertEqual(self.publish(first)[0],200)
        state=self.state()
        self.assertEqual(state['latest'],3)
        self.assertEqual([x['publication'] for x in state['releases'] if x['published']],[3])
        self.assertEqual(self.publish(first)[0],200)
        self.assertEqual(self.state()['latest'],3)

    def test_download_counts_and_detail_route(self):
        self.seed_catalog()
        self.assertEqual(self.request('GET','/firmware?kind=release&id=1')[0],200)
        for name in ('release.itb','kernel.itb'):
            self.assertEqual(self.request('GET','/releases/1/'+name,headers={'X-OTA-Device':'board-01'})[0],200)
        self.assertEqual(self.state()['download_stats'],[])
        self.assertEqual(self.request('HEAD','/releases/1/rootfs.itb')[0],200)
        for headers in ({'X-OTA-Device':'board-01'},{'X-OTA-Device':'board-01'},{}):
            self.assertEqual(self.request('GET','/releases/1/rootfs.itb',headers=headers)[0],200)
        self.assertEqual(self.state()['download_stats'],[{'publication':1,'downloads':3,'identified_downloads':2,'unique_devices':1}])
        self.restart()
        self.assertEqual(self.state()['download_stats'][0]['downloads'],3)

    def test_admin_export_roundtrip_and_counters(self):
        ident=self.draft()
        connection=http.client.HTTPConnection('127.0.0.1',self.port,timeout=15)
        connection.request('GET','/api/download/draft/'+ident)
        response=connection.getresponse();archive=response.read()
        self.assertEqual(response.status,200)
        self.assertEqual(response.getheader('Content-Type'),'application/gzip')
        self.assertIn('firmware-v1.0.1-',response.getheader('Content-Disposition'))
        connection.close()
        with tarfile.open(fileobj=io.BytesIO(archive),mode='r:gz') as bundle:
            self.assertEqual(set(bundle.getnames()),set(self.files))
            for name,data in self.files.items():self.assertEqual(bundle.extractfile(name).read(),data)
        self.assertEqual(self.upload(archive)[0],201)
        self.assertEqual(self.publish(ident)[0],200)
        self.assertEqual(self.request('GET','/api/download/release/1')[0],200)
        self.assertEqual(self.request('HEAD','/api/download/release/1')[0],200)
        self.assertEqual(self.state()['download_stats'],[])
        newer=self.draft(self.bundle(self.fixture(version=65538)))
        self.assertEqual(self.publish(newer)[0],200)
        self.assertEqual(self.request('GET','/api/download/release/1')[0],200)

    def test_export_quarantine_and_invalid_paths(self):
        self.seed_catalog()
        with sqlite3.connect(self.feed/'firmware.db') as db:
            kernel,rootfs=db.execute('select kernel_sha256,rootfs_sha256 from firmware_identity limit 1').fetchone()
        quarantine=self.feed/'quarantine';quarantine.mkdir()
        (quarantine/(kernel+'-'+rootfs)).write_text('incident')
        self.assertEqual(self.request('GET','/api/download/release/1')[0],200)
        self.assertEqual(self.request('GET','/releases/1/rootfs.itb')[0],410)
        for path in ('release/999','release/01','release/1/extra','draft/../../etc/passwd','unknown/1'):
            self.assertEqual(self.request('GET','/api/download/'+path)[0],404,path)
        payload=self.feed/'releases/1/kernel.itb'
        payload.write_bytes(b'corrupt')
        self.assertEqual(self.request('GET','/api/download/release/1')[0],503)

    def test_existing_feed_backfills_once(self):
        self.seed_catalog();self.server.terminate();self.server.wait(timeout=10)
        for name in ('firmware.db','firmware.db-wal','firmware.db-shm'):
            (self.feed/name).unlink(missing_ok=True)
        self.restart();self.counter.write_text('')
        self.assertEqual(self.state()['latest'],1)
        self.assertNotIn('sha256sum',self.counter.read_text())
        self.counter.write_text('');self.state();self.assertEqual(self.counter.read_text(),'')

if __name__=='__main__':unittest.main()
