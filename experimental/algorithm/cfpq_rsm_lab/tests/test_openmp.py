"""The lab must leave OpenMP policy to the inherited runtime environment."""
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

LAB = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(LAB))
import lab
import labstate
import run


class OpenMPTests(unittest.TestCase):
    def test_child_environments_preserve_all_inherited_runtime_settings(self):
        inherited = {'OMP_NUM_THREADS': '3,2', 'OMP_THREAD_LIMIT': '7',
                     'OMP_DYNAMIC': 'TRUE', 'OMP_PROC_BIND': 'spread',
                     'OMP_PLACES': 'cores', 'GOMP_CPU_AFFINITY': '0-5',
                     'KMP_BLOCKTIME': '20', 'PATH': os.defpath}
        with patch.dict(os.environ, inherited, clear=True):
            child = lab.environment()
            before = dict(child)
            run.apply_thread_mode(child, 0)
            self.assertEqual(child, before)
            self.assertEqual(labstate.openmp_environment(child),
                             labstate.openmp_environment(inherited))
            self.assertEqual(dict(os.environ), inherited)

    def test_unset_openmp_settings_remain_unset(self):
        with patch.dict(os.environ, {'PATH': os.defpath}, clear=True):
            child = lab.environment()
            run.apply_thread_mode(child, 0)
            self.assertEqual(labstate.openmp_environment(child), {})

    def test_requested_thread_override_fails_without_changing_environment(self):
        for count in (-1, 1, 12):
            with self.subTest(count=count):
                child = {'OMP_NUM_THREADS': '3,2', 'OMP_THREAD_LIMIT': '7'}
                before = dict(child)
                with self.assertRaisesRegex(SystemExit, 'overrides are disabled'):
                    run.apply_thread_mode(child, count)
                self.assertEqual(child, before)

    def test_legacy_cli_option_cannot_force_a_thread_count(self):
        import contextlib
        import io
        with patch.object(sys, 'argv', ['run.py', '--threads', '12']):
            with contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    run.parse_args()

    def test_all_current_recipes_inherit_threading(self):
        lock = labstate.load_lock()
        self.assertEqual(lock['threading_policy'], 'inherit-openmp-environment')
        for name, recipe in lock['recipes'].items():
            with self.subTest(recipe=name):
                self.assertEqual(recipe['threads'], 0)


if __name__ == '__main__':
    unittest.main()
