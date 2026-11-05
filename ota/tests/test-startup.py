#!/usr/bin/env python3
"""Startup archive commit, durable retention and real uploader acknowledgement."""
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import time
import unittest
import urllib.request
import urllib.error
import zlib

ROOT=Path(__file__).resolve().parents[1]
STAGES=('r5-spl','a53-spl','uboot','kernel')
IDS=('11111111-1111-4111-8111-111111111111','22222222-2222-4222-8222-222222222222')
class Startup(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='startup-archive-');self.base=Path(self.tmp.name)
        self.feed=self.base/'feed';self.feed.mkdir();self.worker=None
        self.start()
    def start(self):
        self.server=subprocess.Popen([str(ROOT/'device-server'),'--root',str(self.feed),'--port','0','--sol-debug-no-auth'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        self.url=self.server.stdout.readline().split()[2].rstrip('/')
        self.http=urllib.request.build_opener(urllib.request.ProxyHandler({}))
    def stop(self):
        self.server.terminate();self.server.communicate(timeout=5)
    def tearDown(self):
        if self.worker:self.worker.terminate();self.worker.communicate(timeout=8)
        self.stop();self.tmp.cleanup()
    def request(self,path,data=None):
        try:r=self.http.open(urllib.request.Request(self.url+path,data=data),timeout=5)
        except urllib.error.HTTPError as e:r=e
        with r:return r.status,json.loads(r.read())
    def push(self,stage,data=b'',off=0,id=IDS[0]):
        return self.request(f'/api/sol/startup/device-a/{id}/{stage}/{off}',data)
    def manifest(self,data,current=1,recovery='recovery-boot',generation=1):
        value=f'0 {generation} {current} {recovery}\n'
        for stage in STAGES:
            b=data.get(stage,b'');value+=f'{stage} {len(b)} {zlib.crc32(b):08x} 0 1\n'
        return value.encode()
    def test_commit_requires_every_byte_and_crc(self):
        data={'kernel':b'panic\x00\n'};manifest=self.manifest(data)
        self.assertEqual(self.push('commit',manifest)[0],409)
        self.assertEqual(self.push('kernel',data['kernel']),(200,{'next':7}))
        self.assertEqual(self.push('commit',manifest.replace(b'kernel 7',b'kernel 8'))[0],409)
        self.assertEqual(self.push('commit',manifest),(200,{'next':1}))
        status,rows=self.request('/api/sol/logs/device-a/'+IDS[0]+'/all/0')
        self.assertEqual(status,200);self.assertEqual(rows[0]['text'].encode('latin1'),data['kernel']);self.assertTrue(rows[0]['raw'])
    def test_recovered_prefix_is_archived_with_explicit_partial_status(self):
        data={'kernel':b'last committed kernel prefix\n'}
        manifest=self.manifest(data).decode().splitlines()
        manifest[-1]=manifest[-1][:-1]+'3'
        self.assertEqual(self.push('kernel',data['kernel'])[0],200)
        self.assertEqual(self.push('commit',('\n'.join(manifest)+'\n').encode())[0],200)
        with sqlite3.connect(self.feed/'sol.db') as db:
            meta=db.execute('select meta from startup_boots').fetchone()[0]
            self.assertIn('3=recovered-prefix',meta)
            self.assertTrue(meta.rstrip().endswith('0 3'))
    def test_restart_retry_and_changed_recovery_context(self):
        data={'uboot':b'U-Boot\r\n'}
        self.push('uboot',data['uboot']);manifest=self.manifest(data)
        self.assertEqual(self.push('commit',manifest)[0],200)
        _,rows=self.request('/api/sol/devices')
        received=rows[0]['archive_received_at'];self.assertGreater(received,1577836800)
        self.stop();self.start()
        self.assertEqual(self.push('uboot',data['uboot']),(200,{'next':8}))
        self.assertEqual(self.push('commit',self.manifest(data,current=0,recovery='next-healthy-boot'))[0],200)
        _,rows=self.request('/api/sol/devices');self.assertEqual(rows[0]['archive_received_at'],received)
        self.assertEqual(self.push('commit',self.manifest(data,generation=2))[0],409)
        self.assertEqual(self.push('uboot',b'evil',8),(200,{'next':8}))
        with sqlite3.connect(self.feed/'sol.db') as db:
            self.assertEqual(db.execute('select data from startup_streams').fetchone()[0],data['uboot'])
            self.assertIn('recovery-boot=recovery-boot',db.execute('select meta from startup_boots').fetchone()[0])
    def test_live_retention_does_not_erase_archive(self):
        data={'r5-spl':b'SPL first\n'};self.push('r5-spl',data['r5-spl']);self.push('commit',self.manifest(data))
        for i in range(18):
            self.assertEqual(self.request(f'/api/sol/push/device-a/live-{i}/{i+1}/kernel/1/0',b'live\n')[0],200)
        _,rows=self.request('/api/sol/devices')
        self.assertEqual(len(rows),17);self.assertEqual(rows[-1]['boot'],IDS[0])
        _,rows=self.request('/api/sol/logs/device-a/'+IDS[0]+'/all/0')
        self.assertEqual(rows[0]['text'],'SPL first\n')
    def test_manifest_and_path_boundaries(self):
        for stage in ('../bad','invalid'):
            self.assertEqual(self.push(stage)[0],400)
        self.assertEqual(self.push('kernel',b'a'*4097)[0],400)
        self.assertEqual(self.push('commit',self.manifest({})+b'junk')[0],409)
        self.assertEqual(self.push('commit',b'0 1 1 boot\x00\n')[0],409)
        self.assertEqual(self.push('r5-spl',b'a',32737),(200,{'next':0}))
    def fake_banks(self,corrupt=False):
        cache=self.base/'handoff';cache.mkdir();(cache/'seal').touch();(cache/'ack').touch()
        text=''
        for slot,id in enumerate(IDS):
            text+=f'{slot} {id} {slot+1} {slot}\n'
            for stage in STAGES:
                data=(f'{stage}: boot {slot}\r\n').encode()
                if stage=='kernel':data+=b'panic-marker\n' if slot==0 else b'normal-kernel\n'
                (cache/f'bank{slot}-{stage}').write_bytes(data)
                crc=zlib.crc32(data)^(1 if corrupt and slot==1 and stage=='kernel' else 0)
                text+=f'{stage} {len(data)} {crc:08x} 0 1\n'
        (cache/'banks').write_text(text)
        store=self.base/'boots';(store/'current-boot').mkdir(parents=True);(store/'current-boot/order').write_text('1\n')
        conf=self.base/'sol.conf';conf.write_text(f'server={self.url}\nauth=none\ndevice=device-a\ninterface=eth0\n')
        health=self.base/'health'
        env=dict(os.environ,LOGCTL_RUN_DIR=str(self.base/'run'),LOGCTL_SOL_CONFIG=str(conf),LOGCTL_HANDOFF_DIR=str(cache),LOGCTL_BOOT_HEALTH=str(health))
        self.worker=subprocess.Popen([str(ROOT.parent/'logging/logctl'),'forward','--root',str(store)],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        return cache,health
    def test_uploader_waits_for_health_then_clears_both_only_after_commit(self):
        cache,health=self.fake_banks();time.sleep(.4)
        self.assertEqual((cache/'seal').read_text(),'');self.assertEqual((cache/'ack').read_text(),'')
        health.write_text('1\n')
        for _ in range(100):
            if (cache/'ack').read_text():break
            self.assertIsNone(self.worker.poll());time.sleep(.1)
        self.assertEqual((cache/'ack').read_text(),' '.join(IDS)+'\n')
        with sqlite3.connect(self.feed/'sol.db') as db:
            self.assertEqual(db.execute('select count(*) from startup_boots where committed=1').fetchone()[0],2)
            self.assertEqual(db.execute('select count(*) from startup_streams').fetchone()[0],8)
    def test_uploader_preserves_ram_when_snapshot_checksum_is_bad(self):
        cache,health=self.fake_banks(corrupt=True);health.write_text('1\n');time.sleep(5.7)
        self.assertEqual((cache/'ack').read_text(),'')
        with sqlite3.connect(self.feed/'sol.db') as db:
            self.assertEqual(db.execute('select count(*) from startup_boots where committed=1').fetchone()[0],1)
    def test_uploader_preserves_ram_when_network_is_unavailable(self):
        cache,health=self.fake_banks();self.stop();self.start()
        # Worker retains the now-closed original URL. Never ACK the reserved RAM.
        health.write_text('1\n');time.sleep(5.7)
        self.assertEqual((cache/'ack').read_text(),'')
if __name__=='__main__':unittest.main()
