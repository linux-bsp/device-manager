#!/usr/bin/env python3
"""Real HTTP clock client with process-local clock/RTC interception.
Never modify the host system clock, including when tests run as root.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT=Path(__file__).resolve().parents[1]
STUB=r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/rtc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
static long long base,started;
static long long monotonic(void){struct timespec t;syscall(SYS_clock_gettime,CLOCK_BOOTTIME,&t);return (long long)t.tv_sec*1000000000LL+t.tv_nsec;}
int clock_gettime(clockid_t id,struct timespec *t){
 if(id!=CLOCK_REALTIME)return syscall(SYS_clock_gettime,id,t);
 if(!started){started=monotonic();const char *s=getenv("DM_CLOCK_TEST_NOW");
  if(s&&!strcmp(s,"real")){struct timespec actual;syscall(SYS_clock_gettime,id,&actual);base=(long long)actual.tv_sec*1000000000LL+actual.tv_nsec;}
  else base=s?atoll(s)*1000000000LL:0;
 }
 long long n=base+monotonic()-started;t->tv_sec=n/1000000000LL;t->tv_nsec=n%1000000000LL;return 0;
}
int clock_settime(clockid_t id,const struct timespec *t){
 FILE *f=fopen(getenv("DM_CLOCK_TEST_CAPTURE"),"a");if(f){fprintf(f,"set %lld %ld\n",(long long)t->tv_sec,t->tv_nsec);fclose(f);}
 if(getenv("DM_CLOCK_TEST_DENY")){errno=EPERM;return -1;}
 if(id!=CLOCK_REALTIME){errno=EINVAL;return -1;}
 base=(long long)t->tv_sec*1000000000LL+t->tv_nsec;started=monotonic();return 0;
}
time_t time(time_t *out){struct timespec t;clock_gettime(CLOCK_REALTIME,&t);if(out)*out=t.tv_sec;return t.tv_sec;}
int open(const char *path,int flags,...){
 mode_t mode=0;if(flags&O_CREAT){va_list a;va_start(a,flags);mode=va_arg(a,int);va_end(a);}
 if(!strcmp(path,"/dev/rtc0"))path=getenv("DM_CLOCK_TEST_RTC");
 return syscall(SYS_openat,AT_FDCWD,path,flags,mode);
}
int ioctl(int fd,unsigned long request,...){
 va_list a;va_start(a,request);void *arg=va_arg(a,void *);va_end(a);
 if(request!=RTC_SET_TIME)return syscall(SYS_ioctl,fd,request,arg);
 struct rtc_time *t=arg;FILE *f=fopen(getenv("DM_CLOCK_TEST_CAPTURE"),"a");
 if(f){fprintf(f,"rtc %d-%02d-%02dT%02d:%02d:%02d\n",t->tm_year+1900,t->tm_mon+1,t->tm_mday,t->tm_hour,t->tm_min,t->tm_sec);fclose(f);}
 if(getenv("DM_CLOCK_TEST_RTC_DENY")){errno=EIO;return -1;}return 0;
}
'''
class ClockSync(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build=tempfile.TemporaryDirectory(prefix='clock-stub-');base=Path(cls.build.name)
        source=base/'stub.c';source.write_text(STUB);cls.preload=base/'stub.so'
        subprocess.run(['cc','-shared','-fPIC',str(source),'-o',str(cls.preload),'-ldl'],check=True)
    @classmethod
    def tearDownClass(cls):cls.build.cleanup()
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='time-sync-');self.base=Path(self.tmp.name)
        self.nonces=[];self.mode='valid';self.worker=None;owner=self
        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                nonce=self.path.rsplit('/',1)[-1];owner.nonces.append(nonce)
                sec,nsec=divmod(time.time_ns(),10**9)
                if owner.mode=='nonce':nonce='0'*32
                if owner.mode=='old':sec=0
                if owner.mode=='nsec':nsec=10**9
                body=f'DMTIME1 {nonce} {sec} {nsec}\n'.encode()
                if owner.mode=='garbage':body=b'not a clock\n'
                if owner.mode=='slow':time.sleep(.035)
                if owner.mode=='stall':time.sleep(1.5)
                self.send_response(503 if owner.mode=='unavailable' else 200);self.end_headers()
                try:self.wfile.write(body)
                except (BrokenPipeError,ConnectionResetError):pass
            def log_message(self,*args):pass
        self.server=ThreadingHTTPServer(('127.0.0.1',0),Handler);self.thread=threading.Thread(target=self.server.serve_forever,daemon=True);self.thread.start()
        self.url=f'http://127.0.0.1:{self.server.server_port}'
        conf=self.base/'sol.conf';conf.write_text(f'server={self.url}\nauth=none\ndevice=device-a\ninterface=lo\n')
        self.capture=self.base/'calls';rtc=self.base/'rtc';rtc.touch()
        self.env=dict(os.environ,TZ='CST-8',LD_PRELOAD=str(self.preload),DM_CLOCK_TEST_CAPTURE=str(self.capture),DM_CLOCK_TEST_RTC=str(rtc),LOGCTL_RUN_DIR=str(self.base/'run'),LOGCTL_SOL_CONFIG=str(conf))
    def tearDown(self):
        if self.worker:self.worker.terminate();self.worker.communicate(timeout=8)
        self.server.shutdown();self.server.server_close();self.thread.join();self.tmp.cleanup()
    def sync(self,**values):
        return subprocess.run([str(ROOT/'logctl'),'sync-time'],env=dict(self.env,**values),capture_output=True,text=True,timeout=12)
    def status(self):return dict(line.split('=',1) for line in (self.base/'run/clock-status').read_text().splitlines())
    def test_initial_clock_calibrates_and_writes_utc_rtc(self):
        result=self.sync();self.assertEqual(result.returncode,0,result.stderr)
        calls=self.capture.read_text();seconds=int(re.search(r'set (\d+) ',calls)[1]);self.assertLess(abs(seconds-time.time()),2)
        self.assertIn(time.strftime('rtc %Y-%m-%dT%H:%M:',time.gmtime(seconds)),calls)
        self.assertIn('+0800',result.stderr)
        self.assertEqual(self.status()['clock-timezone'],'+0800')
        self.assertTrue(self.status()['clock-synced-local'].endswith('+0800'))
        status=self.status();self.assertEqual(status['clock-state'],'synchronized');self.assertEqual(status['rtc-state'],'written')
        self.assertEqual(len(self.nonces),3);self.assertEqual(len(set(self.nonces)),3)
    def test_bad_nonce_epoch_precision_and_payload_never_set_clock(self):
        for mode in ('nonce','old','nsec','garbage','unavailable'):
            with self.subTest(mode=mode):
                self.mode=mode;result=self.sync();self.assertNotEqual(result.returncode,0)
                self.assertFalse(self.capture.exists());self.assertEqual(self.status()['clock-state'],'unsynchronized')
    def test_failed_clock_permission_is_reported(self):
        result=self.sync(DM_CLOCK_TEST_DENY='1');self.assertNotEqual(result.returncode,0)
        self.assertEqual(self.status()['clock-state'],'unsynchronized');self.assertNotIn('rtc ',self.capture.read_text())
    def test_rtc_failure_does_not_invalidate_network_time(self):
        result=self.sync(DM_CLOCK_TEST_RTC_DENY='1');self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(self.status()['clock-state'],'synchronized');self.assertEqual(self.status()['rtc-state'],'write-failed')
    def test_already_correct_clock_is_not_stepped(self):
        result=self.sync(DM_CLOCK_TEST_NOW='real');self.assertEqual(result.returncode,0,result.stderr)
        self.assertNotIn('set ',self.capture.read_text());self.assertIn('rtc ',self.capture.read_text())
    def test_auto_worker_cancels_before_setting_time_during_shutdown(self):
        self.mode='stall';conf=self.base/'sol.conf';conf.write_text(conf.read_text()+'time-sync=1\n')
        store=self.base/'boots';boot=store/'boot-a';boot.mkdir(parents=True);(boot/'order').write_text('1\n')
        self.worker=subprocess.Popen([str(ROOT/'logctl'),'forward','--root',str(store)],env=self.env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        for _ in range(50):
            if self.nonces:break
            time.sleep(.02)
        self.assertTrue(self.nonces);start=time.monotonic();self.worker.terminate();self.worker.communicate(timeout=3)
        self.assertLess(time.monotonic()-start,2.5);self.assertFalse(self.capture.exists());self.worker=None
    def test_roundtrip_uncertainty_is_recorded(self):
        self.mode='slow';result=self.sync();self.assertEqual(result.returncode,0,result.stderr)
        self.assertGreaterEqual(int(self.status()['clock-uncertainty-ms']),15)
if __name__=='__main__':unittest.main()
