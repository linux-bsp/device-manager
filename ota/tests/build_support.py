"""Flags for native C tests linking the same transfer archives as the tools."""
import os
from pathlib import Path
import shlex
ROOT=Path(__file__).resolve().parents[2]
def transfer_flags(client=False):
    return ['-I'+str(ROOT/'transfer/include')]+shlex.split(os.environ.get('CPPFLAGS',''))+[
        str(ROOT/('transfer/build/libdm-transfer.a' if client else 'transfer/build/libdm-transfer-server.a'))]+shlex.split(os.environ.get('LDFLAGS',''))+[
        *(['-lcurl'] if client else []),'-lcrypto','-pthread']
