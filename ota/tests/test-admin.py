#!/usr/bin/env python3
"""Host HTTP tests. Synthetic FIT metadata; no flashing or external devices."""
import gzip
import http.client
import hashlib
import io
import json
import os
from pathlib import Path
import socket
import subprocess
import tarfile
import tempfile
import time
import unittest

SOURCE = Path(__file__).resolve().parents[1]
HOST = SOURCE.parents[1] / 'output/am625/host/bin'
ENV = dict(os.environ, PATH=str(HOST) + ':' + os.environ['PATH'])


class AdminHTTP(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='ota-admin-test-')
        self.root = Path(self.tmp.name)
        self.feed = self.root / 'feed'
        self.feed.mkdir()
        self.log = (self.root / 'server.log').open('w')
        self.server = subprocess.Popen([str(SOURCE / 'device-server'), '--root', str(self.feed), '--port', '0'],
                                       env=ENV, stdout=self.log, stderr=self.log)
        for _ in range(100):
            text = (self.root / 'server.log').read_text()
            if text.startswith('Device Manager: '):
                self.port = int(text.split()[2].split(':')[2].split('/')[0])
                break
            self.assertIsNone(self.server.poll(), text)
            time.sleep(.02)
        else:
            self.fail('Server did not start')
        self.files = self.fixture()
        self.archive = self.bundle(self.files)

    def tearDown(self):
        self.server.terminate()
        self.server.wait(timeout=10)
        self.log.close()
        self.tmp.cleanup()

    def fixture(self, version=65537, product='ti-am62x', bad_hash=False):
        kernel_dts = f'/dts-v1/; / {{ description="kernel"; configurations {{ default="c"; c {{ fw,version=<{version}>; }}; }}; }};'
        kernel_fit = subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb'], input=kernel_dts.encode(), capture_output=True, check=True, env=ENV).stdout
        raw = self.root / 'rootfs-payload'
        raw.write_bytes(bytes(range(256)) * 1100)
        root_dts = f'''/dts-v1/; / {{ description="rootfs"; timestamp=<0>; #address-cells=<1>;
images {{ rootfs {{ description="rootfs"; data=/incbin/("{raw}"); type="firmware"; arch="arm64"; compression="none"; }}; }};
configurations {{ default="c"; c {{ firmware="rootfs"; }}; }}; }};'''
        root_fit = subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb'], input=root_dts.encode(), capture_output=True, check=True, env=ENV).stdout
        files = {'kernel.itb': kernel_fit, 'rootfs.itb': root_fit}
        kernel = hashlib.sha256(files['kernel.itb']).hexdigest()
        rootfs = hashlib.sha256(files['rootfs.itb']).hexdigest()
        if bad_hash:
            rootfs = '0' * 64
        dts = f'''/dts-v1/;
/ {{ configurations {{ default = "conf-1";
conf-1 {{ fw,type = "release"; fw,version = <{version}>;
fw,product = "{product}"; fw,layout-id = "am62x-sd-ab-v1";
fw,components = "kernel", "rootfs";
fw,kernel-file = "kernel.itb"; fw,rootfs-file = "rootfs.itb";
fw,kernel-sha256 = "{kernel}"; fw,rootfs-sha256 = "{rootfs}";
}}; }}; }};
'''
        result = subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb'], input=dts.encode(),
                                capture_output=True, check=True, env=ENV)
        files['release.itb'] = result.stdout
        return files

    def bundle(self, files, extra=(), fmt=tarfile.PAX_FORMAT):
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode='w:gz', format=fmt) as archive:
            for name, data in list(files.items()) + list(extra):
                if isinstance(data, tarfile.TarInfo):
                    archive.addfile(data)
                else:
                    info = tarfile.TarInfo(name)
                    info.size = len(data)
                    info.mtime = 1234567.123 if fmt == tarfile.PAX_FORMAT else 1234567
                    archive.addfile(info, io.BytesIO(data))
        return buf.getvalue()

    def request(self, method, path, body=None, headers=None):
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=15)
        connection.request(method, path, body=body, headers=headers or {})
        result = connection.getresponse()
        data = result.read()
        status = result.status
        connection.close()
        return status, data

    def upload(self, archive=None, **headers):
        fields = {'X-OTA-Request': '1', 'Content-Type': 'application/octet-stream'}
        fields.update(headers)
        return self.request('POST', '/api/upload', self.archive if archive is None else archive, fields)

    def state(self):
        status, body = self.request('GET', '/api/state')
        self.assertEqual(status, 200, body)
        return json.loads(body)

    def publish(self, ident):
        return self.request('POST', '/api/publish/' + ident, headers={'X-OTA-Request': '1'})

    def draft(self, archive=None):
        status, body = self.upload(archive)
        self.assertEqual(status, 201, body)
        return json.loads(body)['id']

    def assert_no_partial(self):
        self.assertEqual(list((self.feed / 'drafts').glob('.upload-*')), [])

    def test_small_packed_version_tags(self):
        for value, tag in ((1, 'v0.0.1'), (256, 'v0.1.0')):
            ident = self.draft(self.bundle(self.fixture(version=value)))
            draft = next(x for x in self.state()['drafts'] if x['id'] == ident)
            self.assertEqual(draft['firmware_version'], tag[1:])
            self.assertEqual(draft['tag'], tag)
            status, body = self.publish(ident)
            self.assertEqual(status, 200, body)
            publication = json.loads(body)['publication']
            entry = next(x for x in self.state()['releases'] if x['publication'] == publication)
            self.assertEqual(entry['tag'], tag)
            self.assertEqual(self.request('GET', f'/releases/{publication}/version')[1].decode().split()[2], tag)

    def test_upload_then_explicit_publish_and_agent(self):
        ident = self.draft()
        state = self.state()
        self.assertEqual(state['latest'], 0)
        self.assertEqual(state['drafts'][0]['firmware_version'], '1.0.1')
        self.assertEqual(state['drafts'][0]['publication'], 0)
        self.assertEqual(self.request('GET', '/latest')[0], 404)
        self.assertEqual(self.request('GET', '/drafts/' + ident + '/release.itb')[0], 404)
        status, body = self.publish(ident)
        self.assertEqual(status, 200, body)
        self.assertEqual(json.loads(body)['publication'], 1)
        self.assertEqual(self.state()['drafts'][0]['publication'], 1)
        self.assertEqual(self.state()['latest'], 1)
        status, body = self.publish(ident)
        self.assertEqual(status, 200, body)
        self.assertEqual(json.loads(body)['publication'], 1)
        self.assertEqual(len(self.state()['releases']), 1)
        for name, content in self.files.items():
            self.assertEqual(self.request('GET', '/releases/1/' + name), (200, content))
        subprocess.run([str(SOURCE / 'fwctld'), '--server', f'http://127.0.0.1:{self.port}',
                        '--cache', str(self.root / 'cache'), '--product', 'ti-am62x',
                        '--layout', 'am62x-sd-ab-v1', '--once'], env=ENV, check=True, capture_output=True)
        for name, content in self.files.items():
            self.assertEqual((self.root / 'cache/1' / name).read_bytes(), content)
        newer = self.draft(self.bundle(self.fixture(version=65538)))
        self.assertEqual(self.state()['latest'], 1)
        status, body = self.publish(newer)
        self.assertEqual(status, 200, body)
        self.assertEqual(json.loads(body)['publication'], 2)
        self.assertEqual(self.state()['latest'], 2)
        self.assert_no_partial()

    def test_corrupt_missing_duplicate_traversal_and_links(self):
        malicious = [b'not gzip', self.archive[:-9], self.bundle(self.fixture(bad_hash=True)),
                     self.bundle({'release.itb': self.files['release.itb']}),
                     self.bundle(self.files, [('release.itb', self.files['release.itb'])]),
                     self.bundle(self.files, [('../escape', b'no')]),
                     self.bundle(self.files, [('/tmp/ota-escape', b'no')])]
        for kind in [tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.FIFOTYPE]:
            link = tarfile.TarInfo('kernel.itb')
            link.type = kind
            link.linkname = '/etc/passwd'
            malicious.append(self.bundle({'release.itb': self.files['release.itb'], 'rootfs.itb': self.files['rootfs.itb']}, [('kernel.itb', link)]))
        huge = tarfile.TarInfo('kernel.itb')
        huge.size = 512 * 1024 * 1024 + 1
        malicious.append(gzip.compress(huge.tobuf(format=tarfile.USTAR_FORMAT) + bytes(1024)))
        path_override = tarfile.TarInfo('extra')
        path_override.size = 0
        path_override.pax_headers = {'path': '../escape'}
        malicious.append(self.bundle(self.files, [('extra', path_override)]))
        for i, archive in enumerate(malicious):
            with self.subTest(case=i):
                status, body = self.upload(archive)
                self.assertEqual(status, 422, body)
                self.assertEqual(self.state()['drafts'], [])
                self.assert_no_partial()
        self.assertFalse((self.root / 'escape').exists())
        self.assertEqual(self.state()['latest'], 0)

    def test_browser_request_guards_and_size(self):
        self.assertEqual(self.request('POST', '/api/upload', self.archive,
                         {'Content-Type': 'application/octet-stream'})[0], 403)
        self.assertEqual(self.upload(Origin='http://another-site.invalid')[0], 403)
        self.assertEqual(self.upload(Origin='null')[0], 403)
        self.assertEqual(self.upload(**{'Content-Type': 'multipart/form-data'})[0], 415)
        self.assertEqual(self.request('POST', '/api/upload', b'',
                         {'X-OTA-Request': '1', 'Content-Type': 'application/octet-stream'})[0], 411)
        self.assertEqual(self.request('POST', '/api/upload', b'',
                         {'X-OTA-Request': '1', 'Content-Type': 'application/octet-stream',
                          'Content-Length': str(512 * 1024 * 1024 + 1)})[0], 413)
        status, body = self.upload(Origin=f'http://127.0.0.1:{self.port}')
        self.assertEqual(status, 201, body)

    def test_raw_header_and_interrupted_body(self):
        requests = [
            b'POST /api/upload HTTP/1.1\r\nHost: localhost\r\nX-OTA-Request: 1\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\n',
            b'POST /api/upload HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n',
            b'POST /api/upload HTTP/1.1\r\nHost: localhost\r\nX-OTA-Request: 1\r\nContent-Length: -1\r\n\r\n',
            b'POST /api/upload HTTP/1.1\r\nHost: localhost\r\nX-OTA-Request: 1\r\nContent-Type: application/octet-stream\r\nContent-Length: 20\r\n\r\nabc',
        ]
        for raw in requests:
            with socket.create_connection(('127.0.0.1', self.port), timeout=5) as conn:
                conn.sendall(raw)
                conn.shutdown(socket.SHUT_WR)
                reply = conn.recv(4096)
                self.assertIn(b'400', reply.split(b'\r\n')[0])
        self.assert_no_partial()
        self.assertEqual(self.request('GET', '/healthz'), (200, b'ok\n'))

    def test_wrong_product_rejected_and_explicit_older_target_published(self):
        self.assertEqual(self.publish(self.draft())[0], 200)
        for archive in [self.bundle(self.fixture(product='imx6ull'))]:
            ident = self.draft(archive)
            self.assertEqual(self.publish(ident)[0], 409)
            self.assertEqual(self.state()['latest'], 1)
        self.assertEqual(self.publish(self.draft(self.bundle(self.fixture(version=65536))))[0], 200)
        self.assertEqual(self.state()['latest'], 2)
        self.assertEqual(self.publish('../escape')[0], 404)
        self.assertEqual(self.publish('draft-AAAAAA')[0], 404)

    def test_page_and_no_download_links(self):
        status, body = self.request('GET', '/')
        self.assertEqual(status, 200)
        self.assertIn('上传并校验'.encode(), body)
        self.assertIn('发布到设备'.encode(), body)
        self.assertNotIn(b'href="/releases/', body)
        self.assertEqual(self.request('HEAD', '/'), (200, b''))


if __name__ == '__main__':
    unittest.main(verbosity=2)
