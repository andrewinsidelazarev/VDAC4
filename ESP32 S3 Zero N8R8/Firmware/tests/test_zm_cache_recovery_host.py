"""Кэш модулей ESP (src/zm_cache.cpp) после обрыва питания — на ПК.

tests/zm_cache_recovery_host.cpp: питание пропадает после каждой операции записи
или удаления при вытеснении и при замене модуля; после перезагрузки каждая
запись указателя читается с прежним содержимым, а файлов вне указателя нет;
модуль, который не прочитался, выбрасывается; load сверяет указатель с файлами
(пропавший, другой длины, лишний). Без Visual Studio Build Tools тест пропускается.
"""
import subprocess
import unittest

from test_weather_parse import ROOT, build_exe

BUILD = ROOT / '.test-build' / 'zm_cache_recovery_host'
SCENARIOS = ['power_cut_during_store_with_eviction', 'power_cut_during_replace',
             'unreadable_module_is_dropped', 'load_reconciles_index_with_files']


class ZmCacheRecoveryHostTest(unittest.TestCase):
    def test_recovery(self) -> None:
        exe = build_exe(BUILD, 'zm_cache_recovery_host.exe', [ROOT / 'include'],
                        [(ROOT / 'src' / 'zm_cache.cpp', 'cache.obj'),
                         (ROOT / 'tests' / 'zm_cache_recovery_host.cpp', 'host.obj')])
        if exe is None:
            self.skipTest('нет MSVC (vcvars64.bat): стенд кэша пропущен')
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.split(), [word for name in SCENARIOS for word in ('ok', name)])


if __name__ == '__main__':
    unittest.main()
