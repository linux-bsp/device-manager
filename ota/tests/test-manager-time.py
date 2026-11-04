#!/usr/bin/env python3
"""Read-only management time endpoint and immutable archive naming metadata."""
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
import urllib.request
import urllib.error

ROOT=Path(__file__).resolve().parents[1]
class ManagerTime(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='manager-time-');self.root=Path(self.tmp.name)
        self.process=subprocess.Popen([str(ROOT/'device-server'),'--root',str(self.root),'--port','0','--sol-debug-no-auth'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        self.url=self.process.stdout.readline().split()[2].rstrip('/')
        self.http=urllib.request.build_opener(urllib.request.ProxyHandler({}))
    def tearDown(self):
        self.process.terminate();self.process.communicate(timeout=5);self.tmp.cleanup()
    def request(self,nonce,method='GET'):
        request=urllib.request.Request(self.url+'/api/time/'+nonce,method=method,data=b'' if method=='POST' else None)
        try:r=self.http.open(request,timeout=5)
        except urllib.error.HTTPError as e:r=e
        with r:return r.status,r.headers,r.read()
    def test_valid_utc_sample_echoes_nonce_and_is_not_cached(self):
        nonce='0123456789abcdef'*2
        status,headers,body=self.request(nonce);self.assertEqual(status,200)
        version,returned,seconds,nanoseconds=body.decode().split()
        self.assertEqual(version,'DMTIME1');self.assertEqual(returned,nonce)
        self.assertLess(abs(int(seconds)-time.time()),2);self.assertTrue(0<=int(nanoseconds)<10**9)
        self.assertEqual(headers['Cache-Control'],'no-store')
        self.assertTrue(headers['Content-Type'].startswith('text/plain'))
    def test_nonce_and_method_boundaries(self):
        for nonce in ('','a'*31,'a'*33,'A'*32,'g'*32,'a'*32+'?cached=1'):
            with self.subTest(nonce=nonce):self.assertEqual(self.request(nonce)[0],400)
        self.assertEqual(self.request('a'*32,'POST')[0],405)
    def test_head_reports_size_without_a_body(self):
        status,headers,body=self.request('b'*32,'HEAD');self.assertEqual(status,200)
        self.assertGreater(int(headers['Content-Length']),40);self.assertEqual(body,b'')
if __name__=='__main__':unittest.main()
