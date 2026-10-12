"""Shared tracker/OPL leveler: gain convergence, gating, stereo, overload and seek."""
import subprocess
import unittest
from sound_paths import FIRMWARE, ROOT
from test_weather_parse import find_vcvars, short


class AudioLevelHostTest(unittest.TestCase):
    def test_streaming_leveler(self):
        vcvars = find_vcvars()
        if vcvars is None:
            self.skipTest('MSVC Build Tools not installed')
        build = ROOT / '.test-build' / 'audio_level_host'
        build.mkdir(parents=True, exist_ok=True)
        script = build / 'build.bat'
        script.write_text('\r\n'.join([
            '@echo off',
            f'call "{short(vcvars)}" >nul 2>nul || exit /b 1',
            'cl /nologo /O2 /utf-8 /EHsc /std:c++17 /I "' + short(FIRMWARE / 'modules/sdk/include') +
            '" "' + short(ROOT / 'tests/audio_level_host.cpp') + '" /Fe:audio_level_host.exe || exit /b 1',
        ]) + '\r\n', encoding='ascii')
        result = subprocess.run(['cmd.exe', '/d', '/c', short(script)], cwd=short(build), capture_output=True)
        self.assertEqual(result.returncode, 0, (result.stdout + result.stderr).decode('cp866', 'replace'))
        result = subprocess.run([str(build / 'audio_level_host.exe')], capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.count('PASS rate='), 3, result.stdout)


if __name__ == '__main__':
    unittest.main()
