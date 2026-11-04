#!/usr/bin/env python3
"""Clock source requests survive server restarts; acceptance is not completion."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import urllib.request
import urllib.error
ROOT=Path(__file__).resolve().parents[1]
class ClockControl(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='clock-control-');self.root=Path(self.tmp.name)
        self.http=urllib.request.build_opener(urllib.request.ProxyHandler({}));self.start()
    def start(self,debug=True):
        self.process=subprocess.Popen([str(ROOT/'device-server'),'--root',str(self.root),'--port','0']+(['--sol-debug-no-auth'] if debug else []),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        self.url=self.process.stdout.readline().split()[2].rstrip('/')
    def stop(self):self.process.terminate();self.process.communicate(timeout=5)
    def tearDown(self):self.stop();self.tmp.cleanup()
    def request(self,path,body=None,headers=None,method=None):
        r=urllib.request.Request(self.url+path,data=body.encode() if body is not None else None,headers=headers or {},method=method)
        try:response=self.http.open(r,timeout=5)
        except urllib.error.HTTPError as e:response=e
        with response:return response.status,response.read().decode()
    def config(self,body='DMCLOCK1 1 3600 ntp://192.168.10.1:123\n',**kw):
        return self.request('/api/clock/config/device-a',body,kw.get('headers',{'X-OTA-Request':'1'}))
    def report(self,rev,error=0,applied=None):
        if applied is None:applied=rev
        return self.request('/api/clock/device/device-a',f'DMCLOCKREPORT1 {applied} {rev} {error} 1793752575 12 boot-a\nclock-state=synchronized\nclock-source=ntp\n')
    def state(self):return json.loads(self.request('/api/clock')[1])['devices'][0]
    def test_pending_ack_and_restart_are_durable(self):
        status,body=self.config();self.assertEqual(status,202);rev=json.loads(body)['revision']
        self.assertEqual(self.state()['completed'],0)
        self.assertEqual(self.request('/api/clock/device/device-a')[1],f'DMCLOCK1 {rev} 1 3600 ntp://192.168.10.1:123\n')
        self.stop();self.start();self.assertEqual(self.state()['revision'],rev)
        self.assertEqual(self.report(rev)[0],200);self.assertEqual(self.state()['error'],0);self.assertGreater(self.state()['completed'],0)
        completed=self.state()['completed'];self.assertEqual(self.report(rev)[0],200);self.assertEqual(self.state()['completed'],completed)
    def test_forcing_same_source_creates_new_request(self):
        a=json.loads(self.config()[1])['revision'];self.report(a)
        b=json.loads(self.config()[1])['revision'];self.assertGreater(b,a);self.assertEqual(self.state()['completed'],0)
        self.report(a);self.assertEqual(self.state()['completed'],0)
        self.report(b,113);self.assertEqual(self.state()['error'],113)
    def test_config_validation_and_cross_origin(self):
        for source in ('http://host/path','ntp://user@host','file:///tmp/time','ntp://host:0','ntp://host:65536','ntp://host:abc','ntp://host\nclock=x'):
            with self.subTest(source=source):self.assertEqual(self.config(f'DMCLOCK1 1 3600 {source}\n')[0],400)
        for body in ('DMCLOCK1 1 0 -\n','DMCLOCK1 1 86401 -\n','DMCLOCK1 2 60 -\n','DMCLOCK1 1 3600 -\nextra'):
            self.assertEqual(self.config(body)[0],400)
        self.assertEqual(self.config(headers={})[0],403)
        self.assertEqual(self.config(headers={'X-OTA-Request':'1','Origin':'http://evil.test'})[0],403)
    def test_supported_sources_and_disable(self):
        for source in ('-','ntp://pool.example.org','ntp://[::1]:123','http://192.168.10.1:8090','https://clock.example.org'):
            self.assertEqual(self.config(f'DMCLOCK1 0 60 {source}\n')[0],202)
            self.assertEqual(self.state()['source'],source)
    def test_unrelated_or_false_ack_is_rejected(self):
        rev=json.loads(self.config()[1])['revision']
        self.assertEqual(self.report(rev+99)[0],409)
        self.assertEqual(self.report(rev,applied=0)[0],400)
        self.assertEqual(self.state()['completed'],0)
    def test_normal_mode_does_not_bypass_device_or_viewer_auth(self):
        self.stop();self.start(False)
        self.assertEqual(self.request('/api/clock')[0],403)
        self.assertEqual(self.config()[0],403)
        self.assertEqual(self.request('/api/clock/device/device-a')[0],403)
if __name__=='__main__':unittest.main()
