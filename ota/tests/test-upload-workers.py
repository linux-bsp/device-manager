#!/usr/bin/env python3
"""Bounded slow upload reception, ownership, completion and failure cleanup."""
import http.client
import importlib.util
import json
import os
from pathlib import Path
import socket
import signal
import sqlite3
import subprocess
import time
import unittest

spec = importlib.util.spec_from_file_location('admin', Path(__file__).with_name('test-admin.py'))
admin = importlib.util.module_from_spec(spec)
spec.loader.exec_module(admin)


class Uploads(unittest.TestCase):
    request = admin.AdminHTTP.request
    fixture = admin.AdminHTTP.fixture
    bundle = admin.AdminHTTP.bundle
    upload = admin.AdminHTTP.upload
    state = admin.AdminHTTP.state
    publish = admin.AdminHTTP.publish
    assert_no_partial = admin.AdminHTTP.assert_no_partial

    def setUp(self):
        self.held = []
        admin.AdminHTTP.setUp(self)

    def tearDown(self):
        for stream in self.held:
            stream.close()
        admin.AdminHTTP.tearDown(self)

    def slow(self):
        stream = socket.create_connection(('127.0.0.1', self.port), timeout=5)
        self.held.append(stream)
        stream.sendall(('POST /api/upload HTTP/1.1\r\nHost: localhost\r\n'
                        'X-OTA-Request: 1\r\nContent-Type: application/octet-stream\r\n'
                        f'Content-Length: {len(self.archive)}\r\n\r\n').encode() + self.archive[:1])
        return stream

    def children(self):
        return (Path('/proc') / str(self.server.pid) / 'task' / str(self.server.pid) / 'children').read_text().split()

    def wait(self, predicate):
        for _ in range(150):
            if predicate():
                return
            time.sleep(.03)
        self.fail('condition did not complete')

    def response(self, stream):
        response = http.client.HTTPResponse(stream)
        try:
            response.begin()
            return response.status, response.read()
        finally:
            response.close()

    def test_slow_uploads_are_bounded_and_control_remains_available(self):
        first, second = self.slow(), self.slow()
        self.wait(lambda: len(self.children()) == 2)
        for path in ('/healthz', '/api/state', '/api/time/' + 'a' * 32):
            started = time.monotonic()
            self.assertEqual(self.request('GET', path)[0], 200)
            self.assertLess(time.monotonic() - started, 1)
        self.assertEqual(self.upload()[0], 503)
        self.assertEqual(self.state()['drafts'], [])
        first.sendall(self.archive[1:])
        status, body = self.response(first)
        self.assertEqual(status, 201, body)
        ident = json.loads(body)['id']
        self.assertEqual(self.state()['latest'], 0)
        self.assertEqual(self.publish(ident)[0], 200)
        for name, data in self.files.items():
            self.assertEqual(self.request('GET', '/releases/1/' + name), (200, data))
        second.shutdown(socket.SHUT_WR)
        self.assertEqual(self.response(second)[0], 400)
        self.wait(lambda: not self.children())
        self.assert_no_partial()

    def test_invalid_complete_and_disconnected_upload_never_become_drafts(self):
        self.assertEqual(self.upload(b'not gzip')[0], 422)
        stream = self.slow()
        self.wait(lambda: len(self.children()) == 1)
        stream.close()
        self.wait(lambda: not self.children())
        self.assertEqual(self.state()['drafts'], [])
        self.assert_no_partial()

    def test_child_descriptor_ownership_and_graceful_stop(self):
        self.slow()
        self.wait(lambda: len(self.children()) == 1)
        child = self.children()[0]
        self.wait(lambda: len(list((Path('/proc') / child / 'fd').iterdir())) == 2)
        paths = [str(fd.readlink()) for fd in (Path('/proc') / child / 'fd').iterdir()]
        self.assertEqual(sum(p.startswith('socket:') for p in paths), 1, paths)
        self.assertTrue(any(p.endswith('package.tar.gz') for p in paths), paths)
        self.server.terminate()
        self.server.wait(timeout=2)
        self.assertFalse((Path('/proc') / child).exists())
        self.assert_no_partial()

    def test_absolute_lifetime_releases_capacity_without_publishing(self):
        self.server.terminate()
        self.server.wait(timeout=3)
        source = self.root / 'short-alarm.c'
        source.write_text('#define _GNU_SOURCE\n#include <dlfcn.h>\n#include <unistd.h>\n'
                          'unsigned alarm(unsigned n){unsigned (*real)(unsigned)=dlsym(RTLD_NEXT,"alarm");'
                          'return real(n?1:0);}\n')
        library = self.root / 'short-alarm.so'
        subprocess.run(['cc', '-shared', '-fPIC', str(source), '-ldl', '-o', str(library)], check=True)
        self.server = subprocess.Popen([str(admin.SOURCE / 'device-server'), '--root', str(self.feed),
                                        '--port', str(self.port)], env=dict(admin.ENV, LD_PRELOAD=str(library)),
                                       stdout=self.log, stderr=self.log)
        time.sleep(.15)
        self.assertIsNone(self.server.poll())
        stream = self.slow()
        self.assertEqual(self.response(stream)[0], 400)
        self.wait(lambda: not self.children())
        self.assert_no_partial()
        self.assertEqual(self.state()['drafts'], [])
        self.assertEqual(self.upload()[0], 201)

    def verifier_gate(self, phase='unpack', seconds=180, helper=False):
        """Pause real verification in the child, never via a production switch."""
        self.server.terminate()
        self.server.wait(timeout=3)
        source = self.root / 'verification-gate.c'
        source.write_text(r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static pid_t owner;
__attribute__((constructor)) static void init(void) { owner=getpid(); }
static void gate(const char *phase) {
    if(getpid()==owner || strcmp(getenv("VERIFY_PHASE"),phase))return;
    int fd=open(getenv("VERIFY_READY"),O_WRONLY|O_CREAT,0600);if(fd>=0)close(fd);
    while(access(getenv("VERIFY_ALLOW"),F_OK))usleep(10000);
}
FILE *fopen(const char *path,const char *mode) {
    FILE *(*real)(const char*,const char*)=dlsym(RTLD_NEXT,"fopen");
    if(strstr(path,".upload-") && strstr(path,"package.tar.gz") && !strcmp(mode,"rb"))gate("unpack");
    if(getpid()==owner && strstr(path,".itb") && !strcmp(mode,"rb")){
        int fd=open(getenv("VERIFY_PARENT_READ"),O_WRONLY|O_CREAT,0600);if(fd>=0)close(fd);
    }
    return real(path,mode);
}
unsigned alarm(unsigned seconds) {
    unsigned (*real)(unsigned)=dlsym(RTLD_NEXT,"alarm");
    return real(seconds?(unsigned)atoi(getenv("VERIFY_SECONDS")):0);
}
void _exit(int status) {
    void (*real)(int)=dlsym(RTLD_NEXT,"_exit");
    if(!status)gate("commit");
    real(status);__builtin_unreachable();
}
''')
        library = self.root / 'verification-gate.so'
        subprocess.run(['cc', '-shared', '-fPIC', str(source), '-ldl', '-o', str(library)], check=True)
        self.ready, self.allow = self.root / 'verify-ready', self.root / 'verify-allow'
        self.parent_read = self.root / 'parent-read'
        env = dict(admin.ENV, LD_PRELOAD=str(library), VERIFY_PHASE=phase,
                   VERIFY_READY=str(self.ready), VERIFY_ALLOW=str(self.allow),
                   VERIFY_PARENT_READ=str(self.parent_read), VERIFY_SECONDS=str(seconds))
        if helper:
            tools = self.root / 'tools'
            tools.mkdir()
            source = self.root / 'stalled-helper.c'
            self.helper_pid = self.root / 'helper-pid'
            source.write_text('#include <stdio.h>\n#include <unistd.h>\nint main(void){'
                              'FILE *f=fopen("'+str(self.helper_pid)+'","w");'
                              'fprintf(f,"%d",getpid());fclose(f);for(;;)pause();}\n')
            subprocess.run(['cc', str(source), '-o', str(tools / 'dumpimage')], check=True)
            env['PATH'] = str(tools) + ':' + env['PATH']
        self.server = subprocess.Popen([str(admin.SOURCE / 'device-server'), '--root', str(self.feed),
                                       '--port', str(self.port)], env=env, stdout=self.log, stderr=self.log)
        time.sleep(.15)
        self.assertIsNone(self.server.poll())
        stream = self.slow()
        stream.sendall(self.archive[1:])
        self.wait(self.ready.exists)
        return stream

    def test_slow_verifier_keeps_controls_available_and_parent_uses_receipt(self):
        stream = self.verifier_gate()
        self.assertEqual(len(self.children()), 1)
        worker = self.children()[0]
        paths = [str(p.readlink()) for p in (Path('/proc') / worker / 'fd').iterdir()]
        self.assertFalse(any('socket:' in p or '.db' in p for p in paths), paths)
        for path in ('/healthz', '/api/state', '/api/time/' + 'b' * 32):
            started = time.monotonic()
            self.assertEqual(self.request('GET', path)[0], 200)
            self.assertLess(time.monotonic() - started, 1)
        self.assertEqual(self.state()['drafts'], [])
        with sqlite3.connect(self.feed / 'firmware.db') as db:
            self.assertEqual(db.execute('SELECT COUNT(*) FROM firmware_identity').fetchone()[0], 0)
        self.allow.touch()
        status, body = self.response(stream)
        self.assertEqual(status, 201, body)
        draft = self.state()['drafts'][0]
        self.assertEqual(draft['id'], json.loads(body)['id'])
        self.assertEqual(draft['tag'], 'v1.0.1')
        self.assertEqual(len(draft['firmware_id']), 64)
        self.assertFalse(self.parent_read.exists(), 'parent re-read payload bytes')
        self.assert_no_partial()

    def test_verification_deadline_cleans_staging_and_releases_slot(self):
        stream = self.verifier_gate(seconds=1)
        self.assertEqual(self.response(stream)[0], 400)
        self.wait(lambda: not self.children())
        self.assertEqual(self.state()['drafts'], [])
        self.assert_no_partial()
        self.allow.touch()
        self.assertEqual(self.upload()[0], 201)

    def test_changed_bytes_after_verification_are_rejected(self):
        stream = self.verifier_gate(phase='commit')
        rootfs = next((self.feed / 'drafts').glob('.upload-*/rootfs.itb'))
        with rootfs.open('r+b') as image:
            image.seek(-1, 2)
            value = image.read(1)[0]
            image.seek(-1, 2)
            image.write(bytes([value ^ 1]))
        self.allow.touch()
        self.assertEqual(self.response(stream)[0], 422)
        self.assertEqual(self.state()['drafts'], [])
        self.assert_no_partial()
        with sqlite3.connect(self.feed / 'firmware.db') as db:
            self.assertEqual(db.execute('SELECT COUNT(*) FROM firmware_identity').fetchone()[0], 0)

    def test_stop_during_verification_never_exposes_a_draft(self):
        self.verifier_gate()
        self.server.terminate()
        self.server.wait(timeout=3)
        self.assert_no_partial()
        self.assertEqual(list((self.feed / 'drafts').glob('draft-*')), [])

    def test_terminated_verifier_reaps_fit_helper_and_large_scratch(self):
        stream = self.verifier_gate(helper=True)
        worker = int(self.children()[0])
        self.allow.touch()
        self.wait(self.helper_pid.exists)
        helper = int(self.helper_pid.read_text())
        self.assertTrue(list((self.feed / 'drafts').glob('.upload-*/.identity-*')))
        os.kill(worker, signal.SIGKILL)
        self.assertEqual(self.response(stream)[0], 400)
        def stopped():
            stat = Path('/proc') / str(helper) / 'stat'
            return not stat.exists() or stat.read_text().split()[2] == 'Z'
        self.wait(stopped)
        self.assert_no_partial()
        self.assertEqual(self.state()['drafts'], [])


if __name__ == '__main__':
    unittest.main()
