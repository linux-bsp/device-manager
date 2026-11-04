#!/usr/bin/env python3
"""Classify collected observations without touching host storage configuration."""
import ctypes
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import unittest

SOURCE=Path(__file__).resolve().parents[1]


class Diagnostics(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build=tempfile.TemporaryDirectory(prefix='fw-diagnostics-build-')
        library=Path(cls.build.name)/'diagnostics.so'
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC',
                        str(SOURCE/'agent/fwctld-diagnostics.c'),'-o',str(library)],check=True)
        cls.lib=ctypes.CDLL(str(library))
        cls.lib.fwctld_diagnostic_reason.argtypes=[ctypes.c_char_p,ctypes.c_char_p,ctypes.c_char_p]

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='fw-diagnostics-')
        self.root=Path(self.tmp.name);self.state=self.root/'state'
        self.state.write_text('persistent\n')

    def tearDown(self):
        self.tmp.cleanup()

    def reason(self):
        output=ctypes.create_string_buffer(b'original-cause',64)
        found=self.lib.fwctld_diagnostic_reason(str(self.state).encode(),str(self.root).encode(),output)
        return found,output.value.decode()

    def test_data_observations_are_specific_and_take_precedence(self):
        (self.root/'crash-dmesg-ramoops-0').write_text('Kernel panic - not syncing: fixture')
        for state,reason in [('unavailable','data-unavailable-observed'),('read-only','data-read-only-observed'),('low-space','data-low-space-observed')]:
            self.state.write_text(state+'\n')
            self.assertEqual(self.reason(),(1,reason))

    def test_linux_panic_and_oops_include_cross_buffer_boundary(self):
        crash=self.root/'crash-dmesg-ramoops-0'
        crash.write_bytes(b'x'*4090+b'Kernel panic - not syncing: fixture')
        self.assertEqual(self.reason(),(1,'linux-panic-observed'))
        crash.write_bytes(b'Oops: fixture')
        self.assertEqual(self.reason(),(1,'linux-oops-observed'))

    def test_missing_state_and_logs_do_not_invent_a_cause(self):
        self.state.unlink();self.assertEqual(self.reason(),(0,'original-cause'))
        self.state.write_text('unknown\n');self.assertEqual(self.reason(),(0,'original-cause'))

    def test_regular_service_text_and_symlinks_are_not_crash_evidence(self):
        outside=self.root/'services.log';outside.write_text('Kernel panic - not syncing: quoted text')
        (self.root/'crash-dmesg-ramoops-0').symlink_to(outside)
        self.assertEqual(self.reason(),(0,'original-cause'))

    def test_raw_pstore_names_are_supported(self):
        (self.root/'console-ramoops-0').write_text('Kernel panic - not syncing: fixture')
        self.assertEqual(self.reason(),(1,'linux-panic-observed'))


class ReportTransitions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build=tempfile.TemporaryDirectory(prefix='fw-report-build-')
        root=Path(cls.build.name);source=root/'report.c';cls.program=root/'report'
        source.write_text('#define fwctld_observe_failure test_observe\n'
            '#include "'+str(SOURCE/'agent/fwctld-auto.c')+'"\n'+r'''
void test_observe(char reason[64],int after_reboot){(void)reason;(void)after_reboot;}
int main(int argc,char **argv){
    if(argc!=3)return 2;
    struct ota_report r={.publication=1,.version=1,.target=1,.running=1};
    strcpy(r.device,"test-device");strcpy(r.hwid,"1010001000110000");
    strcpy(r.boot,"source-a");strcpy(r.phase,"boot-testing");
    snprintf(r.reason,sizeof(r.reason),"%s",argv[2]);
    return report(argv[1],&r,"rolled-back",1)?1:0;
}
''')
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(SOURCE/'shared'),'-I'+str(SOURCE.parent/'identity/include'),
                        '-I'+str(SOURCE/'agent'),str(source),str(SOURCE/'shared/ota-common.c'),
                        str(SOURCE/'shared/ota-report.c'),str(SOURCE/'agent/fwctld-network.c'),
                        str(SOURCE/'agent/fwctld-log.c'),'-o',str(cls.program)],check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def test_recovery_report_preserves_observed_failure_reason(self):
        reports=[]
        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                reports.append(self.rfile.read(int(self.headers['Content-Length'])).decode())
                self.send_response(200);self.end_headers();self.wfile.write(b'ok\n')
            def do_GET(self):
                self.send_response(200);self.end_headers()
                self.wfile.write(b'BSPOTAVERSION1 1 v0.0.1 '+b'a'*64+b'\n')
            def log_message(self,*args):pass
        server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
        worker=threading.Thread(target=server.serve_forever,daemon=True);worker.start()
        try:
            for previous,expected in [('data-unavailable-observed','data-unavailable-observed'),
                                      ('linux-panic-observed','linux-panic-observed'),
                                      ('none','boot-failed-rolled-back')]:
                with self.subTest(reason=previous):
                    result=subprocess.run([str(self.program),f'http://127.0.0.1:{server.server_port}',previous],
                                          env=os.environ,capture_output=True,text=True,timeout=5)
                    self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                    self.assertTrue(reports[-1].endswith(expected+'\n'),reports[-1])
        finally:server.shutdown();worker.join();server.server_close()


if __name__=='__main__':unittest.main()
