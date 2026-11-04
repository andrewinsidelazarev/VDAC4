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
        self.assertIn('cia_cases=100000', result.stdout)
        self.assertIn('vic_cases=100000', result.stdout)


if __name__ == '__main__':
    unittest.main()
