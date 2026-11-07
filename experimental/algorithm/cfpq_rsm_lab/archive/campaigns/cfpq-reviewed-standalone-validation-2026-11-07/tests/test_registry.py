"""Registry, source isolation, patch proofs and pinned-input fail-closed checks."""
import argparse
import csv
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

LAB = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(LAB));sys.path.insert(0,str(LAB/'tests'))
import labstate
import run
import make_profile_variant as profile


class RegistryTests(unittest.TestCase):
    def test_profile_returns_ignore_comments_and_literals(self):
        prefix = '// call transitions return\n// here\n/* return wrong; */\nputs("return wrong;");\n'
        source = prefix + 'return helper("return literal;", 1);\n'
        result = profile._profile_returns(source, 'fusion_s', 'fusion_calls')
        self.assertTrue(result.startswith(prefix))
        self.assertEqual(result.count('CFPQ_PROFILE_RETURN('), 1)
        self.assertIn('(helper("return literal;", 1))', result)

    def test_every_registry_variant_runs_the_generator(self):
        for name,row in labstate.registry().items():
            with self.subTest(name=name):
                source=labstate.source(name,allow_archived=True).read_text()
                if row['profiling'].startswith('unsupported-historical'):
                    self.assertEqual(row['status'],'archived')
                    with self.assertRaisesRegex(SystemExit,'anchor'):
                        profile.instrument(source)
                else:
                    generated=profile.instrument(source)
                    self.assertIn('CFPQ_RSM_PROFILE',generated)
                    self.assertEqual(labstate.digest(LAB/row['source']),row['sha256'])

    def test_all_diffs_apply_and_match_frozen_sources(self):
        self.assertEqual(labstate.verify_diffs(),len(labstate.registry()))

    def test_active_are_standalone_and_stack_is_explicit(self):
        rows=labstate.registry()
        names=['baseline','p1','p2','p3','p4','stack_p1234']
        reviewed = ['p1_review','p2_review','p3_review','p4_review']
        self.assertEqual([n for n,r in rows.items() if r['status']=='active'],names+reviewed)
        for n in names[1:]:self.assertEqual(rows[n]['parent'],'baseline')
        for n in reviewed:
            self.assertEqual(rows[n]['parent'], n.removesuffix('_review'))
            self.assertEqual(rows[n]['diff_parent'], 'baseline')
        texts={n:labstate.source(n).read_text() for n in names}
        self.assertNotIn('s_build_groups',texts['p2'])
        self.assertNotIn('s_compact_build',texts['p3'])
        self.assertNotIn('s_shift_compact',texts['p4'])
        self.assertNotIn('s_build_groups',texts['p4'])
        self.assertEqual(len({rows[n]['sha256'] for n in names}),6)
        self.assertEqual((LAB.parent/'LAGraph_CFPQ_RSM.c').read_bytes(),labstate.source('baseline').read_bytes())
        self.assertNotIn('s_build_groups',labstate.source('p2_review').read_text())
        self.assertNotIn('s_compact_build',labstate.source('p3_review').read_text())
        self.assertNotIn('s_shift_compact',labstate.source('p4_review').read_text())
        self.assertNotIn('s_build_groups',labstate.source('p4_review').read_text())

    def test_stray_flat_baseline_cannot_shadow_registry(self):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);(root/'variants').mkdir()
            (root/'variants/baseline.c').write_text('wrong source')
            expected=labstate.source('baseline')
            args=argparse.Namespace(variants_dir=root/'variants',allow_archived=False)
            with patch.object(run,'LAB_DIR',root),patch.object(run.labstate,'source',return_value=expected) as resolve:
                self.assertEqual(run.variant_source('baseline',args,root/'out'),expected)
                resolve.assert_called_once_with('baseline',allow_archived=False)

    def test_tampered_source_is_rejected(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t)/'source.c';p.write_text('changed');p.chmod(0o444)
            row={'source':str(p),'sha256':'0'*64,'status':'active'}
            with patch.object(labstate,'registry',return_value={'fixture':row}):
                with self.assertRaisesRegex(SystemExit,'source hash mismatch'):labstate.source('fixture')


class LockTests(unittest.TestCase):
    def test_changed_rsm_is_rejected(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t)/'grammar.rsm';p.write_text('original')
            lock={'files':{str(p):{'sha256':labstate.digest(p)}},'runtime_files':[],
                  'cfg_bench':{'path':str(p),'sha256':labstate.digest(p)}}
            labstate.verify_lock(lock)
            p.write_text('mutated')
            with self.assertRaisesRegex(SystemExit,'hash mismatch'):labstate.verify_lock(lock)

    def test_derived_rsms_and_source_sizes_are_pinned(self):
        lock=labstate.load_lock()
        self.assertEqual(lock['recipes']['full']['source_sizes'],[1,10,100])
        for name in lock['files']:
            if name.endswith('.cnf'):self.assertIn(str(Path(name).with_suffix('.rsm')),lock['files'])

    def test_unpinned_binary_and_build_are_rejected(self):
        lock=labstate.load_lock();r=lock['recipes']['smoke']
        args=argparse.Namespace(input_manifest=labstate.LOCK,skip_cfg_build=False)
        with self.assertRaisesRegex(SystemExit,'skip-cfg-build'):labstate.validate_run(args,[])
        args.skip_cfg_build=True;args.cfg_bench=Path('/tmp/not-the-pinned-binary')
        with self.assertRaisesRegex(SystemExit,'pinned binary'):labstate.validate_run(args,[])

    def test_unpinned_recipe_is_rejected(self):
        lock=labstate.load_lock();r=lock['recipes']['smoke']
        args=argparse.Namespace(input_manifest=labstate.LOCK,skip_cfg_build=True,
            cfg_bench=Path(lock['cfg_bench']['path']),config=Path(r['config']),profile=False,
            source_size=1,seed=999,chunks=1,rounds=1,threads=1,hot_rounds=1,order='blocked')
        with self.assertRaisesRegex(SystemExit,'not a pinned recipe'):labstate.validate_run(args,[])

if __name__=='__main__':unittest.main()
