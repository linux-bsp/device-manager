#!/usr/bin/env python3
"""Real sockets/files: resume boundaries, corrupt checkpoints, HTTP and cursor protocol."""
import errno
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shlex
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
PAYLOAD = bytes(range(256))*20000
DIGEST = hashlib.sha256(PAYLOAD).hexdigest()

class Transfer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='transfer-build-')
        cls.driver = Path(cls.build.name)/'driver'
        subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c11','-Wall','-Wextra','-Werror',
            '-I'+str(ROOT/'include'),'-I'+str(ROOT.parent/'firmware/src')]+shlex.split(os.environ.get('CPPFLAGS',''))+
            [str(ROOT/'tests/driver.c'),str(ROOT.parent/'firmware/src/dm_fw_source.c'),str(ROOT/'build/libdm-transfer.a'),str(ROOT/'build/libdm-transfer-server.a')]+
            shlex.split(os.environ.get('LDFLAGS',''))+['-lcurl','-lcrypto','-pthread','-o',str(cls.driver)],check=True)
    @classmethod
    def tearDownClass(cls): cls.build.cleanup()
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='transfer-test-'); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name); self.path = self.root/'firmware'
        self.requests = []; self.uploads = []; self.mode = 'normal'; outer=self
        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                outer.requests.append(dict(self.headers))
                mode=outer.mode
                if mode=='retry' and len(outer.requests)==1:
                    self.send_response(503);self.end_headers();return
                if mode=='missing': self.send_response(404);self.end_headers();return
                offset=int(self.headers.get('Range','bytes=0-')[6:-1])
                ranged='Range' in self.headers and mode!='ignore-range'
                if not ranged:offset=0
                data=PAYLOAD[offset:]
                self.send_response(206 if ranged else 200)
                if ranged:
                    text=f'bytes {offset}-{len(PAYLOAD)-1}/{len(PAYLOAD)}'
                    if mode=='bad-range':text=f'bytes -1-{len(PAYLOAD)-1}/{len(PAYLOAD)}'
                    self.send_header('Content-Range',text)
                self.send_header('Content-Length',str(len(data)));self.end_headers()
                if mode=='partial': data=data[:len(data)//2]
                if mode=='corrupt':data=bytes([data[0]^1])+data[1:]
                try:
                    for i in range(0,len(data),32768):
                        self.wfile.write(data[i:i+32768]);self.wfile.flush()
                        if mode=='slow':time.sleep(.02)
                except (BrokenPipeError,ConnectionResetError):pass
            def do_POST(self):
                outer.uploads.append(self.rfile.read(int(self.headers['Content-Length'])))
                if outer.mode=='post-drop':self.close_connection=True;return
                self.send_response(200);self.end_headers();self.wfile.write(b'ok')
            def log_message(self,*args):pass
        self.server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
        self.worker=threading.Thread(target=self.server.serve_forever,daemon=True);self.worker.start()
        self.addCleanup(self.close_server)
        self.url=f'http://127.0.0.1:{self.server.server_port}/file'
    def close_server(self):self.server.shutdown();self.worker.join();self.server.server_close()
    def run_driver(self,*args,**kwargs):
        return subprocess.check_output([str(self.driver),*map(str,args)],text=True,**kwargs)
    def download(self,cancel=0,resume=1,digest=DIGEST,size=len(PAYLOAD),maximum=len(PAYLOAD),attempts=1):
        return json.loads(self.run_driver('download',self.url,self.path,digest,size,maximum,cancel,resume,attempts))
    def interrupted(self):
        self.mode='partial';r=self.download();self.assertNotEqual(r['rc'],0)
        self.assertEqual(r['kind'],1);self.assertFalse(self.path.exists())
        state=Path(str(self.path)+'.part.state').read_text().split()
        self.assertEqual(int(state[3]),len(PAYLOAD)//2)
        return r['durable']
    def test_atomic_resume_across_processes_and_uncommitted_tail(self):
        offset=self.interrupted()
        with Path(str(self.path)+'.part').open('ab') as stream:stream.write(b'uncommitted tail')
        self.mode='normal';r=self.download()
        self.assertEqual(r['rc'],0);self.assertEqual(r['resume'],offset)
        self.assertEqual(self.requests[-1]['Range'],f'bytes={offset}-')
        self.assertEqual(self.path.read_bytes(),PAYLOAD)
        self.assertFalse(Path(str(self.path)+'.part.state').exists())
        count=len(self.requests);r=self.download();self.assertEqual(r['cached'],1);self.assertEqual(len(self.requests),count)
    def test_corrupt_prefix_or_checkpoint_preserves_evidence(self):
        for part in ('prefix','checkpoint'):
            with self.subTest(part=part):
                self.path=self.root/part;self.interrupted();self.mode='normal'
                p=Path(str(self.path)+('.part' if part=='prefix' else '.part.state'))
                data=p.read_bytes();p.write_bytes(bytes([data[0]^1])+data[1:])
                count=len(self.requests);r=self.download()
                self.assertEqual(r['kind'],4);self.assertNotEqual(r['rc'],0)
                self.assertEqual(len(self.requests),count);self.assertEqual(p.read_bytes(),bytes([data[0]^1])+data[1:])
    def test_range_ignored_restarts_without_appending(self):
        self.interrupted();self.mode='ignore-range';r=self.download()
        self.assertEqual(r['rc'],0);self.assertEqual(self.path.read_bytes(),PAYLOAD)
        self.assertNotIn('Range',self.requests[-1])
    def test_invalid_range_does_not_append(self):
        offset=self.interrupted();self.mode='bad-range';r=self.download()
        self.assertEqual(r['kind'],6);self.assertEqual(r['durable'],offset)
    def test_hash_mismatch_not_promoted(self):
        self.mode='corrupt';r=self.download()
        self.assertEqual(r['kind'],5);self.assertFalse(self.path.exists())
        self.assertFalse(Path(str(self.path)+'.part').exists())
    def test_identity_change_discards_old_prefix(self):
        self.interrupted();self.mode='normal';r=self.download(digest='0'*64)
        self.assertEqual(r['kind'],5);self.assertNotIn('Range',self.requests[-1])
    def test_cancel_is_resumable(self):
        self.mode='slow';r=self.download(cancel=1024*1024)
        self.assertEqual(r['kind'],7);self.assertGreater(r['durable'],0)
        self.mode='normal';self.assertEqual(self.download()['rc'],0)
    def test_killed_process_uses_only_last_checkpoint(self):
        self.mode='slow'
        p=subprocess.Popen([str(self.driver),'download',self.url,str(self.path),DIGEST,str(len(PAYLOAD)),str(len(PAYLOAD)),'0','1'],stdout=subprocess.PIPE)
        try:
            state=Path(str(self.path)+'.part.state');deadline=time.monotonic()+5
            while time.monotonic()<deadline:
                if state.exists() and int(state.read_text().split()[3])>=1024*1024:break
                time.sleep(.02)
            else:self.fail('No durable checkpoint')
            p.kill();p.communicate(timeout=3)
            offset=int(state.read_text().split()[3]);self.mode='normal'
            r=self.download();self.assertEqual(r['rc'],0);self.assertEqual(r['resume'],offset)
        finally:
            if p.poll() is None:p.kill();p.communicate()
    def test_http_status_limit_and_bounded_retry(self):
        self.mode='missing';r=self.download();self.assertEqual(r['kind'],3);self.assertEqual(r['status'],404)
        self.mode='normal';r=self.download(size=0,digest='-',maximum=100,resume=0)
        self.assertEqual(r['kind'],5);self.assertFalse(self.path.exists())
        self.requests.clear();self.mode='retry';self.assertEqual(self.download(attempts=2)['rc'],0)
        self.assertEqual(len(self.requests),2)
    def test_failed_flush_never_promotes_a_file(self):
        preload=self.root/'sync.so'
        subprocess.run(['cc','-Wall','-Wextra','-Werror','-shared','-fPIC',
            str(ROOT.parent/'tests/fixtures/sync_fault.c'),'-ldl','-o',str(preload)],check=True)
        marker=self.root/'fault';marker.touch()
        for case in ('file', 'checkpoint', 'directory'):
            for error in (errno.EIO,errno.ENOSPC,errno.EROFS):
                self.path=self.root/(str(error)+case)
                env=dict(os.environ,LD_PRELOAD=str(preload),DM_TEST_SYNC_MARKER=str(marker),
                    DM_TEST_SYNC_PATH=str(self.root if case=='directory' else self.path)+('.part.state' if case=='checkpoint' else ''),
                    DM_TEST_SYNC_SUFFIX='.part' if case=='file' else self.root.name if case=='directory' else '',DM_TEST_SYNC_ERRNO=str(error))
                r=json.loads(self.run_driver('download',self.url,self.path,DIGEST,len(PAYLOAD),len(PAYLOAD),0,1,env=env))
                self.assertEqual(r['kind'],4);self.assertNotEqual(r['rc'],0)
                if case=='file':self.assertFalse(self.path.exists())
                self.assertEqual(self.download()['rc'],0)

    def test_streaming_upload_and_no_post_replay(self):
        self.path.write_bytes(PAYLOAD)
        r=json.loads(self.run_driver('upload',self.url,self.path,len(PAYLOAD)))
        self.assertEqual(r['rc'],0);self.assertEqual(self.uploads,[PAYLOAD])
        self.mode='post-drop';r=json.loads(self.run_driver('post',self.url))
        self.assertNotEqual(r['rc'],0);self.assertEqual(self.uploads[-1],b'body');self.assertEqual(len(self.uploads),2)
    def test_response_bounds_protocol_and_symlink(self):
        r=json.loads(self.run_driver('request',self.url));self.assertEqual(r['kind'],6)
        r=json.loads(self.run_driver('request','file:///etc/hosts'));self.assertNotEqual(r['rc'],0)
        other=self.root/'untouched';other.write_text('keep')
        Path(str(self.path)+'.part').symlink_to(other)
        self.assertEqual(self.download()['kind'],4);self.assertEqual(other.read_text(),'keep')
    def socket_driver(self,mode,*args,payload=None):
        a,b=socket.socketpair()
        p=subprocess.Popen([str(self.driver),mode,str(b.fileno()),*map(str,args)],pass_fds=(b.fileno(),),stdout=subprocess.PIPE,text=True);b.close()
        if payload is not None:a.sendall(payload);a.shutdown(socket.SHUT_WR)
        if mode=='serve':
            received=b''
            while True:
                part=a.recv(65536)
                if not part:break
                received+=part
        else:received=None
        output=p.communicate(timeout=3)[0];a.close();return output,received
    def test_server_parser_and_deadline(self):
        for header,expected in [(b'Content-Length: 5\r\n',0),(b'Content-Length: 1\r\nContent-Length: 1\r\n',400),
            (b'Transfer-Encoding: chunked\r\n',400),(b'Content-Length: -1\r\n',400),
            (b'Content-Length: 18446744073709551616\r\n',400),(b'X: '+b'a'*8200+b'\r\n',431)]:
            output,_=self.socket_driver('parse',payload=b'POST / HTTP/1.1\r\n'+header+b'\r\n')
            self.assertEqual(int(output.split()[0]),expected)
        output,_=self.socket_driver('receive');self.assertEqual(int(output),-errno.ETIMEDOUT)
    def test_server_file_range_head_and_invalid(self):
        self.path.write_bytes(b'abcdef')
        for rang,head,status,body in [('-',0,200,b'abcdef'),('bytes=2-',0,206,b'cdef'),('bytes=9-',0,416,b''),('-',1,200,b'')]:
            output,wire=self.socket_driver('serve',self.path,6,head,rang)
            headers,actual=wire.split(b'\r\n\r\n',1)
            self.assertIn(str(status).encode(),headers.splitlines()[0]);self.assertEqual(actual,body)
        output,_=self.socket_driver('serve',self.path,6,0,'-','bad\r\nname');self.assertEqual(int(output),-errno.EINVAL)
    def test_firmware_source_http_limits_and_cleanup(self):
        output=self.run_driver('source',self.url,self.root).split()
        # A manifest exceeds the firmware engine's 4 MiB manifest limit.
        self.assertNotEqual(int(output[0]),0);self.assertEqual(list(self.root.iterdir()),[])
        self.mode='missing';output=self.run_driver('source',self.url,self.root).split()
        self.assertEqual(int(output[0]),-errno.ENOENT);self.assertEqual(list(self.root.iterdir()),[])
    def test_firmware_source_tftp_backend_and_temporary_cleanup(self):
        payload=b'FIT fixture via TFTP'+bytes(range(256))*5
        udp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);udp.bind(('127.0.0.1',0));udp.settimeout(5)
        errors=[]
        def serve():
            try:
                request,peer=udp.recvfrom(2048)
                self.assertEqual(request[:2],b'\0\1')
                for offset in range(0,len(payload)+1,512):
                    block=offset//512+1
                    udp.sendto(struct.pack('!HH',3,block)+payload[offset:offset+512],peer)
                    ack,_=udp.recvfrom(64);self.assertEqual(ack,struct.pack('!HH',4,block))
            except Exception as error:errors.append(error)
        thread=threading.Thread(target=serve,daemon=True);thread.start()
        try:
            output=self.run_driver('source',f'tftp://127.0.0.1:{udp.getsockname()[1]}/release.itb',self.root,timeout=8).split()
            self.assertEqual(int(output[0]),0);self.assertEqual(output[1],hashlib.sha256(payload).hexdigest())
            self.assertEqual(list(self.root.iterdir()),[])
        finally:thread.join(timeout=6);udp.close()
        self.assertFalse(errors,errors)
    def test_push_resume_budget_rewind_and_bad_ack(self):
        self.path.write_bytes(b'x'*100)
        for offset,budget,rewind,bad,stuck,rc,next_ in [(26,0,0,0,0,0,100),(0,2,0,0,0,1,26),
            (26,0,1,0,0,0,100),(0,0,0,1,0,-errno.EPROTO,0),(0,0,0,0,1,-errno.EPROTO,0)]:
            result=self.run_driver('push',self.path,100,offset,budget,rewind,bad,stuck).split()
            self.assertEqual(int(result[0]),rc);self.assertEqual(int(result[1]),next_)

if __name__=='__main__':unittest.main()
