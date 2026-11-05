#!/usr/bin/env python3
"""Execute the board rcS in an isolated filesystem using BusyBox ash."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

BOARD = Path(__file__).resolve().parents[3] / 'bsp-base-external/board/ti/am625/rootfs-overlay'

class Criticality(unittest.TestCase):
    def test_auxiliary_failures_recorded_without_rejecting_core_boot(self):
        self.check(['S01logctld', 'S50dropbear', 'S50lighttpd', 'S50ptelemetryd', 'S98fwctld', 'S51psatellited', 'S09can', 'S10pdm', 'S11modules'], False)

    def test_core_and_unknown_failures_are_critical(self):
        for service in ['S49system_manager', 'S40network', 'S99unknown']:
            with self.subTest(service=service):
                self.check([service], True)

    def test_success_has_no_failure_marker(self):
        self.check([], False)

    def test_network_initializes_loopback_before_ethernet(self):
        with tempfile.TemporaryDirectory(prefix='network-init-') as directory:
            root = Path(directory); net = root/'net'; net.mkdir()
            for interface in ('lo', 'eth0'): (net/interface).mkdir()
            calls = root/'calls'; up = root/'ifup'
            up.write_text('#!/bin/sh\necho "$1" >> "$CALLS"\n[ "$1" != "$FAIL_IF" ]\n')
            up.chmod(0o755)
            script = root/'network'
            text = (BOARD/'etc/init.d/S40network').read_text().replace('/sys/class/net',str(net)).replace('/sbin/ifup',str(up))
            script.write_text(text)
            for failure in ('', 'lo'):
                calls.write_text('')
                result = subprocess.run([shutil.which('busybox'), 'ash', str(script), 'start'],
                    env=dict(os.environ, CALLS=str(calls), FAIL_IF=failure), capture_output=True, text=True)
                self.assertEqual(calls.read_text().splitlines(), ['lo','eth0'])
                self.assertEqual(result.returncode, 1 if failure else 0)

    def check(self, failed, critical):
        with tempfile.TemporaryDirectory(prefix='boot-criticality-') as directory:
            root = Path(directory)
            init = root/'init'; init.mkdir()
            run = root/'run'; run.mkdir()
            policy = root/'boot-health'; policy.write_text((BOARD/'etc/default/boot-health').read_text())
            for name in failed + ['S99success']:
                script = init/name
                script.write_text('#!/bin/sh\nexit ' + ('7' if name in failed else '0') + '\n')
                script.chmod(0o755)
            text = (BOARD/'etc/init.d/rcS').read_text()
            text = text.replace('/etc/init.d/', str(init)+'/').replace('/run', str(run))
            text = text.replace('/etc/default/boot-health', str(policy))
            script = root/'rcS'; script.write_text(text)
            result = subprocess.run([shutil.which('busybox'), 'ash', str(script)],
                                    env=dict(os.environ, LOGCTL_CAPTURED='rcS'), capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((run/'rcS.complete').exists())
            self.assertEqual((run/'rcS.failed').exists(), bool(failed))
            self.assertEqual((run/'rcS.critical').exists(), critical)
            for name in failed:
                self.assertIn(name+':7', (run/'rcS.failed').read_text())

if __name__ == '__main__':
    unittest.main()
