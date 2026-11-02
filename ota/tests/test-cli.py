#!/usr/bin/env python3
"""Exercise the compiled C CLI against a disposable real OTA HTTP server."""
import importlib.util
import json
import hashlib
import tarfile
import os
from pathlib import Path
import subprocess
import unittest
spec=importlib.util.spec_from_file_location('admin',Path(__file__).with_name('test-admin.py'))
admin=importlib.util.module_from_spec(spec);spec.loader.exec_module(admin)
class CLI(unittest.TestCase):
    def setUp(self):
        admin.AdminHTTP.setUp(self)
        self.package=self.root/'firmware with spaces;literal.tar.gz';self.package.write_bytes(self.archive)
        self.env=dict(admin.ENV,OTA_SERVER=f'http://127.0.0.1:{self.port}/')
    def cli(self,*args,ok=True,env=None):
        r=subprocess.run([str(admin.SOURCE/'ota-cli'),*map(str,args)],env=env or self.env,capture_output=True,text=True,timeout=30)
        if ok:self.assertEqual(r.returncode,0,r.stderr)
        else:self.assertNotEqual(r.returncode,0,r.stdout)
        return r
    def test_upload_list_publish_and_idempotence(self):
        draft=json.loads(self.cli('upload',self.package).stdout)['id']
        self.assertEqual(self.state()['latest'],0)
        state=json.loads(self.cli('list').stdout)
        self.assertEqual(state['drafts'][0]['id'],draft)
        self.assertIn('\n  "latest":',self.cli('list').stdout)
        publication=json.loads(self.cli('publish',draft).stdout)['publication']
        self.assertEqual(publication,1)
        self.assertEqual(json.loads(self.cli('publish',draft,'--json').stdout)['publication'],1)
        self.assertEqual(self.state()['latest'],1)
    def test_cli_server_overrides_environment_and_json_is_exact(self):
        env=dict(self.env,OTA_SERVER='http://127.0.0.1:1')
        result=self.cli('--server',f'http://127.0.0.1:{self.port}','--json','list',env=env)
        self.assertEqual(result.stdout.encode(),self.request('GET','/api/state')[1])
    def test_rejected_upload_leaves_publication_unchanged(self):
        self.package.write_bytes(b'not a firmware package')
        r=self.cli('upload',self.package,ok=False)
        self.assertIn('HTTP 422',r.stderr)
        self.assertFalse(r.stdout)
        self.assertEqual(self.state()['latest'],0)
        self.assertEqual(self.state()['drafts'],[])
    def test_missing_draft_and_explicit_older_target(self):
        self.assertIn('HTTP',self.cli('publish','draft-missing',ok=False).stderr)
        newer=self.root/'new.tar.gz';newer.write_bytes(self.bundle(self.fixture(65538)))
        new_id=json.loads(self.cli('upload',newer).stdout)['id'];self.cli('publish',new_id)
        old_id=json.loads(self.cli('upload',self.package).stdout)['id'];self.cli('publish',old_id)
        self.assertEqual(self.state()['latest'],2)
    def test_invalid_arguments_and_files(self):
        self.cli('--help')
        for args in [(),('publish','../x'),('upload',self.root),('upload',self.root/'missing'),('list','extra'),('upload',),('--server','file:///tmp/feed','list')]:
            with self.subTest(args=args):self.assertEqual(self.cli(*args,ok=False).returncode,2)
        link=self.root/'link.tar.gz';link.symlink_to(self.package)
        self.assertEqual(self.cli('upload',link,ok=False).returncode,2)
        big=self.root/'big.tar.gz'
        with big.open('wb') as f:f.truncate(512*1024*1024+1)
        self.assertEqual(self.cli('upload',big,ok=False).returncode,2)
        fifo=self.root/'fifo';os.mkfifo(fifo)
        self.assertEqual(self.cli('upload',fifo,ok=False).returncode,2)
    def test_offline_server(self):
        self.server.terminate();self.server.wait(timeout=10)
        self.assertIn('failed',self.cli('list',ok=False).stderr)
    @unittest.skipUnless(os.environ.get('OTA_CLI_TEST_PACKAGE'),'set OTA_CLI_TEST_PACKAGE for a real firmware transport test')
    def test_real_package_roundtrip(self):
        package=Path(os.environ['OTA_CLI_TEST_PACKAGE'])
        draft=json.loads(self.cli('upload',package).stdout)['id']
        self.assertEqual(self.state()['latest'],0)
        publication=json.loads(self.cli('publish',draft).stdout)['publication']
        identity_status, identity = self.request('GET', f'/releases/{publication}/identity')
        self.assertEqual(identity_status, 200, identity)
        self.assertTrue(identity.startswith(f'BSPOTAIMAGE1 {publication} '.encode()))
        version_status, version = self.request('GET', f'/releases/{publication}/version')
        self.assertEqual(version_status, 200, version)
        with tarfile.open(package, 'r:gz') as archive:
            release = self.root/'published-release.itb'
            release.write_bytes(archive.extractfile('release.itb').read())
        declared = subprocess.run(['fdtget', '-t', 's', str(release),
            '/configurations/conf-1', 'fw,firmware-id'], env=admin.ENV, capture_output=True, text=True)
        if declared.returncode == 0:
            tag = subprocess.check_output(['fdtget', '-t', 's', str(release),
                '/configurations/conf-1', 'fw,tag'], env=admin.ENV, text=True).strip()
            self.assertEqual(version.decode().split(),
                ['BSPOTAVERSION1', str(publication), tag, declared.stdout.strip()])
            entry = next(x for x in self.state()['releases'] if x['publication'] == publication)
            self.assertEqual(entry['tag'], tag)
            self.assertEqual(entry['firmware_id'], declared.stdout.strip())
        root_hash=identity.decode().split()[6]
        restore_status, restore_package = self.request('GET', f'/recovery/ti-am62x/am62x-sd-ab-v1/rootfs/{root_hash}/rootfs.itb')
        self.assertEqual(restore_status, 200)
        cache=self.root/'downloaded'
        agent=subprocess.run([str(admin.SOURCE/'fwctld'),'--server',self.env['OTA_SERVER'],
            '--cache',str(cache),'--product','ti-am62x','--layout','am62x-sd-ab-v1','--once'],
            env=self.env,capture_output=True,text=True,timeout=60)
        self.assertEqual(agent.returncode,0,agent.stdout+agent.stderr)
        with tarfile.open(package,'r:gz') as archive:
            for name in ('release.itb','kernel.itb','rootfs.itb'):
                with archive.extractfile(name) as source:
                    self.assertEqual(hashlib.sha256(source.read()).digest(),
                        hashlib.sha256((cache/str(publication)/name).read_bytes()).digest())
        self.assertEqual(restore_package,(cache/str(publication)/'rootfs.itb').read_bytes())
        self.assertEqual(json.loads(self.cli('list').stdout)['latest'],publication)

    def test_list_preserves_device_history(self):
        for publication in (1,2):
            body=f'BSPOTASTATE1 cli-board ti-am62x am62x-sd-ab-v1 {publication} 65538 boot-{publication} success 1 65538\n'
            self.assertEqual(self.request('POST','/api/devices/cli-board',body,{'X-OTA-Request':'1'})[0],200)
        self.assertEqual(len(json.loads(self.cli('list').stdout)['device_history']),2)
for name in ('tearDown','fixture','bundle','request','state'):
    setattr(CLI,name,admin.AdminHTTP.__dict__[name])
if __name__=='__main__':unittest.main()
