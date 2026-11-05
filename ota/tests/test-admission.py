#!/usr/bin/env python3
"""Bounded slow-header admission against an isolated real HTTP server."""
import http.client
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]

class Admission(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='dm-admission-')
        self.process = subprocess.Popen([str(ROOT/'device-server'), '--root', self.tmp.name,
            '--port', '0', '--sol-debug-no-auth'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.port = int(self.process.stdout.readline().split()[2].split(':')[-1].split('/')[0])
        self.sockets = []
    def tearDown(self):
        for stream in self.sockets: stream.close()
        if self.process.poll() is None: self.process.terminate()
        self.process.communicate(timeout=3)
        self.assertEqual(self.process.returncode, 0)
        self.tmp.cleanup()
    def connect(self, source='127.0.0.1'):
        stream = socket.socket()
        stream.settimeout(3)
        stream.bind((source, 0)); stream.connect(('127.0.0.1', self.port))
        self.sockets.append(stream)
        return stream
    def response(self, stream):
        response = http.client.HTTPResponse(stream); response.begin()
        status = response.status; body = response.read(); response.close()
        return status, body
    def get(self, path='/api/state', source='127.0.0.1'):
        stream = self.connect(source)
        try:
            stream.sendall(('GET '+path+' HTTP/1.1\r\nHost: localhost\r\n\r\n').encode())
            return self.response(stream)
        finally: stream.close()
    def test_idle_and_fragmented_clients_do_not_block_control_requests(self):
        for i in range(12):
            stream = self.connect()
            if i % 2: stream.sendall(b'GET / HTTP/1.1\r\nHost:')
        started = time.monotonic()
        for path in ['/api/state', '/api/sol/devices', '/api/time/'+'a'*32]:
            self.assertEqual(self.get(path)[0], 200)
        self.assertLess(time.monotonic() - started, 1.5)
    def test_fragment_terminator_does_not_consume_binary_body(self):
        stream = self.connect()
        header = b'POST /api/sol/push/device-a/boot-a/1/kernel/1/0 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r'
        for offset in range(0, len(header), 13):
            stream.sendall(header[offset:offset+13]); time.sleep(.002)
        stream.sendall(b'\na\x00b\nc')
        status, body = self.response(stream)
        self.assertEqual(status, 200, body)
        self.assertEqual(json.loads(body)['next'], 5)
    def test_bad_header_and_oversize_are_rejected(self):
        for header, expected in [(b'GET / HTTP/1.1\r\nBad:\x00value\r\n\r\n', 400),
                (b'GET / HTTP/1.1\r\nX: '+b'a'*8192, 431),
                (b'GET / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n', 400)]:
            stream = self.connect(); stream.sendall(header)
            self.assertEqual(self.response(stream)[0], expected)
        self.assertEqual(self.get()[0], 200)
    def test_per_peer_and_global_admission_limits(self):
        for _ in range(16): self.connect()
        self.assertEqual(self.get()[0], 503)
        self.assertEqual(self.get(source='127.0.0.2')[0], 200)
        for _ in range(16): self.connect('127.0.0.2')
        self.assertEqual(self.get(source='127.0.0.3')[0], 503)
        for stream in self.sockets: stream.close()
        self.sockets.clear()
        time.sleep(.1)
        self.assertEqual(self.get()[0], 200)
    def test_absolute_deadline_is_not_renewed_by_drip(self):
        stream = self.connect(); stream.settimeout(18)
        started = time.monotonic()
        stream.sendall(b'GET / HTTP/1.1\r\nX: ')
        time.sleep(8)
        stream.sendall(b'keep waiting')
        self.assertEqual(self.response(stream)[0], 408)
        self.assertLess(time.monotonic() - started, 17)
        self.assertEqual(self.get()[0], 200)
    def test_disconnect_churn_does_not_leak_descriptors(self):
        # Catalog initialization deliberately retains DB/WAL/SHM descriptors.
        self.assertEqual(self.get()[0], 200)
        time.sleep(.02)
        initial = len(list(Path(f'/proc/{self.process.pid}/fd').iterdir()))
        for _ in range(100):
            stream = self.connect(); stream.sendall(b'GET /'); stream.close()
        self.sockets.clear(); time.sleep(.2)
        self.assertEqual(self.get()[0], 200)
        time.sleep(.1)
        final = len(list(Path(f'/proc/{self.process.pid}/fd').iterdir()))
        self.assertLessEqual(final, initial + 2)
    def test_stop_does_not_wait_for_incomplete_headers(self):
        for _ in range(10): self.connect()
        started = time.monotonic(); self.process.terminate()
        self.process.communicate(timeout=2)
        self.assertLess(time.monotonic() - started, 2)

if __name__ == '__main__': unittest.main()
