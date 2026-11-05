#!/usr/bin/env python3
"""Compile and run firmware transaction tests without EDK or a device."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
COMMON = [
    'core/dm_fw_layout.c', 'core/dm_fw_metadata.c',
    'storage/dm_fw_storage.c', 'storage/dm_fw_storage_block.c',
    'storage/dm_fw_storage_mtd.c',
]
TESTS = {
    'handle_test': [],
    'firmware_test': ['core/dm_fw.c', *COMMON],
    'firmware_transaction_test': ['core/dm_fw_update.c', *COMMON],
    'firmware_recovery_test': ['core/dm_fw_recovery.c', *COMMON],
    'fwctl_version_test': ['cli/fwctl_firmware.c'],
    'fwctl_main_test': [],
    'fwctl_health_test': ['health/fwctl_systemd.c'],
    'firmware_direct_read_test': [
        name for name in COMMON if name != 'storage/dm_fw_storage_block.c'
    ],
}

with tempfile.TemporaryDirectory(prefix='dm-fw-test-') as temporary:
    for name, sources in TESTS.items():
        binary = Path(temporary) / name
        command = shlex.split(os.environ.get('CC', 'cc'))
        command += ['-std=gnu11', '-Wall', '-Wextra', '-Werror']
        command += ['-I' + str(ROOT / path) for path in ('core', 'storage', 'include', 'health')]
        command += shlex.split(os.environ.get('CPPFLAGS', ''))
        command += [str(ROOT / 'tests' / (name + '.c'))]
        command += [str(ROOT / path) for path in sources]
        if name == 'fwctl_version_test':
            command += ['-Wl,--wrap=fopen']
        if name == 'fwctl_health_test':
            command += ['-ffunction-sections', '-fdata-sections',
                        '-Wl,--gc-sections', '-Wl,--wrap=ioctl']
        command += shlex.split(os.environ.get('LDFLAGS', ''))
        command += ['-lcrypto', '-pthread', '-o', str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)
        print('PASS', name)
