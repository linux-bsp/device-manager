#!/usr/bin/env python3
"""Isolated history deletion and late upload races; never production logs."""
import importlib.util
import json
import contextlib
import os
import subprocess
import time
from pathlib import Path
import sqlite3
import unittest
import urllib.request
import urllib.error
import zlib
spec=importlib.util.spec_from_file_location('sol_test',Path(__file__).with_name('test-sol.py'))
base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)
class Delete(unittest.TestCase):
    setUp=base.SOL.setUp
    tearDown=base.SOL.tearDown
    enroll=base.SOL.enroll
    start=base.SOL.start
    stop=base.SOL.stop
    request=base.SOL.request
    push=base.SOL.push
    @contextlib.contextmanager
    def database(self):
        db=sqlite3.connect(self.feed/'sol.db')
        try:
            with db:yield db
        finally:db.close()
    def remove(self,boot='boot-a',key=None,headers=None,body=None):
        h={'X-OTA-Request':'1','Authorization':'Bearer '+(self.viewer if key is None else key)};h.update(headers or {})
        r=urllib.request.Request(self.url+'/api/sol/records/device-a/'+boot,data=body,method='DELETE',headers=h)
        try:r=self.http.open(r,timeout=5)
        except urllib.error.HTTPError as e:r=e
        with r:
            body=r.read()
            try:value=json.loads(body)
            except json.JSONDecodeError:value=body.decode()
            return r.status,value
    def old_live(self):
        self.push(body=b'current=no\n',stage='meta',gen=0);self.assertEqual(self.push(body=b'hello')[0],200)
    def archive(self):
        data=b'SPL preserved\n';boot='capture-a'
        p='/api/sol/startup/device-a/'+boot+'/'
        self.assertEqual(self.request(p+'r5-spl/0',data,key=self.key)[0],200)
        manifest=f'0 1 0 recovery-a\nr5-spl {len(data)} {zlib.crc32(data):08x} 0 1\na53-spl 0 00000000 0 0\nuboot 0 00000000 0 0\nkernel 0 00000000 0 0\n'.encode()
        self.assertEqual(self.request(p+'commit/0',manifest,key=self.key)[0],200)
        return p,data,manifest
    def test_live_delete_retries_do_not_recreate_logs_or_break_cursors(self):
        self.old_live();self.assertEqual(self.remove(),(200,{'deleted':True}))
        self.assertEqual(self.request('/api/sol/logs/device-a/boot-a/all/0')[0],410)
        self.assertEqual(self.push(body=b''),(200,{'next':5}))
        self.assertEqual(self.push(body=b'hello'),(200,{'next':5}))
        self.assertEqual(self.push(body=b'new',offset=5),(200,{'next':8}))
        self.assertEqual(self.push(body=b'current=no\n',stage='meta',gen=0)[0],200)
        self.assertEqual(self.request('/api/sol/devices')[1],[])
        self.assertEqual(self.push(boot='boot-new',order=2)[0],200)
        self.assertEqual(len(self.request('/api/sol/devices')[1]),1)
    def test_archived_delete_survives_lost_ack_and_restart(self):
        p,data,manifest=self.archive();self.assertEqual(self.remove('capture-a')[0],200)
        self.stop();self.start()
        self.assertEqual(self.request(p+'r5-spl/0',data,key=self.key),(200,{'next':len(data)}))
        self.assertEqual(self.request(p+'commit/0',manifest,key=self.key),(200,{'next':0}))
        self.assertEqual(self.request('/api/sol/devices')[1],[])
        self.assertEqual(self.remove('capture-a')[0],200)
        with self.database() as db:
            self.assertEqual(db.execute('select count(*) from startup_streams').fetchone()[0],0)
            self.assertEqual(db.execute('select count(*) from startup_boots').fetchone()[0],0)
            self.assertEqual(db.execute('select bytes from sol_deleted').fetchone()[0],len(data))
    def test_current_and_incomplete_uploads_are_protected(self):
        for meta in (b'current=yes\n',b'current=yes',b'path=x\ncurrent=yes'):
            self.push(body=meta,stage='meta',gen=0);self.assertEqual(self.remove()[0],409)
        self.assertEqual(self.request('/api/sol/startup/device-a/pending/r5-spl/0',b'data',key=self.key)[0],200)
        self.assertEqual(self.remove('pending')[0],409)
    def test_unknown_latest_boot_is_protected(self):
        self.push();self.assertEqual(self.remove()[0],409)
    def test_auth_origin_body_and_paths(self):
        self.old_live()
        self.assertEqual(self.remove(key=self.key)[0],403)
        self.assertEqual(self.remove(headers={'Origin':'http://evil.example'})[0],403)
        self.assertEqual(self.remove(headers={'X-OTA-Request':'0'})[0],400)
        self.assertEqual(self.remove(body=b'bad')[0],403)
        self.assertEqual(self.remove('absent')[0],404)
        self.assertEqual(self.remove('boot-a/extra')[0],400)
        self.assertEqual(self.remove(headers={'Origin':self.url})[0],200)
    def test_open_view_is_read_only_but_debug_allows_delete(self):
        self.old_live();self.stop();self.start(open_view=True)
        self.assertEqual(self.remove(key='')[0],403)
        self.stop();self.start(no_auth=True);self.assertEqual(self.remove(key='')[0],200)
    def test_real_uploader_continues_after_history_deletion(self):
        self.stop();self.start(no_auth=True)
        store=self.path/'boots'
        for name,order in [('old-boot',1),('current-boot',2)]:
            boot=store/name;boot.mkdir(parents=True);(boot/'order').write_text(str(order)+'\n')
            (boot/'kernel.log').write_text('[seq=1 mono=0 wall=0] initial '+name+'\n')
        config=self.path/'sol.conf';config.write_text(f'server={self.url}\nauth=none\ndevice=device-a\ninterface=lo\n')
        self.worker=subprocess.Popen([str(base.LOG),'forward','--root',str(store)],env=dict(os.environ,LOGCTL_SOL_CONFIG=str(config),LOGCTL_RUN_DIR=str(self.path/'run')),stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        def wait_for(predicate):
            end=time.monotonic()+12
            while time.monotonic()<end:
                if predicate():return
                self.assertIsNone(self.worker.poll());time.sleep(.1)
            self.fail('real uploader did not make progress')
        wait_for(lambda:any(r['boot']=='current-boot' for r in self.request('/api/sol/devices')[1]))
        self.assertEqual(self.remove('old-boot')[0],200)
        with (store/'current-boot/kernel.log').open('a') as f:f.write('[seq=2 mono=1 wall=0] progress after deletion\n')
        wait_for(lambda:any('progress after deletion' in r['text'] for r in self.request('/api/sol/logs/device-a/current-boot/all/0')[1]))
        self.assertFalse(any(r['boot']=='old-boot' for r in self.request('/api/sol/devices')[1]))
        self.worker.terminate();_,stderr=self.worker.communicate(timeout=5);self.worker=None
        self.assertNotIn(b'SOL unavailable',stderr)
    def test_incident_audit_copies_survive_sol_deletion(self):
        self.old_live()
        with self.database() as db:
            db.execute("INSERT INTO evidence_groups(device,publication,source,mode,phase,reason,observed_boot) VALUES('device-a',1,'source-a','logs','rolled-back','boot-failed','recovery-a')")
            db.execute("INSERT INTO evidence_boots VALUES('device-a',1,'source-a','boot-a','target')")
            db.execute("INSERT INTO evidence_chunks(device,publication,source,boot,source_id,stage,gen,off,data) VALUES('device-a',1,'source-a','boot-a',1,'kernel',1,0,'audit evidence')")
        self.assertEqual(self.remove()[0],200)
        with self.database() as db:
            self.assertEqual(db.execute('select count(*) from chunks').fetchone()[0],0)
            self.assertEqual(db.execute('select data from evidence_chunks').fetchone()[0],'audit evidence')
if __name__=='__main__':unittest.main()
