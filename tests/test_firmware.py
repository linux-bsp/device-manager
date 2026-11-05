#!/usr/bin/env python3
"""Compile and run engine transaction tests without EDK or a device."""
import os, shlex, subprocess, tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
src=root/'firmware/src'
common=['dm_fw_layout.c','dm_fw_metadata.c','dm_fw_storage.c','dm_fw_storage_block.c','dm_fw_storage_mtd.c']
tests={'handle_test':[],'firmware_test':['dm_fw.c']+common,'firmware_transaction_test':['dm_fw_update.c']+common,'firmware_recovery_test':['dm_fw_recovery.c']+common,'fwctl_version_test':[],'fwctl_main_test':[],'fwctl_health_test':['../../fwctl/src/fwctl_systemd.c'],'firmware_direct_read_test':[f for f in common if f != 'dm_fw_storage_block.c']}
with tempfile.TemporaryDirectory(prefix='dm-fw-test-') as tmp:
    for name,files in tests.items():
        binary=Path(tmp)/name
        args=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(src),'-I'+str(root/'firmware/include'),'-I'+str(root/'fwctl/include')]+shlex.split(os.environ.get('CPPFLAGS',''))+[str(root/'tests'/f'{name}.c')]+[str(src/f) for f in files]
        if name=='fwctl_version_test':args+=[str(root/'fwctl/src/fwctl_firmware.c'),'-Wl,--wrap=fopen']
        if name=='fwctl_health_test':args+=['-I'+str(root/'fwctl/adapter'),'-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Wl,--wrap=ioctl']
        args+=shlex.split(os.environ.get('LDFLAGS',''))+['-lcrypto','-pthread','-o',str(binary)]
        subprocess.run(args,check=True);subprocess.run([str(binary)],check=True)
        print('PASS',name)
