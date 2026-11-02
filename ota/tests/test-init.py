#!/usr/bin/env python3
"""Run the actual init script under BusyBox ash with no tr in its PATH."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SOURCE=Path(__file__).resolve().parents[3]/'bsp-base-external/package/fwctld/S98fwctld'
class InitScript(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='ota-init-')
        self.root=Path(self.tmp.name)
        self.bin=self.root/'bin'; self.bin.mkdir()
        self.mac=self.root/'address'; self.mac.write_text('1c:63:49:22:c4:f7\n')
        self.config=self.root/'config'
        self.config.write_text(f'FWCTLD_ENABLED=1\nFWCTLD_AUTO_INSTALL=1\nFWCTLD_DEVICE_ID=""\nFWCTLD_SERVER=http://example.test:8090\nFWCTLD_PRODUCT=ti-am62x\nFWCTLD_LAYOUT=am62x-sd-ab-v1\nFWCTLD_CACHE={self.root}/cache\nFWCTLD_INTERVAL=15\n')
        self.calls=self.root/'calls'
        script=SOURCE.read_text().replace('/etc/default/fwctld',str(self.config)).replace('/data/fwctld.conf',str(self.root/'override')).replace('/sys/class/net/eth0/address',str(self.mac))
        self.script=self.root/'init'; self.script.write_text(script)
        for name in ('mkdir',): (self.bin/name).symlink_to(shutil.which(name))
        stub=self.bin/'start-stop-daemon'
        stub.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$TEST_CALLS"\n'); stub.chmod(0o755)
        self.env=dict(os.environ,PATH=str(self.bin),TEST_CALLS=str(self.calls))
    def tearDown(self):self.tmp.cleanup()
    def run_script(self):
        return subprocess.run([shutil.which('busybox'),'ash',str(self.script),'start'],env=self.env,capture_output=True,text=True)
    def override(self,text): (self.root/'override').write_text(text)
    def test_mac_without_tr(self):
        r=self.run_script(); self.assertEqual(r.returncode,0,r.stderr)
        self.assertIn('--device\n1c-63-49-22-c4-f7\n',self.calls.read_text())
        self.assertNotIn('not found',r.stderr)
    def test_console_default_and_disable(self):
        self.assertEqual(self.run_script().returncode,0)
        self.assertIn('--console\n/dev/console\n',self.calls.read_text())
        self.override('FWCTLD_CONSOLE=""\n')
        self.assertEqual(self.run_script().returncode,0)
        self.assertNotIn('--console',self.calls.read_text())
    def test_network_interface_default_override_disable(self):
        self.assertEqual(self.run_script().returncode,0)
        self.assertIn("--interface\neth0\n",self.calls.read_text())
        self.override("FWCTLD_INTERFACE=eth1\n")
        self.assertEqual(self.run_script().returncode,0)
        self.assertIn("--interface\neth1\n",self.calls.read_text())
        self.override('FWCTLD_INTERFACE=""\n')
        self.assertEqual(self.run_script().returncode,0)
        self.assertNotIn("--interface",self.calls.read_text())

    def test_new_service_paths(self):
        self.assertEqual(self.run_script().returncode,0)
        args=self.calls.read_text()
        for expected in ('/usr/sbin/fwctld','/var/run/fwctld.pid','/var/log/fwctld.log'):
            self.assertIn(expected,args)
    def test_persistent_configuration_overrides_defaults(self):
        self.override('FWCTLD_SERVER=http://new.test\nFWCTLD_DEVICE_ID=new-board\n')
        r=self.run_script();self.assertEqual(r.returncode,0,r.stderr)
        self.assertIn('--server\nhttp://new.test',self.calls.read_text())
        self.assertIn('--device\nnew-board',self.calls.read_text())
        self.calls.unlink();self.override('FWCTLD_ENABLED=0\n')
        self.assertEqual(self.run_script().returncode,0)
        self.assertFalse(self.calls.exists())
    def test_explicit_device_without_mac(self):
        self.mac.unlink(); self.override('FWCTLD_DEVICE_ID=custom-board\n')
        self.assertEqual(self.run_script().returncode,0)
        self.assertIn('--device\ncustom-board\n',self.calls.read_text())
    def test_disabled(self):
        self.override('FWCTLD_ENABLED=0\n'); self.assertEqual(self.run_script().returncode,0)
        self.assertFalse(self.calls.exists())
    def test_download_only(self):
        self.mac.unlink(); self.override('FWCTLD_AUTO_INSTALL=0\n')
        self.assertEqual(self.run_script().returncode,0)
        self.assertNotIn('--device',self.calls.read_text())
    def test_invalid_mac_stops(self):
        for value in ('', '1c:63:49', 'zz:63:49:22:c4:f7', '1c:63:49:22:c4:f7:aa'):
            with self.subTest(value=value):
                self.mac.write_text(value+'\n')
                self.assertNotEqual(self.run_script().returncode,0)
                self.assertFalse(self.calls.exists())
    def test_missing_mac_stops(self):
        self.mac.unlink();self.assertNotEqual(self.run_script().returncode,0)
        self.assertFalse(self.calls.exists())

class BoardInitScripts(unittest.TestCase):
    board=SOURCE.parents[2]/'board/ti/am625'

    def test_all_service_shell_syntax(self):
        files=list((self.board/'rootfs-overlay/etc/init.d').glob('*'))+[SOURCE]
        for path in files:
            if path.is_file() and path.read_bytes().startswith(b'#!/bin/sh'):
                with self.subTest(script=path.name):
                    r=subprocess.run([shutil.which('busybox'),'ash','-n',str(path)],capture_output=True,text=True)
                    self.assertEqual(r.returncode,0,r.stderr)

    def test_version_success_failure_and_stop(self):
        source=(self.board/'rootfs-overlay/etc/init.d/S99fw-version').read_text()
        with tempfile.TemporaryDirectory(prefix='version-init-') as tmp:
            root=Path(tmp);calls=root/'calls';stub=root/'fwctl';script=root/'version'
            stub.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$TEST_CALLS"\nexit "$TEST_EXIT"\n');stub.chmod(0o755)
            script.write_text(source.replace('/etc/default/fwctl',str(root/'no-config')).replace('/usr/sbin/fwctl',str(stub)))
            for action in ('start','restart','reload'):
                for code in ('0','1'):
                    env=dict(os.environ,TEST_CALLS=str(calls),TEST_EXIT=code,FWCTL_CONFIG='/test/firmware.conf')
                    r=subprocess.run([shutil.which('busybox'),'ash',str(script),action],capture_output=True,text=True,env=env)
                    self.assertEqual(r.returncode,0,r.stderr)
                    self.assertEqual('[fwctl] [ERROR]' in r.stderr,code=='1')
            self.assertEqual(calls.read_text().splitlines(),['--config /test/firmware.conf version']*6)
            self.assertEqual(subprocess.run([shutil.which('busybox'),'ash',str(script),'stop']).returncode,0)
            self.assertEqual(len(calls.read_text().splitlines()),6)

if __name__=='__main__':unittest.main()
