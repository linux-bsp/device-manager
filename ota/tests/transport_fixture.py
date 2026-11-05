"""Test-only HTTP fault proxy; faults exercise the real in-process transport."""
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import socket
import threading
import time

class FaultProxy:
    def __init__(self, upstream):
        self.upstream=upstream;self.rules=[];outer=self
        class Handler(BaseHTTPRequestHandler):
            def handle_request(self):
                try:outer.handle(self)
                except (BrokenPipeError,ConnectionResetError,OSError):pass
            do_GET=do_POST=handle_request
            def log_message(self,*args):pass
        self.server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
        self.worker=threading.Thread(target=self.server.serve_forever,daemon=True);self.worker.start()
        self.port=self.server.server_port
    def close(self):self.server.shutdown();self.worker.join();self.server.server_close()
    def add(self,marker,**rule):
        marker=Path(marker);marker.touch();self.rules.append(dict(marker=marker,**rule));return marker
    def handle(self,h):
        rule=next((r for r in self.rules if r['marker'].exists() and
            h.path.endswith(r.get('suffix','')) and h.command==r.get('method',h.command)),{})
        if rule.get('seen'):Path(rule['seen']).write_text('requested')
        if rule.get('wait'):
            while not Path(rule['wait']).exists():time.sleep(.02)
        if rule.get('withdraw'):Path(rule['withdraw']).unlink(missing_ok=True)
        if rule.get('drop'):
            h.close_connection=True;return
        body=h.rfile.read(int(h.headers.get('Content-Length',0)))
        conn=http.client.HTTPConnection('127.0.0.1',self.upstream,timeout=10)
        try:
            conn.request(h.command,h.path,body,{k:v for k,v in h.headers.items() if k.lower() not in ('host','connection')})
            r=conn.getresponse();data=r.read();status=r.status
            if rule.get('response') is not None:data=rule['response']
            if rule.get('corrupt'):data=bytes([data[0]^1])+data[1:]
            h.send_response(status)
            for key,value in r.getheaders():
                if key.lower() not in ('content-length','connection','transfer-encoding'):h.send_header(key,value)
            h.send_header('Content-Length',str(len(data)));h.end_headers()
            if rule.get('partial'):data=data[:len(data)//2]
            h.wfile.write(data);h.wfile.flush();h.close_connection=True
        finally:conn.close()
