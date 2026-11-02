#!/usr/bin/env python3
"""Exercise the real daemon; simulate only interface discovery and HTTP transport."""
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

SOURCE = Path(__file__).resolve().parents[1]

class NetworkGate(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='fwctld-network-')
        self.root = Path(self.temp.name)
        self.state = self.root/'state'
        self.state.write_text('down')
        self.calls = self.root/'calls'
        self.bin = self.root/'bin'; self.bin.mkdir()
        curl = self.bin/'curl'
        curl.write_text('#!/bin/sh\necho request >> "$TEST_CALLS"\nexit 22\n')
        curl.chmod(0o755)
        fixture = self.root/'interfaces.c'
        fixture.write_text(r'''
#define _DEFAULT_SOURCE
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct ifaddrs entry;
static struct sockaddr_in v4;
static struct sockaddr_in6 v6;
int getifaddrs(struct ifaddrs **out) {
    char state[32] = "down";
    FILE *f = fopen(getenv("TEST_NETWORK_STATE"), "r");
    if (f) { (void)fscanf(f, "%31s", state); fclose(f); }
    if (!strcmp(state,"error")) return -1;
    *out = NULL;
    if (!strcmp(state,"missing")) return 0;
    memset(&entry,0,sizeof(entry)); memset(&v4,0,sizeof(v4)); memset(&v6,0,sizeof(v6));
    entry.ifa_name = "test0"; entry.ifa_flags = IFF_UP | IFF_RUNNING;
    if (!strcmp(state,"down")) entry.ifa_flags = IFF_UP;
    if (!strcmp(state,"admin-down")) entry.ifa_flags = IFF_RUNNING;
    v4.sin_family = AF_INET; v4.sin_addr.s_addr = htonl(0xc0000201);
    entry.ifa_addr = (void *)&v4;
    if (!strcmp(state,"no-address")) entry.ifa_addr = NULL;
    if (!strcmp(state,"zero")) v4.sin_addr.s_addr = 0;
    if (!strncmp(state,"ipv6",4)) {
        v6.sin6_family = AF_INET6; entry.ifa_addr = (void *)&v6;
        if (!strcmp(state,"ipv6-link")) { v6.sin6_addr.s6_addr[0]=0xfe; v6.sin6_addr.s6_addr[1]=0x80; }
        if (!strcmp(state,"ipv6-global")) { v6.sin6_addr.s6_addr[0]=0x20; v6.sin6_addr.s6_addr[1]=0x01; }
    }
    *out=&entry; return 0;
}
void freeifaddrs(struct ifaddrs *p) { (void)p; }
''')
        preload = self.root/'interfaces.so'
        subprocess.run(['cc','-Wall','-Wextra','-Werror','-shared','-fPIC',str(fixture),'-o',str(preload)],check=True)
        self.env = dict(os.environ, LD_PRELOAD=str(preload), TEST_NETWORK_STATE=str(self.state), TEST_CALLS=str(self.calls), PATH=str(self.bin)+':'+os.environ['PATH'])
        self.args = [str(SOURCE/'fwctld'),'--server','http://127.0.0.1:1','--cache',str(self.root/'cache'),'--product','test','--layout','test','--interface','test0','--interval','1']

    def tearDown(self):
        self.temp.cleanup()

    def test_unready_never_calls_http(self):
        for state in ('missing','down','admin-down','no-address','zero','ipv6-zero','ipv6-link','error'):
            with self.subTest(state=state):
                self.state.write_text(state)
                r = subprocess.run(self.args+['--once'],env=self.env,capture_output=True,text=True,timeout=3)
                self.assertEqual(r.returncode,1)
                self.assertIn('Waiting for network',r.stdout)
                self.assertNotIn('transaction reconciliation incomplete',r.stdout)
                self.assertFalse(self.calls.exists())

    def test_ready_does_not_hide_http_errors(self):
        for state in ('up','ipv6-global'):
            with self.subTest(state=state):
                self.state.write_text(state)
                r = subprocess.run(self.args+['--once'],env=self.env,capture_output=True,text=True,timeout=3)
                self.assertEqual(r.returncode,1)
                self.assertTrue(self.calls.exists())
                self.assertIn('Cannot fetch a valid publication manifest',r.stdout)
                self.assertNotIn('Waiting for network',r.stdout)
                self.calls.unlink()

    def test_link_recovery_loss_and_prompt_stop(self):
        log = self.root/'log'
        with log.open('w') as stream:
            proc = subprocess.Popen(self.args,env=self.env,stdout=stream,stderr=stream)
            def wait_for(predicate):
                end=time.monotonic()+5
                while time.monotonic()<end:
                    if predicate(): return
                    time.sleep(.05)
                self.fail(log.read_text())
            try:
                wait_for(lambda:'Waiting for network' in log.read_text())
                time.sleep(1.2)
                self.assertEqual(log.read_text().count('Waiting for network'),1)
                self.assertFalse(self.calls.exists())
                self.state.write_text('up')
                wait_for(lambda:self.calls.exists())
                self.assertNotIn('Network ready:',log.read_text())
                self.state.write_text('down')
                wait_for(lambda:log.read_text().count('Waiting for network')==2)
                count=self.calls.read_text()
                time.sleep(1.2)
                self.assertEqual(self.calls.read_text(),count)
                proc.terminate()
                self.assertEqual(proc.wait(timeout=2),0)
            finally:
                if proc.poll() is None: proc.kill(); proc.wait()

if __name__ == '__main__': unittest.main()
