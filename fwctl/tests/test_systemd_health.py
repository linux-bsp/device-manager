#!/usr/bin/env python3
"""Exercise real bounded systemctl adapter responses; no host service changes."""
import ctypes
import errno
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
ROOT = Path(__file__).resolve().parents[1]
class SystemdHealth(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.path = Path(cls.tmp.name)
        cls.mock = cls.path / 'systemctl'
        library = cls.path / 'health.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                        '-DFWCTL_SYSTEMCTL="'+str(cls.mock)+'"',
                        str(ROOT/'health/fwctl_systemd.c'), '-o', str(library)], check=True)
        cls.library = ctypes.CDLL(str(library))
        cls.query = cls.library.fwctl_systemd_unit_health
        cls.query.argtypes = [ctypes.c_char_p]
        cls.query.restype = ctypes.c_int
    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()
    def script(self, body):
        self.mock.write_text('#!/bin/sh\n'+body+'\n'); self.mock.chmod(0o755)
    def test_active(self):
        self.script("printf '%s\\n' LoadState=loaded ActiveState=active SubState=running Result=success")
        self.assertEqual(self.query(b'network.service'), 0)
    def test_pending(self):
        self.script("printf '%s\\n' LoadState=loaded ActiveState=activating")
        self.assertEqual(self.query(b'network.service'), -errno.EAGAIN)
    def test_failed(self):
        self.script("printf '%s\\n' LoadState=loaded ActiveState=failed")
        self.assertEqual(self.query(b'network.service'), -errno.EIO)
    def test_missing(self):
        self.script("printf '%s\\n' LoadState=not-found ActiveState=inactive")
        self.assertEqual(self.query(b'network.service'), -errno.EIO)
    def test_invalid_unit(self):
        self.assertEqual(self.query(b'--version'), -errno.EINVAL)
        self.assertEqual(self.query(b'x; reboot'), -errno.EINVAL)
    def test_timeout(self):
        self.script('exec sleep 10')
        start=time.monotonic()
        self.assertEqual(self.query(b'network.service'), -errno.ETIMEDOUT)
        self.assertLess(time.monotonic()-start, 1.5)
    def test_partial_output_not_healthy(self):
        self.script("printf '%s\\n' ActiveState=active")
        self.assertEqual(self.query(b'network.service'), -errno.EAGAIN)
if __name__ == '__main__': unittest.main()
