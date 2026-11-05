#!/usr/bin/env python3
"""Finite boot archive integrity/receipt tests in a disposable server store."""
import hashlib
import os
import subprocess
import time
import tarfile
import io
import json
import importlib.util
import sqlite3
import unittest
import urllib.request
from pathlib import Path
spec=importlib.util.spec_from_file_location('sol_test',Path(__file__).with_name('test-sol.py'))
base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)
spec=importlib.util.spec_from_file_location('sol_delete',Path(__file__).with_name('test-sol-delete.py'))
delete=importlib.util.module_from_spec(spec);spec.loader.exec_module(delete)

class Bundles(unittest.TestCase):
    setUp=base.SOL.setUp
    tearDown=base.SOL.tearDown
    enroll=base.SOL.enroll
    start=base.SOL.start
    stop=base.SOL.stop
    request=base.SOL.request
    remove=delete.Delete.remove
    payload=b'finite binary boot archive\0'*210
    def endpoint(self,boot='boot-a',payload=None,order=1):
        data=self.payload if payload is None else payload
        return f'/api/sol/bundles/device-a/{boot}/{hashlib.sha256(data).hexdigest()}/{len(data)}/{order}/'
    def send(self,action,body=b'',boot='boot-a',payload=None,order=1,key=None):
        return self.request(self.endpoint(boot,payload,order)+str(action),body,key=self.key if key is None else key)
    def complete(self,boot='boot-a',payload=None,order=1):
        data=self.payload if payload is None else payload
        for off in range(0,len(data),4096):self.assertEqual(self.send(off,data[off:off+4096],boot,data,order),(200,{'next':min(off+4096,len(data))}))
        self.assertEqual(self.send('commit',boot=boot,payload=data,order=order),(200,{'next':1}))
    def test_durable_prefix_commit_download_range_and_restart(self):
        self.assertEqual(self.send(0,self.payload[:4096]),(200,{'next':4096}))
        self.assertEqual(self.send('commit'),(200,{'next':0}))
        self.assertEqual(self.request('/api/sol/bundles')[1],[])
        self.stop();self.start()
        self.assertEqual(self.send(0),(200,{'next':4096}))
        self.assertEqual(self.send(4096,self.payload[4096:]),(200,{'next':len(self.payload)}))
        self.assertEqual(self.send('commit'),(200,{'next':1}))
        self.assertEqual(self.send('commit'),(200,{'next':1}))
        self.assertEqual(self.send(0,b'wrong'),(200,{'next':len(self.payload)}))
        status,rows=self.request('/api/sol/bundles');self.assertEqual(status,200)
        self.assertEqual(rows[0]['hash'],hashlib.sha256(self.payload).hexdigest())
        req=urllib.request.Request(self.url+'/api/sol/bundles/device-a/boot-a',headers={'Authorization':'Bearer '+self.viewer,'Range':'bytes=10-'})
        with self.http.open(req) as response:
            self.assertEqual(response.status,206);self.assertEqual(response.read(),self.payload[10:])
            self.assertIn('+0800',response.headers['Content-Disposition'])
    def test_corrupt_full_payload_cannot_receive_archive_receipt(self):
        for off in range(0,len(self.payload),4096):self.send(off,b'x'*len(self.payload[off:off+4096]))
        self.assertEqual(self.send('commit'),(200,{'next':0}))
        self.assertEqual(self.send(0),(200,{'next':0}))
        self.assertEqual(self.request('/api/sol/bundles')[1],[])
        self.complete()
    def test_short_file_recovers_cursor_without_fabricating_missing_bytes(self):
        self.send(0,self.payload[:4096])
        path=self.feed/'sol-bundles/device-a/boot-a.part';path.write_bytes(b'short')
        self.assertEqual(self.send(0),(200,{'next':0}))
        self.complete()
    def test_rename_before_catalog_commit_can_be_recovered(self):
        for off in range(0,len(self.payload),4096):self.send(off,self.payload[off:off+4096])
        path=self.feed/'sol-bundles/device-a/boot-a.part';path.rename(path.with_suffix('.tar.gz'))
        self.stop();self.start()
        self.assertEqual(self.send(0),(200,{'next':0}))
        self.complete()
    def test_auth_path_identity_and_chunk_limits(self):
        self.assertEqual(self.send(0,key=self.viewer)[0],403)
        self.assertEqual(self.send(0,b'x'*4097)[0],400)
        self.assertEqual(self.send('01')[0],400)
        self.assertEqual(self.send('../bad')[0],400)
        self.assertEqual(self.send(0,b'first')[0],200)
        self.assertEqual(self.send(0,payload=b'different'),(200,{'next':0}))
        self.assertTrue(any(e['reason']=='bundle-restarted' for e in self.request('/api/sol/retention/device-a')[1]))
        self.assertEqual(self.send(0,boot='boot-other')[0],503)
        self.assertEqual(self.request('/api/sol/bundles',key=self.key)[0],403)
    def test_quota_removes_whole_archives_with_audit(self):
        for i in range(17):self.complete(f'boot-{i}',b'archive',i+1)
        self.assertEqual(len(self.request('/api/sol/bundles')[1]),16)
        self.assertFalse((self.feed/'sol-bundles/device-a/boot-0.tar.gz').exists())
        self.assertEqual(self.send(0,boot='boot-0',payload=b'archive',order=1)[0],410)
        self.assertTrue(any(e['reason']=='bundle-quota' for e in self.request('/api/sol/retention/device-a')[1]))
    def test_symlink_store_does_not_overwrite_outside_files(self):
        victim=self.path/'victim';victim.mkdir()
        (self.feed/'sol-bundles').symlink_to(victim,target_is_directory=True)
        self.assertEqual(self.send(0)[0],503)
        self.assertEqual(list(victim.iterdir()),[])

    def test_partial_timeout_is_audited_and_new_boot_can_upload(self):
        self.send(0,b'partial')
        with base.database(self.feed/'sol.db') as db:
            db.execute("UPDATE bundles SET received=0")
        self.complete('boot-new',b'new boot',2)
        self.assertEqual(self.send(0)[0],410)
        self.assertFalse((self.feed/'sol-bundles/device-a/boot-a.part').exists())
        self.assertTrue(any(e['reason']=='bundle-timeout' for e in self.request('/api/sol/retention/device-a')[1]))

    def test_explicit_delete_removes_package_and_prevents_recreation(self):
        self.send(0,b'partial')
        self.assertEqual(self.remove()[0],409)
        # Restart the same transfer using its existing bytes.
        self.send(7,self.payload[7:4096]);self.send(4096,self.payload[4096:])
        self.send('commit')  # wrong prefix cannot be committed
        self.complete()
        self.assertEqual(self.remove(),(200,{'deleted':True}))
        self.assertFalse((self.feed/'sol-bundles/device-a/boot-a.tar.gz').exists())
        self.assertEqual(self.request('/api/sol/bundles')[1],[])
        self.assertEqual(self.send(0)[0],410)
        self.assertEqual(self.remove()[0],200)

    def test_real_forwarder_resumes_cached_package_and_receipts_only_closed_boot(self):
        self.stop();self.start(no_auth=True)
        store=self.path/'boots';store.mkdir()
        old=store/'closed-boot';old.mkdir();(old/'order').write_text('1')
        payload=('[seq=1 mono=0 wall=0] '+os.urandom(600000).hex()).encode()
        (old/'kernel.log').write_bytes(payload)
        current=store/'running-boot';current.mkdir();(current/'order').write_text('2')
        (current/'services.log').write_text('[seq=2 mono=0 wall=0] active')
        config=self.path/'sol.conf';config.write_text(f'server={self.url}\nauth=none\ndevice=device-a\ninterface=lo\n')
        env=dict(os.environ,LOGCTL_SOL_CONFIG=str(config),LOGCTL_RUN_DIR=str(self.path/'run'),LOGCTL_BOOT_ID='running-boot')
        def start_worker():
            self.worker=subprocess.Popen([str(base.LOG),'forward','--root',str(store)],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        def wait_for(predicate):
            end=time.monotonic()+50
            while time.monotonic()<end:
                value=predicate()
                if value:return value
                self.assertIsNone(self.worker.poll());time.sleep(.05)
            self.fail('archive did not progress')
        def partial():
            with base.database(self.feed/'sol.db') as db:
                row=db.execute("SELECT pos,size FROM bundles WHERE boot='closed-boot'").fetchone()
            return row and 0<row[0]<row[1]
        start_worker();wait_for(partial)
        state=self.path/'boots-archive-upload/pending.state';cached=self.path/'boots-archive-upload/pending.tar.gz'
        manifest=state.read_bytes();digest=hashlib.sha256(cached.read_bytes()).hexdigest()
        self.worker.kill();self.worker.communicate(timeout=5);self.worker=None
        self.assertFalse((old/'.archive-receipt').exists())
        # Lost process and a server restart preserve durable prefix and package.
        self.stop();self.start(no_auth=True)
        config.write_text(f'server={self.url}\nauth=none\ndevice=device-a\ninterface=lo\n')
        # Bind restart's ephemeral endpoint to the same persisted package.
        state.write_bytes(manifest.replace(manifest.split(b' ')[1],self.url.encode(),1))
        start_worker();wait_for(lambda:(old/'.archive-receipt').exists())
        self.assertFalse((current/'.archive-receipt').exists())
        self.assertFalse(cached.exists())
        rows=self.request('/api/sol/bundles')[1]
        self.assertEqual(len(rows),1);self.assertEqual(rows[0]['hash'],digest)
        with self.http.open(self.url+'/api/sol/bundles/device-a/closed-boot') as response:data=response.read()
        self.assertEqual(hashlib.sha256(data).hexdigest(),digest)
        with tarfile.open(fileobj=io.BytesIO(data)) as package:
            self.assertEqual(package.extractfile('closed-boot/kernel.log').read(),payload)
        # Local retention diagnostics rely on the snapshot receipt, not a cursor.
        run=self.path/'run';(run/'root').write_text(str(store))
        status=subprocess.run([str(base.LOG),'status'],env=env,capture_output=True,text=True,check=True).stdout
        self.assertIn('archive-ack=1',status)
        self.worker.terminate();self.worker.communicate(timeout=5);self.worker=None
        with (old/'kernel.log').open('ab') as file:file.write(b'changed')
        status=subprocess.run([str(base.LOG),'status'],env=env,capture_output=True,text=True,check=True).stdout
        self.assertIn('archive-ack=0',status)

    def test_acknowledged_boot_is_pruned_before_older_unconfirmed_evidence(self):
        self.stop();self.start(no_auth=True)
        store=self.path/'boots';store.mkdir()
        boot=store/'archived-boot';boot.mkdir();(boot/'order').write_text('20')
        (boot/'kernel.log').write_text('[seq=1 mono=0 wall=0] archived')
        config=self.path/'sol.conf';config.write_text(f'server={self.url}\n'+'auth=none\ndevice=device-a\ninterface=lo\n')
        env=dict(os.environ,LOGCTL_SOL_CONFIG=str(config),LOGCTL_RUN_DIR=str(self.path/'run'),LOGCTL_BOOT_ID='current-boot')
        self.worker=subprocess.Popen([str(base.LOG),'forward','--root',str(store)],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        end=time.monotonic()+8
        while not (boot/'.archive-receipt').exists() and time.monotonic()<end:time.sleep(.05)
        self.assertTrue((boot/'.archive-receipt').exists())
        self.worker.terminate();self.worker.communicate(timeout=5);self.worker=None
        for i in range(11):
            old=store/f'old-{i}';old.mkdir();(old/'order').write_text(str(i+1));(old/'kernel.log').write_text('evidence')
        env['LOGCTL_SOL_CONFIG']=str(self.path/'absent.conf')
        self.worker=subprocess.Popen([str(base.LOG.with_name('logctld')),'--root',str(store)],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        end=time.monotonic()+5
        while boot.exists() and time.monotonic()<end:time.sleep(.05)
        self.assertFalse(boot.exists())
        self.assertTrue((store/'old-0/kernel.log').exists())
        events=[json.loads(line) for line in (store/'.retention.log').read_text().splitlines()]
        self.assertEqual(events[0]['reason'],'archived-retention')


if __name__=='__main__':unittest.main()
