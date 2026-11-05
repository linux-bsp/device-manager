#!/usr/bin/env python3
"""Real HTTP/log forwarder tests; no hardware, flash or production feed access."""
import contextlib
import sqlite3
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
import urllib.request
import urllib.error

ROOT = Path(__file__).resolve().parents[1]
LOG = ROOT.parent/'logging/logctl'
@contextlib.contextmanager
def database(path):
    connection=sqlite3.connect(path)
    try:
        with connection:yield connection
    finally:connection.close()

class SOL(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='sol-test-')
        self.path=Path(self.tmp.name);self.feed=self.path/'feed';self.feed.mkdir()
        self.process=None;self.worker=None
        self.enroll('device-a');self.enroll('device-b')
        self.viewer=(self.feed/'sol-keys/viewer').read_text().strip()
        self.key=(self.feed/'sol-keys/device-a').read_text().strip()
        self.start()
    def enroll(self,device):
        subprocess.run([str(ROOT/'device-server'),'sol-enroll','--root',str(self.feed),'--device',device],check=True,stdout=subprocess.DEVNULL)
    def start(self,open_view=False,no_auth=False):
        self.process=subprocess.Popen([str(ROOT/'device-server'),'--root',str(self.feed),'--port','0',*(['--sol-open-view'] if open_view else []),*(['--sol-debug-no-auth'] if no_auth else [])],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        line=self.process.stdout.readline();self.url=line.split()[2].rstrip('/')
        self.http=urllib.request.build_opener(urllib.request.ProxyHandler({}))
    def stop(self):
        if self.process:self.process.terminate();self.process.communicate(timeout=5);self.process=None
    def tearDown(self):
        if self.worker:self.worker.terminate();self.worker.communicate(timeout=8)
        self.stop();self.tmp.cleanup()
    def request(self,path,data=None,key=None,headers=None):
        h={'Authorization':'Bearer '+(self.viewer if key is None else key)};h.update(headers or {})
        req=urllib.request.Request(self.url+path,data=data,headers=h)
        try:r=self.http.open(req,timeout=5)
        except urllib.error.HTTPError as e:r=e
        with r:return r.status,json.loads(r.read())
    def push(self,body=b'line\n',device='device-a',boot='boot-a',order=1,stage='kernel',gen=1,offset=0,key=None):
        return self.request(f'/api/sol/push/{device}/{boot}/{order}/{stage}/{gen}/{offset}',data=body,key=self.key if key is None else key)
    def logs(self,boot='boot-a',stage='all',cursor=0):
        status,rows=self.request(f'/api/sol/logs/device-a/{boot}/{stage}/{cursor}');self.assertEqual(status,200);return rows
    def test_first_connection_is_immutable_across_pushes_and_restart(self):
        self.assertEqual(self.push(body=b'x')[0],200)
        row=self.request('/api/sol/devices')[1][0]
        first=row['first_connected_at'];self.assertEqual(row['connection_time_source'],'sol')
        with database(self.feed/'sol.db') as db:
            db.execute("UPDATE boots SET seen=seen+100")
        self.assertEqual(self.push(stage='meta',body=b'current=yes\n')[0],200)
        self.stop();self.start()
        self.assertEqual(self.request('/api/sol/devices')[1][0]['first_connected_at'],first)
    def test_legacy_connection_uses_oldest_audited_request(self):
        self.push()
        self.stop()
        with database(self.feed/'sol.db') as db:
            db.execute('DROP TABLE sol_connections')
            db.execute('UPDATE boots SET seen=1790001000')
        with database(self.feed/'firmware.db') as db:
            db.execute('CREATE TABLE device_requests(device TEXT,request_boot TEXT,first_seen INTEGER)')
            db.executemany('INSERT INTO device_requests VALUES(?,?,?)',[('device-a','boot-a',1790000000),('device-a','boot-a',1790000100)])
        self.start()
        row=self.request('/api/sol/devices')[1][0]
        self.assertEqual(row['first_connected_at'],1790000000)
        self.assertEqual(row['connection_time_source'],'ota-history')

    def test_credentials_and_method_boundaries(self):
        self.assertEqual(self.request('/api/sol/devices',key='0'*64)[0],403)
        self.assertEqual(self.push(device='device-b')[0],403)
        self.assertEqual(self.push(key=self.viewer)[0],403)
        self.assertEqual(self.push(stage='../bad')[0],400)
        self.assertEqual(self.push(body=b'x'*4097)[0],400)
        self.assertEqual(self.request('/api/sol/devices',data=b'')[0],405)
        self.assertEqual(self.request('/api/sol/push/device-a/boot-a/1/kernel/1/0',b'x',key=self.key,headers={'Origin':'http://evil.test'})[0],400)
    def test_open_view_only_bypasses_browser_read_auth(self):
        self.assertEqual(self.request('/api/sol/config',key='')[1],{'open_view':False})
        self.stop();self.start(open_view=True)
        self.assertEqual(self.request('/api/sol/config',key='')[1],{'open_view':True})
        self.assertEqual(self.request('/api/sol/devices',key='')[0],200)
        self.assertEqual(self.push(key='')[0],403)
        self.assertEqual(self.push()[0],200)
        self.assertEqual(self.request('/api/sol/logs/device-a/boot-a/all/0',key='')[0],200)
    @unittest.skipUnless(LOG.exists(), "build host logctl to test the uploader")
    def test_debug_connects_without_any_credentials(self):
        self.stop()
        import shutil
        shutil.rmtree(self.feed/'sol-keys')
        self.start(no_auth=True)
        self.assertEqual(self.request('/api/sol/config',key='')[1],{'open_view':True})
        with self.http.open(self.url+'/api/sol/devices') as r:self.assertEqual(r.status,200)
        store=self.path/'boots';boot=store/'boot-a';boot.mkdir(parents=True);(boot/'order').write_text('1\n')
        (boot/'kernel.log').write_text('[seq=1 mono=0 wall=0] no-credential-stream\n')
        config=self.path/'sol.conf';config.write_text(f'server={self.url}\nauth=none\ndevice=device-a\ninterface=eth0\n')
        env=dict(os.environ,LOGCTL_SOL_CONFIG=str(config),LOGCTL_RUN_DIR=str(self.path/'run'))
        self.worker=subprocess.Popen([str(LOG),'forward','--root',str(store)],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        for _ in range(80):
            rows=self.logs()
            if rows:break
            self.assertIsNone(self.worker.poll());time.sleep(.1)
        self.assertEqual(len(rows),1);self.assertIn('no-credential-stream',rows[0]['text'])
        self.assertFalse((self.feed/'sol-keys').exists())
    def test_cursor_duplicate_and_restart(self):
        self.assertEqual(self.push(),(200,{'next':5}))
        self.assertEqual(self.push(),(409,{'next':5}))
        self.assertEqual(len(self.logs()),1)
        self.assertEqual(self.push(body=b'',offset=0),(200,{'next':5}))
        self.stop();self.start()
        self.assertEqual(self.push(body=b'two\n',offset=5),(200,{'next':9}))
        rows=self.logs();self.assertEqual(len(rows),2)
        self.assertEqual([(r["gen"],r["off"]) for r in rows],[(1,0),(1,5)])
        self.assertEqual(len(self.logs(cursor=rows[0]['id'])),1)
        self.assertEqual(self.logs(stage='uboot'),[])
    def test_binary_utf8_and_literal_markup(self):
        data='日志 <script>alert(1)</script>\x00\n'.encode()
        self.assertEqual(self.push(body=data)[0],200)
        actual=self.logs()[0]['text'].encode('latin1')
        self.assertEqual(actual,data)
    def test_boot_and_generation_retention(self):
        for i in range(18):self.assertEqual(self.push(boot=f'boot-{i}',order=i+1)[0],200)
        status,boots=self.request('/api/sol/devices');self.assertEqual(status,200)
        self.assertEqual(len(boots),16);self.assertEqual(boots[0]['boot'],'boot-17')
        self.assertEqual(self.logs('boot-0'),[])
        for gen in range(2,8):self.assertEqual(self.push(boot='boot-17',order=18,gen=gen)[0],200)
        self.assertEqual(len(self.logs('boot-17')),4)
        # Backfill an old boot: arriving last must not evict the current boot.
        self.assertEqual(self.push(boot='boot-0',order=1)[0],200)
        self.assertTrue(self.logs('boot-17'))
        status,events=self.request('/api/sol/retention/device-a');self.assertEqual(status,200)
        self.assertTrue(any(e['reason']=='generation-limit' for e in events))
        self.assertTrue(any(e['reason']=='boot-or-capacity-limit' for e in events))
    def test_chunk_retention_and_private_key_not_served(self):
        for i in range(2052):
            self.assertEqual(self.push(body=b'x',offset=i)[0],200)
        import sqlite3
        with database(self.feed/'sol.db') as c:
            self.assertEqual(c.execute('select count(*) from chunks').fetchone()[0],2052)
            self.assertEqual(c.execute('select min(off) from chunks').fetchone()[0],0)
        self.assertEqual(self.push(body=b''),(200,{'next':2052}))
        try:self.http.open(self.url+'/sol-keys/viewer',timeout=5)
        except urllib.error.HTTPError as e:self.assertEqual(e.code,404)
        else:self.fail('private credential exposed by HTTP')
    def test_legacy_sparse_cursor_is_reset_and_audited_on_restart(self):
        self.push(body=b'first');self.push(body=b'second',offset=5)
        self.stop()
        import sqlite3
        with database(self.feed/'sol.db') as c:c.execute('delete from chunks where off=0')
        self.start()
        self.assertEqual(self.push(body=b''),(200,{'next':0}))
        self.assertEqual(self.push(body=b'firstsecond'),(200,{'next':11}))
        status,events=self.request('/api/sol/retention/device-a');self.assertEqual(status,200)
        self.assertEqual(events[0]['reason'],'legacy-incomplete-stream')
        self.assertEqual(len(self.logs()),1)
    def test_capacity_prunes_whole_boots_without_sparse_cursors(self):
        self.push(boot='old',body=b'old');self.push(boot='current',order=2,body=b'x')
        # Seed many tiny chunks efficiently; request processing enforces the
        # same hard object quota as large chunks (at most 64 MiB/device).
        import sqlite3
        with database(self.feed/'sol.db') as c:
            c.executemany('INSERT INTO chunks(device,boot,stage,gen,off,data) VALUES(?,?,?,?,?,?)',
                [('device-a','old','kernel',1,i,'x') for i in range(3,16386)])
            c.execute("UPDATE streams SET pos=16386 WHERE boot='old'")
        self.assertEqual(self.push(boot='current',order=2,body=b'y',offset=1),(200,{'next':2}))
        self.assertEqual(self.logs('old'),[])
        self.assertEqual([r['off'] for r in self.logs('current')],[0,1])
        self.assertEqual(self.push(boot='old',order=1,body=b''),(200,{'next':0}))
    @unittest.skipUnless(LOG.exists(), 'build host logctl')
    def test_forwarder_backfills_more_than_four_boots_and_chunk_budget(self):
        store=self.path/'boots';store.mkdir()
        payload=b'[seq=1 mono=0 wall=0] '+b'x'*(140*1024)
        for i in range(7):
            boot=store/f'boot-{i}';boot.mkdir();(boot/'order').write_text(str(i+1))
            (boot/'kernel.log').write_bytes(payload if i==0 else f'[seq=1 mono=0 wall=0] history-{i}'.encode())
        config=self.path/'sol.conf';config.write_text(f'server={self.url}\ndevice=device-a\nkey-file={self.feed}/sol-keys/device-a\ninterface=eth0\n')
        env=dict(os.environ,LOGCTL_SOL_CONFIG=str(config),LOGCTL_RUN_DIR=str(self.path/'run'))
        self.worker=subprocess.Popen([str(LOG),'forward','--root',str(store)],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        self.request('/api/sol/devices')
        for _ in range(360):
            with database(self.feed/'sol.db') as c:
                rows=c.execute('select boot,pos from streams').fetchall()
            if len(rows)==7 and dict(rows).get('boot-0')==len(payload):break
            self.assertIsNone(self.worker.poll());time.sleep(.1)
        else:self.fail('historical boots or yielded chunk stream starved')
    def test_symlink_credential_rejected(self):
        p=self.feed/'sol-keys/device-a';p.unlink();p.symlink_to(self.feed/'sol-keys/viewer')
        self.assertEqual(self.push(key=self.viewer)[0],403)
    @unittest.skipUnless(LOG.exists(), "build host logctl to test the uploader")
    def test_waits_for_separate_credential_without_exiting(self):
        store=self.path/'boots';boot=store/'boot-a';boot.mkdir(parents=True);(boot/'order').write_text('1\n')
        (boot/'events.log').write_text('[seq=1 mono=0 wall=0] provisioning test\n')
        keyfile=self.path/'provisioned-key'
        config=self.path/'sol.conf';config.write_text(f'server={self.url}\ndevice=device-a\nkey-file={keyfile}\ninterface=eth0\n')
        env=dict(os.environ,LOGCTL_SOL_CONFIG=str(config),LOGCTL_RUN_DIR=str(self.path/'run'))
        self.worker=subprocess.Popen([str(LOG),'forward','--root',str(store)],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        time.sleep(.3);self.assertIsNone(self.worker.poll());self.assertEqual(self.logs(),[])
        fd=os.open(keyfile,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
        with os.fdopen(fd,'w') as f:f.write(self.key+'\n')
        for _ in range(80):
            if self.logs():break
            time.sleep(.1)
        self.assertEqual(len(self.logs()),1)
    @unittest.skipUnless(LOG.exists(), "build host logctl to test the uploader")
    def test_real_forwarder_offline_rotation_and_reconnect(self):
        store=self.path/'boots';boot=store/'boot-a';boot.mkdir(parents=True);(boot/'order').write_text('1\n')
        log=boot/'services.log';log.write_text('[seq=1 mono=0 wall=0] first\n')
        config=self.path/'sol.conf';config.write_text(f'server={self.url}\ndevice=device-a\nkey-file={self.feed}/sol-keys/device-a\ninterface=eth0\n')
        env=dict(os.environ,LOGCTL_SOL_CONFIG=str(config),LOGCTL_RUN_DIR=str(self.path/'run'))
        self.worker=subprocess.Popen([str(LOG),'forward','--root',str(store)],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        def wait(predicate):
            for _ in range(150):
                if predicate():return
                self.assertIsNone(self.worker.poll());time.sleep(.1)
            self.fail('forwarder timed out')
        wait(lambda:len(self.logs())==1)
        # Simulate worker restart/lost client cursor: server must deduplicate.
        self.worker.terminate();self.worker.communicate(timeout=8)
        log.rename(boot/'services.log.1');log.write_text('[seq=2 mono=1 wall=1] second\n')
        self.worker=subprocess.Popen([str(LOG),'forward','--root',str(store)],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        wait(lambda:len(self.logs())==2)
        status,boots=self.request('/api/sol/devices');self.assertEqual(status,200)
        self.assertIn('storage=',boots[0]['meta']);self.assertIn('uboot=unavailable',boots[0]['meta'])
        # Stop HTTP, append data and start it again on the same port.
        port=self.url.rsplit(':',1)[1];self.stop()
        with log.open('a') as f:f.write('[seq=3 mono=2 wall=2] while-offline\n')
        time.sleep(5.5)
        self.process=subprocess.Popen([str(ROOT/'device-server'),'--root',str(self.feed),'--port',port],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        self.process.stdout.readline()
        wait(lambda:len(self.logs())==3)

if __name__=='__main__':unittest.main()
