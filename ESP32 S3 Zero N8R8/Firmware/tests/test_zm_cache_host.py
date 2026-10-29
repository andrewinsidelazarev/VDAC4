"""Кэш модулей ESP (src/zm_cache.cpp) на ПК.

tests/zm_cache_host.cpp: хранилище в памяти с заданной ёмкостью. Проверяется:
- место считается целыми блоками LittleFS, свободное — ёмкость минус файлы
  кэша и его указатель (не свободное место всей LittleFS: в vdac4-0.1.23 оно
  читалось как 0 КБ при пустом разделе);
- модуль кладётся, читается, переживает перезагрузку указателя; другая длина
  или время — промах; то же имя заменяет прежнюю копию;
- вытесняется давно не бравшийся модуль, активное приложение — никогда;
- не помещающийся даже после вытеснения — отказ без вытеснения; ELF брошенного
  приложения (HMM2.ELF, 29.10.2026) держит место VIDEO.ELF, пока он busy; неудачная
  запись не оставляет записи; имена — только 8.3 заглавными.
Без Visual Studio Build Tools тест пропускается.
"""
import subprocess
import unittest

from test_weather_parse import ROOT, build_exe

BUILD = ROOT / '.test-build' / 'zm_cache_host'
SCENARIOS = [
    'footprint_counts_whole_blocks', 'empty_cache_has_its_capacity',
    'modules_are_stored_read_and_survive_reload', 'same_name_replaces_the_old_copy',
    'least_recently_used_goes_first_busy_stays', 'too_big_is_refused_without_evicting',
    'abandoned_app_holds_the_room',
    'failed_write_leaves_no_entry', 'names_are_8_3', 'no_capacity_no_store',
]


class ZmCacheHostTest(unittest.TestCase):
    def test_cache(self) -> None:
        exe = build_exe(BUILD, 'zm_cache_host.exe', [ROOT / 'include'],
                        [(ROOT / 'src' / 'zm_cache.cpp', 'cache.obj'),
                         (ROOT / 'tests' / 'zm_cache_host.cpp', 'host.obj')])
        if exe is None:
            self.skipTest('нет MSVC (vcvars64.bat): стенд кэша пропущен')
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.split(), [word for name in SCENARIOS
                                                 for word in ('ok', name)])


if __name__ == '__main__':
    unittest.main()
