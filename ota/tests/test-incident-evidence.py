#!/usr/bin/env python3
"""Real HTTP incident archives, separate from the disposable SOL window."""
import importlib.util
import json
from pathlib import Path
import sqlite3
import unittest

spec=importlib.util.spec_from_file_location('sol_tests',Path(__file__).with_name('test-sol.py'))
sol_tests=importlib.util.module_from_spec(spec);spec.loader.exec_module(sol_tests)


class IncidentEvidence(sol_tests.SOL):
    def report(self,phase,observed='source-a',reason='none',source='source-a',publication=1):
        body=f'BSPOTASTATE2 device-a test test {publication} 1 {source} {phase} 1 1 {reason}\n'.encode()
        headers={'X-OTA-Request':'1','X-OTA-Device':'device-a','X-OTA-IP':'192.0.2.10',
                 'X-OTA-MAC':'02:00:00:00:00:01','X-OTA-Hostname':'test-board','X-OTA-Boot':observed}
        return self.request('/api/devices/device-a',data=body,key=self.key,headers=headers)

    # OTA replies are plain text; preserve the existing JSON helper for SOL only.
    def request(self,path,data=None,key=None,headers=None):
        if not path.startswith('/api/devices/'):
            return super().request(path,data,key,headers)
        import urllib.request
        import urllib.error
        request=urllib.request.Request(self.url+path,data=data,headers=headers or {})
        try:response=self.http.open(request,timeout=5)
        except urllib.error.HTTPError as error:response=error
        with response:return response.status,response.read()

    def evidence(self,source='source-a',publication=1):
        return self.request(f'/api/sol/incidents/device-a/{publication}/{source}')

    def chunks(self,boot,source='source-a',publication=1,cursor=0):
        return self.request(f'/api/sol/incidents/device-a/{publication}/{source}/{boot}/{cursor}')

    def test_failed_attempts_are_distinct_and_survive_live_retention(self):
        self.assertEqual(self.report('installing')[0],200)
        self.push(b'source-upgrade-log\n',boot='source-a')
        for order,boot in enumerate(('attempt-a','attempt-b'),2):
            self.assertEqual(self.report('boot-testing',observed=boot)[0],200)
            self.push((boot+' health-failed\n').encode(),boot=boot,order=order)
        for i in range(6):self.push(b'new boot\n',boot=f'other-{i}',order=i+10)
        self.assertEqual(self.logs('attempt-a'),[])
        self.assertEqual(self.report('rolled-back',observed='recovered',reason='boot-failed-rolled-back')[0],200)
        status,meta=self.evidence();self.assertEqual(status,200)
        self.assertEqual(meta['mode'],'logs');self.assertFalse(meta['complete'])
        self.assertEqual(meta['observed_boot'],'recovered')
        self.assertEqual({b['boot'] for b in meta['boots']},{'attempt-a','attempt-b'})
        for boot in ('attempt-a','attempt-b'):
            status,rows=self.chunks(boot);self.assertEqual(status,200)
            self.assertEqual(''.join(r['text'] for r in rows),boot+' health-failed\n')
        self.assertEqual(self.chunks('source-a')[1],[])
        self.stop();self.start()
        self.assertEqual(self.evidence()[1],meta)

    def test_delayed_upload_is_attached_and_reupload_does_not_duplicate(self):
        self.report('boot-testing',observed='attempt-a')
        self.report('rolled-back',observed='recovered',reason='boot-failed-rolled-back')
        self.assertEqual(self.evidence()[1]['boots'][0]['bytes'],0)
        self.push(b'late log\n',boot='attempt-a')
        self.assertEqual(self.chunks('attempt-a')[1][0]['text'],'late log\n')
        for i in range(6):self.push(boot=f'other-{i}',order=i+10)
        self.push(b'late log\n',boot='attempt-a',order=1)
        self.assertEqual(len(self.chunks('attempt-a')[1]),1)

    def test_linux_and_data_failures_have_reason_only_and_no_log_access(self):
        for reason in ('linux-panic-observed','data-read-only-observed','data-unavailable-observed'):
            with self.subTest(reason=reason):
                source=reason+'-source'
                self.report('boot-testing',observed='attempt-'+reason,source=source)
                self.push(b'must not attach\n',boot='attempt-'+reason)
                self.report('rolled-back',observed='recovered',reason=reason,source=source)
                meta=self.evidence(source=source)[1]
                self.assertEqual(meta['mode'],'reason-only');self.assertEqual(meta['reason'],reason)
                self.assertEqual(meta['boots'],[]);self.assertEqual(meta['bytes'],0)
                self.assertEqual(self.chunks('attempt-'+reason,source=source)[0],404)
                self.push(b'more\n',boot='attempt-'+reason,offset=16)
                self.assertEqual(self.evidence(source=source)[1]['bytes'],0)

    def test_no_failed_boot_is_not_replaced_by_the_recovery_boot(self):
        self.report('installing');self.push(b'old system\n',boot='source-a')
        self.report('rolled-back',observed='recovered',reason='boot-failed-rolled-back')
        meta=self.evidence()[1]
        self.assertEqual(meta['mode'],'reason-only');self.assertEqual(meta['boots'],[])
        self.assertEqual(self.chunks('source-a')[0],404)

    def test_data_observation_survives_a_healthy_recovery_boot(self):
        self.report('installing');self.push(b'pre-upgrade\n',boot='source-a')
        self.report('boot-testing',observed='attempt-a',reason='data-unavailable-observed')
        self.push(b'do not attach\n',boot='attempt-a')
        self.report('rolled-back',observed='recovered',reason='boot-failed-rolled-back')
        meta=self.evidence()[1]
        self.assertEqual(meta['mode'],'reason-only')
        self.assertEqual(meta['reason'],'data-unavailable-observed')
        self.assertEqual(meta['observed_boot'],'attempt-a')
        self.assertEqual(meta['boots'],[]);self.assertEqual(meta['bytes'],0)

    def test_install_failure_keeps_source_boot_logs(self):
        self.report('installing');self.push(b'write failed\n',boot='source-a')
        self.report('install-failed',reason='install-command-failed')
        meta=self.evidence()[1];self.assertEqual(meta['mode'],'logs')
        self.assertEqual([(b['boot'],b['role']) for b in meta['boots']],[('source-a','source')])

    def test_success_removes_only_pending_evidence_and_auth_is_enforced(self):
        self.report('boot-testing',observed='attempt-a');self.push(boot='attempt-a')
        self.report('success',observed='attempt-a')
        self.assertEqual(self.evidence()[0],404)
        self.assertEqual(self.request('/api/sol/incidents/device-a/1/source-a',key='0'*64)[0],403)
        self.assertEqual(self.request('/api/sol/incidents/device-a/01/source-a')[0],400)

    def test_archive_budget_is_explicit_and_does_not_block_SOL(self):
        self.report('boot-testing',observed='attempt-a')
        with sqlite3.connect(self.feed/'sol.db') as db:
            db.execute('update evidence_groups set bytes=33554430')
        self.assertEqual(self.push(b'over-budget\n',boot='attempt-a')[0],200)
        self.report('rolled-back',observed='recovered',reason='boot-failed-rolled-back')
        meta=self.evidence()[1]
        self.assertTrue(meta['truncated']);self.assertEqual(self.chunks('attempt-a')[1],[])

    def test_archive_failure_does_not_lose_the_upgrade_result(self):
        self.report('installing');self.push(b'saved despite archive retry\n',boot='source-a')
        with sqlite3.connect(self.feed/'sol.db') as db:
            db.execute('drop table evidence_chunks')
        self.assertEqual(self.report('install-failed',reason='install-command-failed')[0],200)
        status,state=self.request('/api/state')
        self.assertEqual(status,200)
        self.assertEqual(state['device_history'][0]['phase'],'install-failed')
        self.stop();self.start()
        self.assertEqual(self.request('/api/devices/device-a')[0],200)
        self.assertEqual(self.evidence()[1]['mode'],'logs')
        self.assertEqual(self.chunks('source-a')[1][0]['text'],'saved despite archive retry\n')


if __name__=='__main__':
    suite=unittest.TestSuite(IncidentEvidence(name) for name in IncidentEvidence.__dict__ if name.startswith('test_'))
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
