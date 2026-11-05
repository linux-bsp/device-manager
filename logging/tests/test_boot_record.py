#!/usr/bin/env python3
"""Boot evidence tests use disposable roots, never the host's shutdown path."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import unittest

spec = importlib.util.spec_from_file_location('logging_tests', Path(__file__).with_name('test_logging.py'))
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)


class BootRecord(unittest.TestCase):
    setUp = base.Logging.setUp
    tearDown = base.Logging.tearDown
    cli = base.Logging.cli
    start = base.Logging.start
    stop = base.Logging.stop
    wait = base.Logging.wait
    text = base.Logging.text

    def ready(self):
        self.start()
        self.wait(lambda: self.cli('boot-state').returncode == 0)

    def state(self):
        return json.loads(self.cli('boot-state', check=True).stdout)

    def test_intent_health_shutdown_and_daemon_restart(self):
        boot = self.store / self.env['LOGCTL_BOOT_ID']
        boot.mkdir()
        (boot/'uboot.log').write_text('U-Boot\nReset reason: RST\n')
        self.ready()
        self.assertEqual(self.state()['uboot_reset_label'], 'RST')
        self.assertEqual(self.state()['phase'], 'booting')
        self.cli('boot-event', 'health-confirmed', check=True)
        before = self.state()
        self.assertTrue(before['health_confirmed'])
        self.cli('boot-event', 'health-confirmed', check=True)
        self.assertEqual(self.state(), before)
        self.cli('boot-event', 'ota-update', check=True)
        self.cli('boot-event', 'shutdown', check=True)
        state = self.state()
        self.assertEqual(state['intent'], 'ota-update')
        self.assertEqual(state['phase'], 'shutdown-started')
        self.assertEqual(state['reset_cause'], 'unknown')
        self.assertNotEqual(self.cli('boot-event', 'operator').returncode, 0)
        self.stop(); self.ready()
        self.assertEqual(self.state(), state)
        events=[json.loads(line) for line in self.cli('boot-events',check=True).stdout.splitlines()]
        self.assertEqual(events[-1],state)
        self.assertEqual(len(events),4)
        self.assertTrue(state['updated_local'].endswith('+08:00'))

    def test_previous_recorded_boot_is_not_inferred_as_hardware_cause(self):
        self.ready()
        self.cli('boot-event', 'health-confirmed', check=True)
        self.cli('boot-event', 'ota-update', check=True)
        self.cli('boot-event', 'shutdown', check=True)
        self.stop()
        self.env['LOGCTL_BOOT_ID'] = 'next-boot'
        boot = self.store/'next-boot'; boot.mkdir()
        (boot/'uboot.log').write_text('Reset reason: WDOG\n')
        self.ready()
        state = self.state()
        self.assertEqual(state['previous_recorded_linux_boot'], 'test-boot-1')
        self.assertEqual(state['previous_intent'], 'ota-update')
        self.assertEqual(state['previous_phase'], 'shutdown-started')
        self.assertEqual(state['previous_health'], 'confirmed')
        self.assertEqual(state['previous_record_integrity'], 'valid')
        self.assertEqual(state['uboot_reset_label'], 'WDOG')
        self.assertEqual(state['reset_cause'], 'unknown')

    def test_corrupt_previous_record_does_not_block_new_boot(self):
        self.ready(); self.stop()
        state = self.store/'test-boot-1/boot-state'
        state.write_bytes(state.read_bytes().replace(b'booting', b'healthy'))
        self.env['LOGCTL_BOOT_ID'] = 'after-corruption'
        self.ready()
        self.assertEqual(self.state()['previous_record_integrity'], 'invalid')
        self.assertEqual(self.state()['previous_intent'], 'none')
        self.assertEqual(self.state()['reset_cause'], 'unknown')
        self.cli('write', 'services', 'collection-still-works', check=True)
        self.wait(lambda: 'collection-still-works' in self.text())

    def test_corrupt_current_record_is_preserved(self):
        self.ready()
        state = self.store/'test-boot-1/boot-state'
        state.write_bytes(b'corrupt persisted evidence')
        self.assertNotEqual(self.cli('boot-state').returncode, 0)
        self.assertNotEqual(self.cli('boot-event', 'health-confirmed').returncode, 0)
        self.stop(); self.start()
        self.cli('write', 'services', 'still-running', check=True)
        self.wait(lambda: 'still-running' in self.text())
        self.assertEqual(state.read_bytes(), b'corrupt persisted evidence')

    def test_partial_journal_write_and_bounded_rotation(self):
        self.ready()
        journal = self.store/'test-boot-1/boot-events'
        with journal.open('ab') as out:
            out.write(b'interrupted-event')
        self.cli('boot-event', 'operator', check=True)
        self.assertIn(b'interrupted-event\n', journal.read_bytes())
        self.cli('boot-event', 'cancel-intent', check=True)
        self.assertEqual(self.state()['intent'], 'none')
        history=self.cli('boot-events');self.assertNotEqual(history.returncode,0)
        self.assertEqual(len(history.stdout.splitlines()),3)
        self.assertIn('damaged records',history.stderr)
        for i in range(700):
            self.cli('boot-event', 'operator' if i % 2 == 0 else 'cancel-intent', check=True)
        self.assertLessEqual(journal.stat().st_size, 65536)
        self.assertLessEqual(journal.with_name('boot-events.1').stat().st_size, 65536)
        self.assertGreater(self.state()['sequence'], 700)

    def test_temp_symlink_cannot_modify_outside_file(self):
        boot = self.store/'test-boot-1'; boot.mkdir()
        victim = self.base/'outside'; victim.write_text('protected')
        (boot/'boot-state.tmp').symlink_to(victim)
        self.start()
        self.cli('write', 'services', 'collection-active', check=True)
        self.wait(lambda: 'collection-active' in self.text())
        self.assertEqual(victim.read_text(), 'protected')
        self.assertNotEqual(self.cli('boot-state').returncode, 0)

    def test_failed_state_sync_never_replaces_committed_state(self):
        self.ready()
        previous = self.state()
        source = self.base/'fail-sync.c'
        source.write_text('#include <errno.h>\nint fsync(int fd){(void)fd;errno=EIO;return -1;}\n')
        library = self.base/'fail-sync.so'
        subprocess.run([os.environ.get('CC', 'cc'), '-shared', '-fPIC', str(source), '-o', str(library)], check=True)
        self.env['LD_PRELOAD'] = str(library)
        self.assertNotEqual(self.cli('boot-event', 'ota-update').returncode, 0)
        del self.env['LD_PRELOAD']
        self.assertEqual(self.state(), previous)


if __name__ == '__main__':
    unittest.main()
