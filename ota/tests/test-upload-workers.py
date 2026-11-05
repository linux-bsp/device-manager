#!/usr/bin/env python3
"""Bounded slow upload reception, ownership, completion and failure cleanup."""
import http.client
import importlib.util
import json
import os
from pathlib import Path
import socket
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


if __name__ == '__main__':
    unittest.main()
