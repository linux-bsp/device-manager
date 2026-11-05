#!/usr/bin/env python3
"""Real slow-reader isolation, bounded workers, download accounting and quarantine."""
import hashlib
import http.client
import importlib.util
import json
import os
from pathlib import Path
import socket
import subprocess
import time
import unittest
spec=importlib.util.spec_from_file_location('admin',Path(__file__).with_name('test-admin.py'))
admin=importlib.util.module_from_spec(spec);spec.loader.exec_module(admin)

class Downloads(unittest.TestCase):
    setUp=admin.AdminHTTP.setUp
    request=admin.AdminHTTP.request
    bundle=admin.AdminHTTP.bundle
    upload=admin.AdminHTTP.upload
    draft=admin.AdminHTTP.draft
    publish=admin.AdminHTTP.publish
    state=admin.AdminHTTP.state
    def fixture(self,version=65537):
        self.held=[]
        files=admin.AdminHTTP.fixture(self,version)
        raw=self.root/'large-root';raw.write_bytes(os.urandom(8*1024*1024))
        dts=f'/dts-v1/; / {{ description="rootfs"; timestamp=<0>; #address-cells=<1>; images {{ rootfs {{ description="rootfs"; data=/incbin/("{raw}"); type="firmware"; arch="arm64"; compression="none"; }}; }}; configurations {{ default="c"; c {{ fw,hwids="1010001000110000"; firmware="rootfs"; }}; }}; }};'
        files['rootfs.itb']=subprocess.run(['dtc','-I','dts','-O','dtb'],input=dts.encode(),capture_output=True,check=True,env=admin.ENV).stdout
        digests={name:hashlib.sha256(data).hexdigest() for name,data in files.items()}
        dts=f'/dts-v1/; / {{ configurations {{ default="c"; c {{ fw,type="release"; fw,version=<{version}>; fw,hwids="1010001000110000"; fw,components="kernel","rootfs"; fw,kernel-file="kernel.itb"; fw,rootfs-file="rootfs.itb"; fw,kernel-sha256="{digests["kernel.itb"]}"; fw,rootfs-sha256="{digests["rootfs.itb"]}"; }}; }}; }};'
        files['release.itb']=subprocess.run(['dtc','-I','dts','-O','dtb'],input=dts.encode(),capture_output=True,check=True,env=admin.ENV).stdout
        return files
    def tearDown(self):
        for stream,response in self.held:response.close();stream.close()
        admin.AdminHTTP.tearDown(self)
    def setup_release(self):
        self.assertEqual(self.publish(self.draft())[0],200)
    def slow(self,path='/releases/1/rootfs.itb'):
        stream=socket.socket();stream.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,4096)
        stream.settimeout(3);stream.connect(('127.0.0.1',self.port))
        stream.sendall(('GET '+path+' HTTP/1.1\r\nHost: localhost\r\nX-OTA-HWID: 1010001000110000\r\n\r\n').encode())
        response=http.client.HTTPResponse(stream);response.begin()
        self.held.append((stream,response));return response
    def children(self):
        return (Path('/proc')/str(self.server.pid)/'task'/str(self.server.pid)/'children').read_text().split()
    def wait(self,predicate):
        for _ in range(100):
            if predicate():return
            time.sleep(.03)
        self.fail('condition did not complete')
    def download_count(self):
        return sum(row['downloads'] for row in self.state()['download_stats'])
    def test_slow_readers_are_bounded_and_control_remains_available(self):
        self.setup_release()
        for _ in range(4):self.assertEqual(self.slow().status,200)
        self.wait(lambda:len(self.children())==4)
        started=time.monotonic()
        self.assertEqual(self.request('GET','/api/state')[0],200)
        self.assertLess(time.monotonic()-started,1)
        self.assertEqual(self.slow().status,503)
        self.assertEqual(len(self.children()),4)
        self.assertEqual(self.download_count(),0)
        for stream,response in self.held:response.close();stream.close()
        self.held.clear();self.wait(lambda:not self.children())
        status,body=self.request('GET','/releases/1/rootfs.itb')
        self.assertEqual((status,hashlib.sha256(body).digest()),(200,hashlib.sha256(self.files['rootfs.itb']).digest()))
        self.wait(lambda:self.download_count()==1)
    def test_range_and_head_preserve_response_and_completion_semantics(self):
        self.setup_release()
        self.assertEqual(self.request('HEAD','/releases/1/rootfs.itb'),(200,b''))
        self.assertEqual(self.download_count(),0)
        status,body=self.request('GET','/releases/1/rootfs.itb',headers={'Range':'bytes=1000-'})
        self.assertEqual(status,206);self.assertEqual(body,self.files['rootfs.itb'][1000:])
        self.wait(lambda:self.download_count()==1)
        self.assertEqual(self.request('GET','/releases/1/rootfs.itb',headers={'Range':'bytes=99999999-'})[0],416)
        self.wait(lambda:not self.children());self.assertEqual(self.download_count(),1)
    def test_quarantine_cancels_inflight_distribution(self):
        self.setup_release();self.assertEqual(self.slow().status,200)
        child=self.children();self.assertEqual(len(child),1)
        body='BSPOTASTATE3 board-a 1010001000110000 1 65537 boot-fail device-error 1 65537 storage-io-error\n'
        self.assertEqual(self.request('POST','/api/devices/board-a',body,{'X-OTA-Request':'1'})[0],200)
        self.wait(lambda:not self.children())
        self.assertEqual(self.request('GET','/releases/1/rootfs.itb')[0],410)
        self.assertEqual(self.download_count(),0)
    def test_worker_does_not_retain_database_or_listener_descriptors(self):
        self.setup_release();self.assertEqual(self.slow().status,200)
        child=self.children()[0]
        paths=[]
        for fd in (Path('/proc')/child/'fd').iterdir():
            try:paths.append(str(fd.readlink()))
            except FileNotFoundError:pass
        self.assertEqual(len(paths),2,paths)
        self.assertEqual(sum(p.startswith('socket:') for p in paths),1)
        self.assertTrue(any(p.endswith('rootfs.itb') for p in paths))
        started=time.monotonic();self.server.terminate();self.server.wait(timeout=2)
        self.assertLess(time.monotonic()-started,2)
        self.assertFalse((Path('/proc')/child).exists())

    def test_transport_deadline_does_not_count_incomplete_download(self):
        self.setup_release()
        self.server.terminate(); self.server.wait(timeout=3)
        source=self.root/'short-alarm.c'
        source.write_text('#define _GNU_SOURCE\n#include <dlfcn.h>\n#include <unistd.h>\nunsigned alarm(unsigned value){unsigned (*real)(unsigned)=dlsym(RTLD_NEXT,"alarm");return real(value?1:0);}\n')
        library=self.root/'short-alarm.so'
        subprocess.run(['cc','-shared','-fPIC',str(source),'-ldl','-o',str(library)],check=True)
        self.server=subprocess.Popen([str(admin.SOURCE/'device-server'),'--root',str(self.feed),'--port',str(self.port)],
            env=dict(admin.ENV,LD_PRELOAD=str(library)),stdout=self.log,stderr=self.log)
        time.sleep(.15)
        self.assertIsNone(self.server.poll())
        self.assertEqual(self.slow().status,200)
        self.wait(lambda:not self.children())
        self.assertEqual(self.download_count(),0)
        self.assertEqual(self.request('GET','/healthz')[0],200)

if __name__=='__main__':unittest.main()
