#!/usr/bin/env python3
"""Finite startup evidence correlation, copied attachments and old-data backfill."""
import contextlib
import importlib.util
import json
from pathlib import Path
import sqlite3
import unittest
import urllib.request
import zlib
spec=importlib.util.spec_from_file_location('incident_fixture',Path(__file__).with_name('test-incident-evidence.py'))
fixture=importlib.util.module_from_spec(spec);spec.loader.exec_module(fixture)
class StartupIncident(fixture.IncidentEvidence):
    for _name in dir(fixture.IncidentEvidence):
        if _name.startswith('test_'):locals()[_name]=None
    @contextlib.contextmanager
    def database(self):
        db=sqlite3.connect(self.feed/'sol.db');db.execute('PRAGMA foreign_keys=ON')
        try:
            with db:yield db
        finally:db.close()
    def archive(self,capture,current,recovery='recovery-a',generation=None,slot=None,dropped=0):
        generation=(4 if current else 3) if generation is None else generation
        slot=current if slot is None else slot
        bodies={s:(s+' '+('healthy' if current else 'Kernel panic - failure')+'\n').encode() for s in ('r5-spl','a53-spl','uboot','kernel')}
        prefix='/api/sol/startup/device-a/'+capture+'/'
        manifest=f'{slot} {generation} {current} {recovery}\n'
        for stage,data in bodies.items():
            self.assertEqual(self.request(prefix+stage+'/0',data,key=self.key)[0],200)
            manifest+=f'{stage} {len(data)} {zlib.crc32(data):08x} {dropped if stage=="kernel" else 0} 1\n'
        self.assertEqual(self.request(prefix+'commit/0',manifest.encode(),key=self.key)[0],200)
        return bodies
    def pair(self,**kwargs):
        a=self.archive('failed-capture',0,**kwargs);b=self.archive('recovered-capture',1,**kwargs);return a,b
    def failure(self,source='source-a',recovery='recovery-a',publication=1):
        self.assertEqual(self.report('rolled-back',observed=recovery,reason='linux-panic-observed',source=source,publication=publication)[0],200)
    def test_event_before_archives_attaches_exact_adjacent_pair(self):
        self.failure();self.assertEqual(self.evidence()[1]['startup_archives'],[])
        failed=self.archive('failed-capture',0);self.assertEqual(self.evidence()[1]['startup_archives'],[])
        recovered=self.archive('recovered-capture',1);meta=self.evidence()[1]
        self.assertEqual(meta['mode'],'logs');self.assertEqual(meta['boots'],[])
        self.assertEqual([x['role'] for x in meta['startup_archives']],['failed-startup','recovery-startup'])
        self.assertTrue(all(x['complete'] for x in meta['startup_archives']))
        for capture,bodies in [('failed-capture',failed),('recovered-capture',recovered)]:
            rows=self.chunks(capture)[1];self.assertTrue(all(r['raw'] for r in rows))
            self.assertEqual(b''.join(r['text'].encode('latin1') for r in rows),b''.join(bodies.values()))
            self.assertEqual(self.chunks(capture,cursor=4)[1],[])
        self.assertEqual(self.chunks('recovery-a')[0],404)
    def test_archives_before_event_and_retry_are_idempotent(self):
        self.pair();self.failure();before=self.evidence()[1]
        self.failure();self.assertEqual(self.evidence()[1],before)
        self.stop();self.start();self.assertEqual(self.evidence()[1],before)
        with self.database() as db:
            self.assertEqual(db.execute('select count(*) from evidence_startups').fetchone()[0],2)
            self.assertEqual(db.execute('select count(*) from evidence_startup_streams').fetchone()[0],8)
    def test_existing_database_is_backfilled(self):
        self.pair();self.failure();expected=self.evidence()[1]
        self.stop()
        with self.database() as db:
            db.execute('DELETE FROM startup_context');db.execute('DELETE FROM evidence_startups')
        self.start();self.assertEqual(self.evidence()[1],expected)
    def test_old_evidence_is_backfilled_before_first_sol_delete(self):
        self.pair();self.failure();expected=self.evidence()[1]
        self.stop()
        with self.database() as db:
            db.execute('DELETE FROM startup_context');db.execute('DELETE FROM evidence_startups')
        self.start()
        req=urllib.request.Request(self.url+'/api/sol/records/device-a/failed-capture',method='DELETE',headers={'X-OTA-Request':'1','Authorization':'Bearer '+self.viewer})
        with self.http.open(req) as response:self.assertEqual(response.status,200)
        self.assertEqual(self.evidence()[1],expected)
    def test_sol_deletion_keeps_independent_incident_attachments(self):
        bodies,_=self.pair();self.failure();before=self.evidence()[1]
        for capture in ('failed-capture','recovered-capture'):
            req=urllib.request.Request(self.url+'/api/sol/records/device-a/'+capture,method='DELETE',headers={'X-OTA-Request':'1','Authorization':'Bearer '+self.viewer})
            with self.http.open(req) as response:self.assertEqual(response.status,200)
        self.assertEqual(self.request('/api/sol/devices')[1],[])
        self.assertEqual(self.evidence()[1],before)
        self.assertEqual(b''.join(r['text'].encode('latin1') for r in self.chunks('failed-capture')[1]),b''.join(bodies.values()))
        self.stop();self.start();self.assertEqual(self.evidence()[1],before)
    def test_wrong_recovery_generation_or_bank_is_never_guessed(self):
        for i,(recovery,generation,slot) in enumerate([('unrelated',4,1),('rec-1',6,1),('rec-2',4,0)]):
            context=f'rec-{i}';source=f'src-{i}';self.failure(source=source,recovery=context)
            self.archive(f'failed-{i}',0,recovery=context)
            self.archive(f'healthy-{i}',1,recovery=recovery,generation=generation,slot=slot)
            self.assertEqual(self.evidence(source=source)[1]['startup_archives'],[])
    def test_ambiguous_recovery_context_is_hidden_not_misattributed(self):
        self.pair();self.failure();self.assertEqual(len(self.evidence()[1]['startup_archives']),2)
        self.failure(source='another-source',publication=2)
        self.assertEqual(self.evidence()[1]['startup_archives'],[])
        self.assertEqual(self.chunks('failed-capture')[0],404)
        self.assertEqual(self.evidence(source='another-source',publication=2)[1]['startup_archives'],[])
    def test_dropped_bytes_are_marked_incomplete(self):
        self.failure();self.pair(dropped=11)
        for archive in self.evidence()[1]['startup_archives']:
            self.assertFalse(archive['complete']);self.assertEqual(archive['stages'][-1]['dropped'],11)
    def test_corrupted_archived_bytes_do_not_get_trusted_as_attachments(self):
        self.pair()
        with self.database() as db:db.execute("UPDATE startup_streams SET data=X'626164' WHERE boot='failed-capture' AND stage='kernel'")
        self.failure();self.assertEqual(self.evidence()[1]['startup_archives'],[])
    def test_source_boot_is_not_claimed_as_a_recovered_failure(self):
        self.pair(recovery='source-a');self.failure(recovery='source-a')
        self.assertEqual(self.evidence()[1]['startup_archives'],[])
if __name__=='__main__':unittest.main()
