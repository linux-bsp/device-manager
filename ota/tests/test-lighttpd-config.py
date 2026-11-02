#!/usr/bin/env python3
"""Validate board web configuration with native lighttpd 1.4.82.
Usage: python3 tests/test-lighttpd-config.py /path/to/lighttpd /path/to/modules
"""
import http.client
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

binary=Path(sys.argv[1]).resolve(); modules=Path(sys.argv[2]).resolve()
source=Path(__file__).resolve().parents[3]/'board/ti/am625/rootfs-overlay/etc/lighttpd/lighttpd.conf'
with tempfile.TemporaryDirectory(prefix='lighttpd-config-') as temp:
    root=Path(temp); docs=root/'www'; docs.mkdir(); (docs/'folder').mkdir()
    (docs/'index.html').write_text('pdebug configuration test')
    (docs/'folder/hidden-name.txt').write_text('directory listing must stay disabled')
    (root/'cgi').mkdir()
    with socket.socket() as sock:
        sock.bind(('127.0.0.1',0)); port=sock.getsockname()[1]
    config=source.read_text().replace('/usr/share/pdebug-web',str(docs)).replace('/usr/libexec/pdebug-web/',str(root/'cgi')+'/').replace('/run/pdebug-web',str(root)).replace('/var/log/pdebug-web.log',str(root/'server.log')).replace('server.port = 8080',f'server.port = {port}').replace('server.bind = "0.0.0.0"','server.bind = "127.0.0.1"')
    new=root/'new.conf'; new.write_text(config)
    old=root/'old.conf'; old.write_text(config.replace('"mod_setenv")','"mod_setenv", "mod_dirlisting")')+'\ndir-listing.activate = "disable"\n')
    for path, expect_warning in ((old,True),(new,False)):
        result=subprocess.run([str(binary),'-tt','-m',str(modules),'-f',str(path)],capture_output=True,text=True)
        assert result.returncode==0,result.stdout+result.stderr
        assert ('unknown config-key: dir-listing.activate' in result.stderr)==expect_warning,result.stderr
    with (root/'process.log').open('w') as log:
        server=subprocess.Popen([str(binary),'-D','-m',str(modules),'-f',str(new)],stdout=log,stderr=log)
        try:
            for _ in range(100):
                try:
                    c=http.client.HTTPConnection('127.0.0.1',port,timeout=2)
                    c.request('GET','/'); response=c.getresponse();body=response.read();c.close()
                    assert response.status==200 and body==b'pdebug configuration test';break
                except OSError:
                    assert server.poll() is None,(root/'process.log').read_text()
                    time.sleep(.02)
            else:raise AssertionError('server failed to listen')
            c=http.client.HTTPConnection('127.0.0.1',port,timeout=2)
            c.request('GET','/folder/');response=c.getresponse();body=response.read();c.close()
            assert response.status==403,(response.status,body)
            assert b'hidden-name.txt' not in body
        finally:
            server.terminate();server.wait(timeout=5)
    assert 'unknown config-key' not in (root/'server.log').read_text()
print('PASS: previous warning reproduced; fixed config has no warning; index served; directory listing denied')
