#!/usr/bin/env python3
"""SNTP packet validation and durable remote configuration; fake device clock only."""
import importlib.util
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import threading
import tempfile
import time
import unittest
import urllib.request
spec=importlib.util.spec_from_file_location('clock_test',Path(__file__).with_name('test_time_sync.py'))
base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)
ROOT=Path(__file__).resolve().parents[1]
class TimeSources(unittest.TestCase):
    setUpClass=classmethod(base.ClockSync.setUpClass.__func__)
    tearDownClass=classmethod(base.ClockSync.tearDownClass.__func__)
    start_worker=base.ClockSync.start_worker
    sync=base.ClockSync.sync
    status=base.ClockSync.status
    def setUp(self):
        base.ClockSync.setUp(self);self.mode='valid';self.ntp_mode='valid';self.ntp_seconds=None;self.packets=0
        self.udp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);self.udp.bind(('127.0.0.1',0));self.udp.settimeout(.1)
        self.ntp_stop=threading.Event()
        def serve():
            while not self.ntp_stop.is_set():
                try:packet,peer=self.udp.recvfrom(512)
                except socket.timeout:continue
                self.packets+=1
                response=bytearray(48);response[0]=0x24;response[1]=2;response[24:32]=packet[40:48]
                sec,ns=divmod(time.time_ns(),10**9)
                if self.ntp_seconds is not None:sec=self.ntp_seconds
                timestamp=struct.pack('!II',(sec+2208988800)&0xffffffff,(ns<<32)//10**9)
                response[32:40]=response[40:48]=timestamp
                if self.ntp_mode=='li':response[0]|=0xc0
                if self.ntp_mode=='stratum':response[1]=0
                if self.ntp_mode=='origin':response[24:32]=b'notnonce'
                if self.ntp_mode=='mode':response[0]=0x23
                if self.ntp_mode=='zero':response[40:48]=b'\0'*8
                if self.ntp_mode=='truncated':response=response[:47]
                if self.ntp_mode!='silent':self.udp.sendto(response,peer)
        self.ntp_thread=threading.Thread(target=serve,daemon=True);self.ntp_thread.start()
        self.source=f'ntp://127.0.0.1:{self.udp.getsockname()[1]}'
        self.disk=tempfile.TemporaryDirectory(prefix='.clock-config-test-',dir=ROOT)
        self.config=Path(self.disk.name)/'time-source.conf';self.env['TIMECTL_STATE_FILE']=str(self.config)
        self.config.write_text(f'DMCLOCKLOCAL1 1 0 3600 {self.source} 0\n')
    def tearDown(self):
        self.ntp_stop.set();self.ntp_thread.join(timeout=2);self.udp.close();base.ClockSync.tearDown(self);self.disk.cleanup()
    def test_ntp_corrects_initial_clock_and_era_after_2036(self):
        for sec in (int(time.time()),2208988800):
            self.ntp_seconds=sec;result=self.sync();self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(self.status()['clock-source'],'ntp');self.assertLess(abs(int(self.status()['clock-synced-at'])-sec),2)
        self.assertEqual(self.packets,6)
    def test_bad_datagrams_cannot_set_time(self):
        for mode in ('li','stratum','origin','mode','zero','truncated'):
            with self.subTest(mode=mode):
                self.ntp_mode=mode;result=self.sync();self.assertNotEqual(result.returncode,0)
                self.assertFalse(self.capture.exists());self.assertEqual(self.status()['clock-state'],'unsynchronized')
    def test_stopping_worker_cancels_waiting_ntp_without_setting_clock(self):
        self.ntp_mode='silent'
        self.config.write_text(f'DMCLOCKLOCAL1 1 1 3600 {self.source} 0\n')
        self.start_worker()
        end=time.monotonic()+4
        while not self.packets and time.monotonic()<end:time.sleep(.02)
        self.assertGreater(self.packets,0);start=time.monotonic()
        self.worker.terminate();self.worker.communicate(timeout=3);self.worker=None
        self.assertLess(time.monotonic()-start,2.5);self.assertFalse(self.capture.exists())
    def test_http_alternative_source_is_independent_of_management_address(self):
        self.config.write_text(f'DMCLOCKLOCAL1 1 0 60 {self.url} 0\n')
        conf=self.base/'time.conf';conf.write_text('server=http://127.0.0.1:1\nauth=none\ndevice=device-a\ninterface=lo\n')
        self.assertEqual(self.sync().returncode,0);self.assertEqual(self.status()['clock-server'],self.url)
    def test_remote_apply_report_and_persistence_across_worker_restart(self):
        (self.base/'feed').mkdir()
        manager=subprocess.Popen([str(ROOT.parent/'ota/device-server'),'--root',str(self.base/'feed'),'--port','0','--sol-debug-no-auth'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        try:
            url=manager.stdout.readline().split()[2].rstrip('/');http=urllib.request.build_opener(urllib.request.ProxyHandler({}))
            conf=self.base/'time.conf';conf.write_text(f'server={url}\nauth=none\ndevice=device-a\ninterface=lo\nenabled=1\n')
            self.config.unlink()
            def submit(source,enabled=1):
                r=urllib.request.Request(url+'/api/clock/config/device-a',data=f'DMCLOCK1 {enabled} 60 {source}\n'.encode(),headers={'X-OTA-Request':'1'})
                with http.open(r) as f:return json.load(f)['revision']
            def state():
                with http.open(url+'/api/clock') as f:return json.load(f)['devices'][0]
            def start():
                self.start_worker()
            def until(predicate):
                end=time.monotonic()+15
                while time.monotonic()<end:
                    row=state()
                    if predicate(row):return row
                    time.sleep(.1)
                self.fail('device did not acknowledge configuration: '+str(state()))
            revision=submit(self.source);start()
            row=until(lambda r:r['completed'] and r['revision']==revision)
            self.assertEqual(row['error'],0);self.assertIn('clock-source=ntp',row['report'])
            self.assertIn(self.source,self.config.read_text())
            self.worker.terminate();self.worker.communicate(timeout=3);self.worker=None
            previous_packets=self.packets;previous_mtime=self.config.stat().st_mtime_ns
            start();row=until(lambda r:self.packets>previous_packets and 'clock-state=synchronized' in r['report'])
            self.assertEqual(self.config.stat().st_mtime_ns,previous_mtime)
            disabled=submit('-',0);row=until(lambda r:r['completed'] and r['revision']==disabled)
            self.assertEqual(row['error'],0);self.assertIn('clock-state=disabled',row['report'])
            failed=submit('http://127.0.0.1:1');row=until(lambda r:r['completed'] and r['revision']==failed)
            self.assertNotEqual(row['error'],0);self.assertIn('clock-state=unsynchronized',row['report'])
        finally:
            if self.worker:self.worker.terminate();self.worker.communicate(timeout=3);self.worker=None
            manager.terminate();manager.communicate(timeout=5)
if __name__=='__main__':unittest.main()
