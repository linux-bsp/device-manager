#!/usr/bin/env python3
"""FIT HWID declarations persist across refreshes and old-catalog migration."""
import importlib.util
from pathlib import Path
import shutil
import sqlite3
import subprocess
import time
import unittest

spec = importlib.util.spec_from_file_location('admin', Path(__file__).with_name('test-admin.py'))
admin = importlib.util.module_from_spec(spec)
spec.loader.exec_module(admin)
HWID = '1010001000110000'
OTHER = '1010002000110000'


class Hardware(unittest.TestCase):
    def setUp(self):
        self.http = admin.AdminHTTP()
        self.http.setUp()
        self.addCleanup(self.http.tearDown)
        self.counter = self.http.root / 'calls'
        tools = self.http.root / 'tools'
        tools.mkdir()
        for name in ('fdtget', 'sha256sum', 'dumpimage'):
            path = tools / name
            path.write_text(f'#!/bin/sh\necho {name} >> "{self.counter}"\nexec "{shutil.which(name, path=admin.ENV["PATH"])}" "$@"\n')
            path.chmod(0o755)
        self.env = dict(admin.ENV, PATH=str(tools) + ':' + admin.ENV['PATH'])
        self.restart()

    def restart(self):
        h = self.http
        h.server.terminate()
        h.server.wait(timeout=10)
        h.log.seek(0)
        h.log.truncate()
        h.server = subprocess.Popen([str(admin.SOURCE / 'device-server'), '--root', str(h.feed), '--port', '0'],
                                    env=self.env, stdout=h.log, stderr=h.log)
        for _ in range(200):
            text = (h.root / 'server.log').read_text()
            if text.startswith('Device Manager: '):
                h.port = int(text.split()[2].split(':')[2].split('/')[0])
                return
            self.assertIsNone(h.server.poll(), text)
            time.sleep(.02)
        self.fail('server did not restart')

    def bundle(self, hwids=None, raw=None):
        files = self.http.fixture(version=1, hwid=hwids if hwids is not None and raw is None else None)
        fit = self.http.root / 'hardware-release.itb'
        fit.write_bytes(files['release.itb'])
        if hwids is not None or raw is not None:
            arguments = ['fdtput', '-t', 's' if raw is None else 'bx', str(fit), '/configurations/conf-1', 'fw,hwids']
            arguments += hwids if raw is None else [f'{byte:x}' for byte in raw]
            subprocess.run(arguments, env=admin.ENV, check=True, capture_output=True)
        if hwids is None and raw is None:
            pass
        files['release.itb'] = fit.read_bytes()
        return self.http.bundle(files)

    def test_single_and_multiple_hardware_ids_persist_in_drafts_and_releases(self):
        h = self.http
        ident = h.draft(self.bundle([HWID, OTHER]))
        self.assertEqual(h.publish(ident)[0], 200)
        for item in h.state()['drafts'] + h.state()['releases']:
            self.assertEqual(item['hwids'], [HWID, OTHER])
        self.restart()
        self.counter.write_text('')
        self.assertEqual(h.state()['releases'][0]['hwids'], [HWID, OTHER])
        self.assertEqual(self.counter.read_text(), '')
        with sqlite3.connect(h.feed / 'firmware.db') as db:
            self.assertEqual(db.execute('SELECT hwids FROM firmware_hardware').fetchall(),
                             [(HWID + ' ' + OTHER,), (HWID + ' ' + OTHER,)])

    def test_old_catalog_backfills_without_rehashing_firmware(self):
        h = self.http
        ident = h.draft(self.bundle([HWID]))
        h.publish(ident)
        with sqlite3.connect(h.feed / 'firmware.db') as db:
            db.execute('DROP TABLE firmware_hardware')
        self.restart()
        self.counter.write_text('')
        self.assertEqual(h.state()['releases'][0]['hwids'], [HWID])
        calls = self.counter.read_text().splitlines()
        self.assertTrue(calls)
        self.assertEqual(set(calls), {'fdtget'})
        self.counter.write_text('')
        h.state()
        self.assertEqual(self.counter.read_text(), '')

    def test_undeclared_firmware_cannot_be_uploaded_or_published(self):
        self.assertEqual(self.http.upload(self.bundle())[0], 422)
        self.assertEqual(self.http.state()['drafts'], [])

    def test_malformed_duplicate_and_noncanonical_hardware_are_rejected_before_draft_commit(self):
        for values in ([HWID, HWID], ['2010001000110000'], ['1010001000110001'],
                       ['101000100011A000'], [HWID + ' ' + OTHER], ['<script>']):
            with self.subTest(values=values):
                self.assertEqual(self.http.upload(self.bundle(values))[0], 422)
                self.assertEqual(self.http.state()['drafts'], [])
                self.http.assert_no_partial()
        self.assertEqual(self.http.upload(self.bundle(raw=HWID.encode()))[0], 422)

    def test_allowlist_boundary_is_not_silently_truncated(self):
        values = [f'101{i:04x}000110000' for i in range(1, 258)]
        ident = self.http.draft(self.bundle(values[:256]))
        self.assertEqual(self.http.state()['drafts'][0]['hwids'], values[:256])
        self.assertEqual(self.http.upload(self.bundle(values))[0], 422)
        self.assertEqual([x['id'] for x in self.http.state()['drafts']], [ident])


if __name__ == '__main__':
    unittest.main()
