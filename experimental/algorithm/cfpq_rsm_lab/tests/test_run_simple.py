"""Small regressions for per-chunk repetitions and result acceptance."""
from copy import deepcopy
from contextlib import redirect_stdout
import csv
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_simple


class SimpleRunnerTests(unittest.TestCase):
    def setUp(self):
        self.enterContext(redirect_stdout(io.StringIO()))
        self.key = ('graph.g', 'grammar.cnf', 1, 0)
        row = dict(algo_name='CFL_CFPQ_RSM', result='2', result_hash='123', time_s='0.01')
        self.results = {name: {self.key: [dict(row) for _ in range(5)]}
                        for name in ['baseline', 'p1_review']}

    def check(self, results):
        run_simple.check_correctness(results, {self.key}, 5)

    def test_five_matching_samples_pass(self):
        self.check(self.results)

    def test_missing_sample_fails(self):
        self.results['p1_review'][self.key].pop()
        with self.assertRaisesRegex(ValueError, 'expected 5 measurements'):
            self.check(self.results)

    def test_extra_case_fails(self):
        self.results['p1_review'][('graph.g', 'grammar.cnf', 10, 0)] = []
        with self.assertRaisesRegex(ValueError, 'unexpected cases'):
            self.check(self.results)

    def test_equal_count_but_different_hash_fails(self):
        self.results['p1_review'][self.key][2]['result_hash'] = '124'
        with self.assertRaisesRegex(ValueError, 'differs from baseline'):
            self.check(self.results)

    def test_unstable_baseline_fails(self):
        self.results['baseline'][self.key][3]['result'] = '3'
        with self.assertRaisesRegex(ValueError, 'differs from baseline'):
            self.check(self.results)

    def test_pair_without_baseline_uses_selected_reference(self):
        results = {'p1_review': deepcopy(self.results['baseline']),
                   'p1_full': deepcopy(self.results['p1_review'])}
        self.assertEqual(run_simple.reference_variant(list(results)), 'p1_review')
        run_simple.check_correctness(results, {self.key}, 5, 'p1_review')
        results['p1_review'][self.key][4]['result_hash'] = '999'
        with self.assertRaisesRegex(ValueError, 'differs from p1_review'):
            run_simple.check_correctness(results, {self.key}, 5, 'p1_review')

    def test_reference_must_be_selected_and_variants_distinct(self):
        for names, reference in [([], None), (['p1_review', 'p1_review'], None),
                                 (['p1_review', 'p1_full'], 'baseline')]:
            with self.subTest(names=names, reference=reference), self.assertRaises(ValueError):
                run_simple.reference_variant(names, reference)
        self.assertEqual(run_simple.reference_variant(['p1_full', 'p1_review'], 'p1_review'), 'p1_review')
        self.assertEqual(run_simple.reference_variant(['p1_review', 'baseline']), 'baseline')

    def test_analysis_records_ratio_against_nonbaseline_reference(self):
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary)
            settings = dict(variants=['p1_review', 'p1_full'], reference='p1_review',
                            source_sizes=[1], chunks=1, rounds=5)
            (out/'settings.json').write_text(json.dumps(settings))
            (out/'config.csv').write_text('graph.g,grammar.cnf,-1,pool.result\n')
            for name, seconds in [('p1_review', '0.01'), ('p1_full', '0.005')]:
                fields = ['graph','cnf','source_size','chunk','algo_name','result','result_hash','time_s']
                with (out/f'{name}.csv').open('w', newline='') as stream:
                    writer = csv.DictWriter(stream, fieldnames=fields)
                    writer.writeheader()
                    writer.writerows(dict(graph='graph.g',cnf='grammar.cnf',source_size=1,chunk=0,
                                          algo_name='CFL_CFPQ_RSM',result=2,result_hash=123,time_s=seconds)
                                     for _ in range(5))
            run_simple.analyze(out)
            with (out/'summary.csv').open() as stream:
                rows = list(csv.DictReader(stream))
            self.assertTrue(all(row['reference']=='p1_review' for row in rows))
            self.assertAlmostEqual(float(rows[1]['geometric_time_ratio']), 0.5)

    def test_invalid_times_and_missing_hash_fail(self):
        for field, value in [('time_s', 'nan'), ('time_s', '0'), ('result_hash', '0')]:
            with self.subTest(field=field, value=value):
                results = deepcopy(self.results)
                results['p1_review'][self.key][0][field] = value
                with self.assertRaises(ValueError):
                    self.check(results)

    def test_repetitions_go_directly_to_cfg_bench(self):
        args = SimpleNamespace(source_sizes='1,10,100', chunks=30, seed=2026, rounds=5, hot=True)
        cmd = run_simple.benchmark_command('cfg_bench', 'java.csv', 'p1.so', 'p1.csv', args)
        self.assertEqual(cmd[cmd.index('-r')+1], '5')
        self.assertEqual(cmd[cmd.index('--source-sizes')+1], '1,10,100')
        self.assertEqual(cmd.count('--hot'), 1)
        self.assertNotIn('--threads', cmd)
        args.hot = False
        self.assertNotIn('--hot', run_simple.benchmark_command('cfg_bench', 'java.csv', 'p1.so', 'p1.csv', args))


if __name__ == '__main__':
    unittest.main()
