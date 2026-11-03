#!/usr/bin/env python3
"""Run the actual U-Boot producer through warm resets against the Linux ABI."""
from pathlib import Path
import subprocess
import tempfile
import unittest

WORKSPACE=Path(__file__).resolve().parents[2]
UBOOT=WORKSPACE/'uboot/u-boot-2026.04'
LINUX=WORKSPACE/'linux/linux-7.0'
class BootlogABI(unittest.TestCase):
    def test_layout_is_identical_in_both_firmware_stages(self):
        self.assertEqual((UBOOT/'include/bsp_bootlog_layout.h').read_bytes(),(LINUX/'include/linux/bsp_bootlog_layout.h').read_bytes())
    def run_c(self,defines,body):
        with tempfile.TemporaryDirectory(prefix='bootlog-abi-') as folder:
            root=Path(folder)
            files={
                'linux/types.h':'#include <stdint.h>\ntypedef uint32_t u32;typedef uint64_t u64;typedef uint8_t u8;typedef unsigned long ulong;\n',
                'bsp_bootlog.h':'#include <stdbool.h>\n#include <stddef.h>\n#include <linux/types.h>\n#define __aligned(n) __attribute__((aligned(n)))\n#define __section(s)\n',
                'linux/string.h':'#include <string.h>\n',
                'linux/kernel.h':'#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))\n',
                'linux/kconfig.h':'#define IS_ENABLED(x) (x)\n',
                'asm/cache.h':'static inline void flush_dcache_range(unsigned long a,unsigned long b){(void)a;(void)b;}\n',
                'cpu_func.h':'',
            }
            for name,text in files.items():
                p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
            (root/'bsp_bootlog_layout.h').write_bytes((UBOOT/'include/bsp_bootlog_layout.h').read_bytes())
            source=root/'test.c'
            source.write_text('#define _GNU_SOURCE\n#include <assert.h>\n#include <sys/mman.h>\n'+defines+
                f'\n#include "{UBOOT}/common/bsp_bootlog.c"\n'+'''
static void reset(void){active=NULL;capacity=0;initialized=false;in_ddr=false;}
static void create(int slot,unsigned long long gen){struct bl_bank *b=bank(slot);memset(b,0,BL_BANK_SIZE);b->magic=BL_MAGIC;b->version=2;b->generation=gen;b->crc=bl_crc(0,b,32);}
int main(void){void *m=mmap((void *)BL_BASE,BL_TOTAL,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);assert(m==(void *)BL_BASE);
'''+body+'\nreturn 0;}\n')
            result=subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-I'+str(root),str(source),'-o',str(root/'test')],capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr.decode())
            subprocess.run([str(root/'test')],check=True,capture_output=True)
    def test_r5_rotation_corruption_overflow_and_restart_first_after_ack(self):
        self.run_c('#define CONFIG_CPU_V7R 1\n#define CONFIG_XPL_BUILD 1',r'''
create(1,10);bank(1)->crc^=1; /* Reject a torn header, start in the first bank. */
bsp_bootlog_write("SPL first\n",10);bsp_bootlog_ddr_ready();assert(bl_valid(bank(0)));assert(bank(0)->generation==1);
assert(bl_record_valid(active,0));assert(active->length==10);
bank(0)->id[0]=42;bank(0)->crc=bl_crc(0,bank(0),32);
reset();bsp_bootlog_write("second",6);bsp_bootlog_ddr_ready();assert(active==(void *)bank(1)+bl_offset[0]);assert(bank(1)->generation==2);assert(bank(0)->id[0]==42);
assert(!memcmp((void *)bank(0)+bl_offset[0]+sizeof(*active),"SPL first\n",10));
char full[4096];memset(full,'x',sizeof(full));for(int i=0;i<10;i++)bsp_bootlog_write(full,sizeof(full));assert(active->dropped>0);assert(bl_record_valid(active,0));
active->crc^=1;assert(!bl_record_valid(active,0));assert(bl_prefix_valid(active,0));active->crc^=1;
active->data[1]^=1;assert(!bl_record_valid(active,0));assert(!bl_prefix_valid(active,0));assert(bl_valid(bank(0)));
reset();bsp_bootlog_write("third",5);bsp_bootlog_ddr_ready();assert(active==(void *)bank(0)+bl_offset[0]);assert(bank(0)->generation==3);assert(bank(1)->generation==2);
/* The Linux ACK clears both banks after server commit. */
memset((void *)BL_BASE,0,BL_TOTAL);reset();bsp_bootlog_write("after ack",9);bsp_bootlog_ddr_ready();assert(active==(void *)bank(0)+bl_offset[0]);assert(bank(0)->generation==1);assert(!bl_valid(bank(1)));
''')
    def test_a53_preserves_previous_bank_and_current_spl(self):
        self.run_c('#define CONFIG_XPL_BUILD 1',r'''
create(0,4);create(1,5);bank(0)->id[0]=7;bank(0)->crc=bl_crc(0,bank(0),32);
memset((void *)bank(1)+bl_offset[0],0x5a,32);
bsp_bootlog_write("A53\n",4);assert(active==(void *)bank(1)+bl_offset[1]);assert(bl_record_valid(active,1));assert(bank(0)->id[0]==7);assert(*(u8 *)((void *)bank(1)+bl_offset[0])==0x5a);
''')
    def test_uboot_preserves_sibling_and_rejects_missing_handoff(self):
        self.run_c('#define CONFIG_XPL_BUILD 0',r'''
reset();bsp_bootlog_write("no handoff",10);assert(active==NULL);
create(0,8);create(1,7);reset();bsp_bootlog_write("U-Boot\n",7);assert(active==(void *)bank(0)+bl_offset[2]);assert(bl_record_valid(active,2));assert(bank(1)->generation==7);
''')
if __name__=='__main__':unittest.main()
