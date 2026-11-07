"""Status must not turn incomplete, changed or newer failed evidence into PASS."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

LAB=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(LAB))
import experiment_status as status
import labstate


class ReceiptTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)
        self.addCleanup(patch.stopall)
        patch.object(status,'LAB',self.root).start()
        (self.root/'tool.py').write_text('tool revision 1')
        (self.root/'check.log').write_text('completed')
        self.receipt={'schema':1,'kind':'other','variants':{'p1':'a'*64},
            'input_manifest_sha256':'lock-v1','outcome':'PASS','exit_code':0,
            'started_at':'2026-11-07T10:00:00+00:00','finished_at':'2026-11-07T10:01:00+00:00',
            'tools_and_sources':{'tool.py':labstate.digest(self.root/'tool.py')},
            'artifacts':{'check.log':labstate.digest(self.root/'check.log')},'log':'check.log'}

    def verify(self, receipt=None, variants=None, lock='lock-v1'):
        return status.verify_receipt(receipt or self.receipt,variants or {'p1':'a'*64},lock)

    def test_verified_zero_exit_receipt_can_pass(self):
        self.assertEqual(self.verify()['status'],'PASS')

    def test_changed_source_is_stale(self):
        self.assertEqual(self.verify(variants={'p1':'b'*64})['status'],'STALE')

    def test_changed_tool_is_stale(self):
        (self.root/'tool.py').write_text('tool revision 2')
        self.assertEqual(self.verify()['status'],'STALE')

    def test_modified_or_missing_artifact_is_stale(self):
        (self.root/'check.log').write_text('edited after execution')
        self.assertEqual(self.verify()['status'],'STALE')
        (self.root/'check.log').unlink()
        self.assertEqual(self.verify()['status'],'STALE')

    def test_changed_manifest_is_stale(self):
        self.assertEqual(self.verify(lock='lock-v2')['status'],'STALE')

    def test_newer_failure_supersedes_older_pass(self):
        newer={**self.receipt,'finished_at':'2026-11-07T11:00:00+00:00','exit_code':1,'outcome':'FAIL'}
        selected=status.latest_attempt([self.receipt,newer])
        self.assertIs(selected,newer)
        self.assertEqual(self.verify(selected)['status'],'FAIL')

    def test_newer_timeout_is_incomplete(self):
        newer={**self.receipt,'finished_at':'2026-11-07T11:00:00+00:00','exit_code':None,'outcome':'INCOMPLETE'}
        self.assertEqual(self.verify(status.latest_attempt([self.receipt,newer]))['status'],'INCOMPLETE')

    def test_exit_zero_does_not_prove_the_oracle_suite_completed(self):
        receipt={**self.receipt,'kind':'oracle_existing'}
        self.assertEqual(self.verify(receipt)['status'],'FAIL')

    def test_tooling_requires_an_actual_complete_test_result(self):
        (self.root/'check.log').write_text('Ran 53 tests in 1.0s\n\nFAILED (failures=1)\n')
        receipt={**self.receipt,'kind':'tooling','artifacts':{'check.log':labstate.digest(self.root/'check.log')}}
        self.assertEqual(self.verify(receipt)['status'],'FAIL')
        (self.root/'check.log').write_text('Ran 53 tests in 1.0s\n\nOK\n')
        receipt['artifacts']['check.log']=labstate.digest(self.root/'check.log')
        self.assertEqual(self.verify(receipt)['tests'],53)

    def test_receipt_files_are_not_overwritten(self):
        context={k:v for k,v in self.receipt.items() if k not in ('outcome','exit_code','artifacts')}
        status.record(context,self.root,'check',self.root/'check.log',self.receipt['started_at'],0,'PASS')
        before=(self.root/'check.result.json').read_bytes()
        with self.assertRaisesRegex(SystemExit,'overwrite'):
            status.record(context,self.root,'check',self.root/'check.log',self.receipt['started_at'],1,'FAIL')
        self.assertEqual((self.root/'check.result.json').read_bytes(),before)


class CoverageTests(unittest.TestCase):
    def test_one_source_size_is_not_complete_performance_coverage(self):
        by_size={'1':{'status':'NOT_RUN'},'10':{'status':'NOT_RUN'},'100':{'status':'PASS'}}
        self.assertEqual(status.coverage_status(by_size),{'status':'PARTIAL','measured_source_sizes':[100]})
        by_size['10']['status']='FAIL'
        self.assertEqual(status.coverage_status(by_size)['status'],'NEEDS_ATTENTION')

    def test_all_planned_sizes_are_required(self):
        self.assertEqual(status.coverage_status({str(n):{'status':'PASS'} for n in (1,10,100)})['status'],'PASS')


class StartHistoryTests(unittest.TestCase):
    def test_unfinished_start_is_visible_and_completed_start_is_not_a_false_failure(self):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);out=root/'runs/run';out.mkdir(parents=True)
            (out/'command.log').write_text('command started')
            context={'schema':1,'campaign':'current','kind':'tooling','variants':{},
                     'tools_and_sources':{},'input_manifest_sha256':'lock'}
            with patch.object(status,'LAB',root):
                status.record_start(context,out,'command',out/'command.log','2026-11-07T10:00:00+00:00')
                attempts,problems=status.read_attempts('current')
                self.assertEqual(problems,[])
                self.assertEqual(len(attempts),1)
                self.assertEqual(attempts[0]['outcome'],'INCOMPLETE')
                status.record(context,out,'command',out/'command.log','2026-11-07T10:00:00+00:00',0,'PASS')
                attempts,problems=status.read_attempts('current')
                self.assertEqual(problems,[])
                self.assertEqual(len(attempts),1)
                self.assertEqual(attempts[0]['outcome'],'PASS')
                # A corrupted completion cannot make its unfinished start disappear.
                (out/'command.result.json').write_text('{broken')
                attempts,problems=status.read_attempts('current')
                self.assertEqual(len(problems),1)
                self.assertEqual(attempts[0]['outcome'],'INCOMPLETE')
