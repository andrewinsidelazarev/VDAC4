"""Регистровое ядро 6502 и таймеры: сверка с cRSID на уровне одной команды."""
import pathlib
import subprocess
import unittest

from sound_paths import ROOT, FIRMWARE
from test_sid_source import CLANG

BUILD = ROOT / '.test-build' / 'sid_cpu'


class SidCpuTest(unittest.TestCase):
    def test_generated_source_is_current(self):
        import sys
        result = subprocess.run([sys.executable, str(ROOT / 'tools/make_sid_cpu.py'), '--check'],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_cpu_and_timers_match_reference(self):
        BUILD.mkdir(parents=True, exist_ok=True)
        exe = BUILD / 'sid_cpu_host.exe'
        result = subprocess.run([str(CLANG), '-std=gnu11', '-O2', '-w',
            '-I', str(ROOT / 'tests/stubs_sid'), '-I', str(ROOT / 'components/crsid'),
            '-I', str(FIRMWARE / 'modules/sdk/include'),
            str(ROOT / 'tests/sid_cpu_host.c'), '-o', str(exe)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('cpu_cases=32768', result.stdout)
        self.assertIn('cached_cpu_cases=32768', result.stdout)
        self.assertIn('operand_fetch_cases=2688', result.stdout)
        self.assertIn('bank_read_cases=245760', result.stdout)
        self.assertIn('cia_cache_bus_cases=4096', result.stdout)
        self.assertIn('cia_cases=100000', result.stdout)
        self.assertIn('cia_cascade_cases=100001', result.stdout)
        self.assertIn('cia_plan_cases=160000', result.stdout)
        self.assertIn('vic_cases=100000', result.stdout)

    def check_dsp_host(self, name, expected):
        BUILD.mkdir(parents=True, exist_ok=True)
        exe = BUILD / (name + '.exe')
        result = subprocess.run([str(CLANG), '-std=gnu11', '-O2', '-w',
            '-I', str(ROOT / 'tests/stubs_sid'), '-I', str(ROOT / 'components/crsid'),
            '-I', str(FIRMWARE / 'modules/sdk/include'),
            str(ROOT / 'tests' / (name + '.c')), '-o', str(exe)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(expected, result.stdout)

    def test_independent_voice_matches_oversampling(self):
        self.check_dsp_host('sid_voice_host', 'voice_cases=200000')

    def test_cached_filter_preserves_integrator_state(self):
        self.check_dsp_host('sid_params_host', 'parameter_filter_cases=200000')

    def test_osc3_matches_reference_waves(self):
        self.check_dsp_host('sid_osc3_host', 'osc3_cases=200000')


if __name__ == '__main__':
    unittest.main()
