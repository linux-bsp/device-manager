#!/usr/bin/env python3
"""Exercise real daemons with failed file/directory flushes; no block writes."""
import errno
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
import unittest

SOURCE = Path(__file__).resolve().parents[1]


class StorageSync(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = tempfile.TemporaryDirectory(prefix='dm-sync-fixture-')
        cls.preload = Path(cls.fixture.name)/'fault.so'
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fPIC', '-shared',
                        str(SOURCE/'tests/fixtures/sync_fault.c'), '-ldl',
                        '-o', str(cls.preload)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.fixture.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='dm-sync-')
        self.root = Path(self.tmp.name)
        self.marker = self.root/'fail-sync'
        self.env = dict(os.environ, LD_PRELOAD=str(self.preload),
                        DM_TEST_SYNC_MARKER=str(self.marker))

    def tearDown(self):
        self.tmp.cleanup()

    def wait(self, predicate, timeout=9):
        until = time.monotonic()+timeout
        while time.monotonic() < until:
            if predicate():
                return
            time.sleep(.05)
        self.fail('Timed out waiting for sync status')

    def status(self, runtime):
        try:
            return dict(line.split('=', 1) for line in (runtime/'sync-status').read_text().splitlines())
        except FileNotFoundError:
            return {}

    def test_logging_sync_error_preserves_watermark_and_recovers(self):
        for error in (errno.EIO, errno.ENOSPC, errno.EROFS):
            with self.subTest(error=error):
                base = self.root/str(error);base.mkdir()
                store = base/'boots';store.mkdir()
                services = base/'services';services.mkdir()
                runtime = base/'run'
                env = dict(self.env, LOGCTL_RUN_DIR=str(runtime),
                           LOGCTL_BOOT_ID='sync-test', LOGCTL_SERVICE_DIR=str(services),
                           LOGCTL_SOL_CONFIG=str(base/'no-sol'), DM_TEST_SYNC_PATH=str(store),
                           DM_TEST_SYNC_SUFFIX='.log', DM_TEST_SYNC_ERRNO=str(error))
                with (base/'stderr').open('w') as stderr:
                    proc = subprocess.Popen([str(SOURCE/'logging/logctld'), '--root', str(store)],
                                            env=env, stdout=subprocess.DEVNULL, stderr=stderr)
                    try:
                        self.wait(lambda: self.status(runtime).get('sync-state') == 'ok')
                        before = self.status(runtime)['synced-sequence']
                        self.marker.touch()
                        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sock:
                            sock.sendto(b'services'.ljust(16, b'\0')+b'unsynced-body\n', str(runtime/'socket'))
                        self.wait(lambda: self.status(runtime).get('sync-state') == 'error')
                        status = self.status(runtime)
                        self.assertEqual(status['synced-sequence'], before)
                        self.assertGreater(int(status['observed-sequence']), int(before))
                        self.assertEqual(int(status['sync-errno']), error)
                        result = subprocess.run([str(SOURCE/'logging/logctl'), 'status'],
                                                env=env, capture_output=True, text=True, check=True)
                        self.assertIn('sync-state=error', result.stdout)
                        self.marker.unlink()
                        self.wait(lambda: self.status(runtime).get('sync-state') == 'ok')
                        self.assertGreater(int(self.status(runtime)['synced-sequence']), int(before))
                    finally:
                        self.marker.unlink(missing_ok=True)
                        proc.terminate();proc.wait(timeout=5)
                        self.assertEqual(proc.returncode, 0)
                self.assertIn('local persistence unconfirmed', (base/'stderr').read_text())
                self.assertIn('synchronization recovered', (base/'stderr').read_text())

    def test_download_sync_failure_never_promotes_current(self):
        payloads = {name: (name+' fixture\n').encode() for name in ('release.itb', 'kernel.itb', 'rootfs.itb')}
        manifest = b'BSPOTA1 2 test test\n'+b''.join(
            f'{name} {len(body)} {hashlib.sha256(body).hexdigest()}\n'.encode()
            for name, body in payloads.items())
        paths = {'/latest': manifest, '/releases/2/version': b'BSPOTAVERSION1 2 v0.0.1 '+b'a'*64+b'\n'}
        paths.update({'/releases/2/'+name: body for name, body in payloads.items()})

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                body = paths.get(self.path)
                self.send_response(200 if body else 404);self.end_headers()
                if body:self.wfile.write(body)

            def log_message(self, *args):
                pass

        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        worker = threading.Thread(target=server.serve_forever, daemon=True);worker.start()
        try:
            for suffix in ('manifest', 'kernel.itb', '.download-2', 'cache'):
                for error in (errno.EIO, errno.ENOSPC, errno.EROFS):
                    with self.subTest(suffix=suffix, error=error):
                        cache = self.root/(suffix.replace('.', '')+'-'+str(error))/'cache'
                        cache.mkdir(parents=True)
                        # A complete staged file still requires a successful flush before reuse.
                        if suffix == 'kernel.itb':
                            stage = cache/'.download-2';stage.mkdir()
                            (stage/suffix).write_bytes(payloads[suffix])
                        previous = manifest.replace(b'BSPOTA1 2 ', b'BSPOTA1 1 ', 1)
                        (cache/'current').write_bytes(previous)
                        env = dict(self.env, DM_TEST_SYNC_PATH=str(cache),
                                   DM_TEST_SYNC_SUFFIX=suffix, DM_TEST_SYNC_ERRNO=str(error))
                        args = [str(SOURCE/'ota/fwctld'), '--server',
                                f'http://127.0.0.1:{server.server_port}', '--cache', str(cache),
                                '--product', 'test', '--layout', 'test', '--once']
                        self.marker.touch()
                        result = subprocess.run(args, env=env, capture_output=True, text=True, timeout=8)
                        self.assertNotEqual(result.returncode, 0, result.stdout+result.stderr)
                        self.assertEqual((cache/'current').read_bytes(), previous)
                        self.assertNotIn('Ready: firmware', result.stdout)
                        self.marker.unlink()
                        result = subprocess.run(args, env=env, capture_output=True, text=True, timeout=8)
                        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                        self.assertEqual((cache/'current').read_bytes(), manifest)
        finally:
            server.shutdown();worker.join();server.server_close()


if __name__ == '__main__':
    unittest.main()
