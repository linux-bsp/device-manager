#!/usr/bin/env python3
"""Exercise the real C cache collector against interrupted and foreign entries."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]
HWID = '1010001000110000'

class Cache(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='cache-test-build-')
        cls.binary = Path(cls.build.name)/'prune'
        main = cls.binary.with_suffix('.c')
        main.write_text('#include "fwctld-cache.h"\n#include <stdio.h>\n#include <stdlib.h>\n'
                        'int main(int n,char **v){unsigned count;unsigned long long bytes;'
                        'if(n!=4)return 2;int rc=fwctld_cache_prune(v[1],v[2],atoi(v[3]),&count,&bytes);'
                        'printf("%u %llu\\n",count,bytes);return rc?1:0;}\n')
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                        '-I'+str(SOURCE/'agent'),'-I'+str(SOURCE/'shared'),
                        '-I'+str(SOURCE.parent/'identity/include'),str(main),
                        str(SOURCE/'agent/fwctld-cache.c'),str(SOURCE/'shared/ota-common.c'),
                        '-o',str(cls.binary)],check=True)
    @classmethod
    def tearDownClass(cls): cls.build.cleanup()
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='cache-test-')
        self.root = Path(self.tmp.name)
    def tearDown(self): self.tmp.cleanup()
    def package(self, number, hwid=HWID):
        path = self.root/str(number); path.mkdir()
        manifest = f'BSPOTA2 {number} {hwid}\n'
        for name in ('release.itb','kernel.itb','rootfs.itb'):
            (path/name).write_bytes(b'data')
            manifest += f'{name} 4 '+ 'a'*64+'\n'
        (path/'manifest').write_text(manifest)
        return path
    def current(self, number): shutil.copyfile(self.root/str(number)/'manifest',self.root/'current')
    def prune(self, keep=2, ok=True):
        r = subprocess.run([str(self.binary),str(self.root),HWID,str(keep)],capture_output=True,text=True)
        self.assertEqual(r.returncode,0 if ok else 1,r.stderr)
        return int(r.stdout.split()[0])
    def test_keeps_current_and_newest(self):
        for i in range(1,7): self.package(i)
        self.current(1)
        self.assertEqual(self.prune(),3)
        self.assertEqual(sorted(p.name for p in self.root.iterdir()),['1','5','6','current'])
    def test_partials_and_unknown_entries_untouched(self):
        for i in range(1,5): self.package(i)
        self.current(4)
        partial=self.root/'.download-5';partial.mkdir();(partial/'kernel.itb').write_text('partial')
        (self.root/'1'/'operator-notes').write_text('retain')
        (self.root/'2'/'kernel.itb').unlink();(self.root/'2'/'kernel.itb').symlink_to(self.root/'3'/'kernel.itb')
        self.assertEqual(self.prune(1),1)
        self.assertTrue((self.root/'1'/'operator-notes').exists())
        self.assertTrue((self.root/'2'/'kernel.itb').is_symlink())
        self.assertEqual((partial/'kernel.itb').read_text(),'partial')
    def test_corrupt_or_foreign_current_blocks_deletion(self):
        for i in range(1,4): self.package(i)
        (self.root/'current').write_text('invalid\n')
        self.assertEqual(self.prune(ok=False),0)
        self.package(4,'1010001000210000');self.current(4)
        self.assertEqual(self.prune(ok=False),0)
        self.assertTrue((self.root/'1').is_dir())
    def test_foreign_manifest_and_symlink_directory_skipped(self):
        self.package(1,'1010001000210000')
        self.package(2);self.package(3);self.current(3)
        outside=self.root/'unmanaged';outside.mkdir();(outside/'precious').write_text('keep')
        (self.root/'4').symlink_to(outside,target_is_directory=True)
        self.assertEqual(self.prune(1),1)
        self.assertTrue((self.root/'1').exists())
        self.assertTrue((outside/'precious').exists())
    def test_recovers_partial_gc_without_recursing(self):
        self.package(1);self.package(2);self.current(2)
        (self.root/'1').rename(self.root/'.gc-1');(self.root/'.gc-1'/'kernel.itb').unlink()
        dirty=self.root/'.gc-3';dirty.mkdir();(dirty/'unmanaged').mkdir()
        self.assertEqual(self.prune(),1)
        self.assertFalse((self.root/'.gc-1').exists())
        self.assertTrue((dirty/'unmanaged').exists())
    def test_disabled_and_missing_current_preserve_all(self):
        for i in range(1,5):self.package(i)
        self.assertEqual(self.prune(),0)
        self.current(4)
        self.assertEqual(self.prune(0),0)
        self.assertTrue((self.root/'1').exists())
    def test_mismatched_number_size_hardlink_preserved(self):
        for i in range(1,5):self.package(i)
        self.current(4)
        m=self.root/'1'/'manifest';m.write_text(m.read_text().replace('BSPOTA2 1','BSPOTA2 9'))
        (self.root/'2'/'rootfs.itb').write_text('wrong size')
        os.link(self.root/'3'/'rootfs.itb',self.root/'alias')
        self.assertEqual(self.prune(1),0)
        self.assertTrue((self.root/'1').is_dir())

if __name__ == '__main__':unittest.main()
