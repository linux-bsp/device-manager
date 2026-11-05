#!/usr/bin/env python3
"""Queue complete control and bulk requests together; slow export cannot jump ahead."""
import http.client
import importlib.util
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import time
import unittest

spec = importlib.util.spec_from_file_location('admin', Path(__file__).with_name('test-admin.py'))
admin = importlib.util.module_from_spec(spec)
spec.loader.exec_module(admin)


class Scheduling(unittest.TestCase):
    fixture = admin.AdminHTTP.fixture
    bundle = admin.AdminHTTP.bundle
    request = admin.AdminHTTP.request
    upload = admin.AdminHTTP.upload
    draft = admin.AdminHTTP.draft
    publish = admin.AdminHTTP.publish

    def setUp(self):
        self.streams = []
        admin.AdminHTTP.setUp(self)
        self.assertEqual(self.publish(self.draft())[0], 200)
        self.gate = self.root / 'export-allowed'
        self.started = self.root / 'export-started'
        wrappers = self.root / 'tools'
        wrappers.mkdir()
        real = shutil.which('tar', path=admin.ENV['PATH'])
        wrapper = wrappers / 'tar'
        wrapper.write_text(f'#!/bin/sh\ntouch "{self.started}"\n'
                           f'while [ ! -e "{self.gate}" ]; do sleep .01; done\n'
                           f'exec "{real}" "$@"\n')
        wrapper.chmod(0o755)
        self.server.terminate()
        self.server.wait(timeout=3)
        self.server = subprocess.Popen([str(admin.SOURCE / 'device-server'), '--root', str(self.feed),
                                        '--port', str(self.port)], stdout=self.log, stderr=self.log,
                                       env=dict(admin.ENV, PATH=str(wrappers) + ':' + admin.ENV['PATH']))
        time.sleep(.15)
        self.assertIsNone(self.server.poll())

    def tearDown(self):
        self.gate.touch()
        if self.server.poll() is None:
            self.server.send_signal(signal.SIGCONT)
        for stream in self.streams:
            stream.close()
        admin.AdminHTTP.tearDown(self)

    def connect(self, method, path, body=b''):
        stream = socket.create_connection(('127.0.0.1', self.port), timeout=2)
        self.streams.append(stream)
        stream.sendall((f'{method} {path} HTTP/1.1\r\nHost: localhost\r\n'
                        'X-OTA-HWID: 1010001000110000\r\nX-OTA-Request: 1\r\n'
                        f'Content-Length: {len(body)}\r\n').encode())
        return stream, b'\r\n' + body

    def socket_count(self):
        count = 0
        for fd in (Path('/proc') / str(self.server.pid) / 'fd').iterdir():
            try:
                count += str(fd.readlink()).startswith('socket:')
            except FileNotFoundError:
                pass
        return count

    def response(self, stream):
        response = http.client.HTTPResponse(stream)
        try:
            response.begin()
            return response.status, response.read()
        finally:
            response.close()

    def batch(self, controls):
        baseline = self.socket_count()
        bulk = [self.connect('GET', '/api/download/release/1') for _ in range(3)]
        priority = [self.connect(*request) for request in controls]
        for _ in range(100):
            if self.socket_count() == baseline + len(bulk) + len(priority):
                break
            time.sleep(.01)
        else:
            self.fail('pending clients were not accepted')
        self.server.send_signal(signal.SIGSTOP)
        # SIGSTOP is asynchronous: wait until the fixture process is stopped.
        for _ in range(100):
            if '\nState:\tT' in (Path('/proc') / str(self.server.pid) / 'status').read_text():
                break
            time.sleep(.01)
        else:
            self.fail('fixture did not stop')
        for stream, suffix in bulk + priority:
            stream.sendall(suffix)
        self.server.send_signal(signal.SIGCONT)
        try:
            # Any export dispatched first waits on gate and makes this fail.
            for stream, _ in priority:
                self.assertEqual(self.response(stream)[0], 200)
        finally:
            self.gate.touch()
        for stream, _ in bulk:
            status, body = self.response(stream)
            self.assertEqual(status, 200)
            self.assertTrue(body.startswith(b'\x1f\x8b'))
        self.assertTrue(self.started.exists())

    def test_device_control_and_result_precede_bulk(self):
        report = b'BSPOTASTATE3 board-a 1010001000110000 1 65537 boot-one success 1 65537 none\n'
        self.batch([('GET', '/healthz'), ('GET', '/api/time/' + 'a' * 32),
                    ('GET', '/api/policy/board-a/1'), ('POST', '/api/devices/board-a', report),
                    ('GET', '/releases/1/identity')])

    def test_management_query_precedes_bulk_without_starving_exports(self):
        self.batch([('GET', '/api/state')])


if __name__ == '__main__':
    unittest.main()
