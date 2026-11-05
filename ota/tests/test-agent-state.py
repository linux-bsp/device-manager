#!/usr/bin/env python3
"""Persistent OTA journal: reopen, write-ahead recovery, ordering and sync errors."""
from pathlib import Path
from build_support import transfer_flags
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='fwctld-state-test-') as name:
    directory = Path(name)
    binary = directory / 'test'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I'+str(root/'agent'), '-I'+str(root/'shared'), '-I'+str(root.parent/'identity/include'),
                    str(root/'tests/agent-state-test.c'), str(root/'agent/fwctld-state.c'),
                    str(root/'shared/ota-common.c'), str(root/'shared/ota-report.c'),
                    '-Wl,--wrap=fsync']+transfer_flags()+['-o', str(binary)], check=True)
    subprocess.run([str(binary), str(directory/'state')], check=True)
print('PASS: durable OTA journal, ordered outbox, damaged-state refusal and injected fsync failures')
