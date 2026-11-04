#!/usr/bin/env python3
"""HWID in real libubootenv: MMC files, eMMC areas and a NOR syscall simulator."""
import ctypes
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
HWID = '1010001000110000'
OTHER = '1010002000110000'
SIZE = 65536

class Status(ctypes.Structure):
    _fields_ = [('text',ctypes.c_char * 17)]

def environment(value=HWID, flag=1):
    values = {'bootcmd':'run fw_boot','ipaddr':'192.168.10.211','keep_me':'preserved'}
    if value is not None: values['hwid']=value
    data=b'\0'.join((k+'='+v).encode() for k,v in values.items())+b'\0\0'
    data=data.ljust(SIZE-5,b'\0')
    return struct.pack('<I',zlib.crc32(data))+bytes([flag])+data

class HwidTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory(prefix='hwid-build-')
        cls.addClassCleanup(cls.tmp.cleanup)
        # LIBUBOOTENV_SOURCE may point at an independently checked out v0.3.7.
        source=Path(os.environ.get('LIBUBOOTENV_SOURCE',ROOT.parent/'output/am625-time-ui/build/libubootenv-0.3.7'))/'src'
        if not source.is_dir(): raise RuntimeError('Set LIBUBOOTENV_SOURCE to the libubootenv source directory')
        lib=Path(cls.tmp.name)/'hwid.so'
        subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-shared','-fPIC','-std=gnu11','-Wall','-Wextra',
            '-DNO_YAML_SUPPORT','-DVERSION="0.3.7"','-I'+str(ROOT/'identity/include'),'-I'+str(source),
            str(ROOT/'identity/hwid.c'),str(ROOT/'tests/hwid_mtd_mock.c')]+
            [str(source/p) for p in ('uboot_env.c','uboot_mtd.c','common.c','extended_config.c')]+
            ['-Wl,--wrap=stat,--wrap=open,--wrap=realpath,--wrap=ioctl,--wrap=write','-lz','-o',str(lib)],check=True)
        cls.lib=ctypes.CDLL(str(lib))
        cls.lib.dm_hwid_read.argtypes=[ctypes.c_char_p,ctypes.POINTER(Status)]
        cls.lib.dm_hwid_set.argtypes=[ctypes.c_char_p,ctypes.c_char_p,ctypes.POINTER(Status)]

    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='hwid-env-');self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.media=self.root/'user-area';self.config=self.root/'fw_env.config'
        self.media.write_bytes(b'\xa5'*SIZE+environment(flag=8)+environment(flag=7)+b'\xa5'*SIZE)
        self.configure()
        os.environ['HWID_TEST_NOR']=str(self.media)
        for key,value in [('test_mtd_type',3),('test_fail_write',0),('test_erases',0),('test_nor_enabled',0)]: ctypes.c_int.in_dll(self.lib,key).value=value

    def configure(self,device=None):
        ctypes.c_int.in_dll(self.lib,'test_nor_enabled').value=int(device=='/dev/mtd-hwid-test')
        self.config.write_text(f'{device or self.media} 0x10000 0x10000 0x10000\n{device or self.media} 0x20000 0x10000 0x10000\n')

    def read(self):
        result=Status();rc=self.lib.dm_hwid_read(os.fsencode(self.config),ctypes.byref(result));return rc,result.text.decode()
    def change(self,value):
        result=Status();rc=self.lib.dm_hwid_set(os.fsencode(self.config),value.encode(),ctypes.byref(result));return rc,result.text.decode()

    def test_sd_read_change_and_preserve_environment(self):
        self.assertEqual(self.read(),(0,HWID));self.assertEqual(self.change(OTHER),(0,OTHER))
        raw=self.media.read_bytes();self.assertEqual(raw[:SIZE],b'\xa5'*SIZE);self.assertEqual(raw[3*SIZE:],b'\xa5'*SIZE)
        for i in (1,2):
            record=raw[i*SIZE:(i+1)*SIZE]
            self.assertEqual(struct.unpack('<I',record[:4])[0],zlib.crc32(record[5:]))
            values=dict(x.split(b'=',1) for x in record[5:].split(b'\0') if b'=' in x)
            self.assertEqual(values[b'bootcmd'],b'run fw_boot');self.assertEqual(values[b'keep_me'],b'preserved')
        self.assertEqual(self.change(HWID),(0,HWID))

    def test_same_value_is_idempotent(self):
        before=self.media.read_bytes();self.assertEqual(self.change(HWID),(0,HWID));self.assertEqual(before,self.media.read_bytes())

    def test_latest_copy_selects_changed_identity(self):
        with self.media.open('r+b') as f: f.seek(2*SIZE);f.write(environment(OTHER,9))
        self.assertEqual(self.read(),(0,OTHER))

    def test_counter_wrap_matches_uboot(self):
        with self.media.open('r+b') as f:f.seek(SIZE);f.write(environment(HWID,255));f.write(environment(OTHER,0))
        self.assertEqual(self.read(),(0,OTHER))

    def test_corrupt_copy_falls_back(self):
        with self.media.open('r+b') as f:f.seek(SIZE+32);f.write(b'broken')
        self.assertEqual(self.read(),(0,HWID));self.assertEqual(self.change(OTHER),(0,OTHER))

    def test_both_bad_crc_do_not_reset_boot_environment(self):
        with self.media.open('r+b') as f:
            for i in (1,2):f.seek(i*SIZE);f.write(b'xxxx')
        before=self.media.read_bytes();self.assertNotEqual(self.read()[0],0);self.assertNotEqual(self.change(OTHER)[0],0)
        self.assertEqual(before,self.media.read_bytes())

    def test_invalid_and_missing_identity(self):
        before=self.media.read_bytes()
        for text in ('1','2010001000110000','1010001000110001','0000000000000000','10100010001x0000'):
            self.assertEqual(self.change(text)[0],-22)
        self.assertEqual(before,self.media.read_bytes())
        with self.media.open('r+b') as f:f.seek(SIZE);f.write(environment(None,10))
        self.assertEqual(self.read()[0],-61)

    def test_emmc_user_boot0_boot1_are_explicit(self):
        for area in ('boot0','boot1'):
            file=self.root/area;file.write_bytes(self.media.read_bytes());self.configure(file)
            self.assertEqual(self.change(OTHER),(0,OTHER))
        self.configure();self.assertEqual(self.read(),(0,HWID))

    def test_nor_real_library_erase_and_active_flags(self):
        with self.media.open('r+b') as f:f.seek(SIZE);f.write(environment(HWID,1));f.write(environment(HWID,0))
        self.configure('/dev/mtd-hwid-test')
        self.assertEqual(self.read(),(0,HWID));self.assertEqual(self.change(OTHER),(0,OTHER))
        self.assertEqual(ctypes.c_int.in_dll(self.lib,'test_erases').value,1)
        self.assertEqual(self.media.read_bytes()[:SIZE],b'\xa5'*SIZE)
        self.assertEqual(self.media.read_bytes()[3*SIZE:],b'\xa5'*SIZE)

    def test_nor_interrupted_write_keeps_old_copy(self):
        with self.media.open('r+b') as f:f.seek(SIZE);f.write(environment(HWID,1));f.write(environment(HWID,0))
        self.configure('/dev/mtd-hwid-test');ctypes.c_int.in_dll(self.lib,'test_fail_write').value=1
        self.assertNotEqual(self.change(OTHER)[0],0)
        ctypes.c_int.in_dll(self.lib,'test_fail_write').value=0
        self.assertEqual(self.read(),(0,HWID))

    def test_nand_is_rejected(self):
        self.configure('/dev/mtd-hwid-test');ctypes.c_int.in_dll(self.lib,'test_mtd_type').value=4
        self.assertEqual(self.change(OTHER)[0],-95)

    def test_uboot_value_codec_identical(self):
        self.assertEqual((ROOT/'identity/include/dm/hwid-value.h').read_bytes(),
            (ROOT.parent/'uboot/u-boot-2026.04/include/dm-hwid-value.h').read_bytes())

if __name__=='__main__':unittest.main()
