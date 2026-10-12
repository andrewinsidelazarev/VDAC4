"""Кэш ресурсов музыки на флешке (src/resource_disk_cache.cpp) на ПК.

tests/resource_cache_host.cpp: каталог кэша — временная папка ПК, код тот же, что
в прошивке поверх FatFs (stdio, stat, utime). Проверяется:
- записанная копия годна со своим временем, читается байт в байт, временного
  файла не остаётся; файл длиннее куска чтения и записи (16 КБ) и не кратен ему;
  путь к каталогу кэша длиннее 140 знаков (папка «флешки» на ПК);
- файл на карте новее копии (хоть на 2 с) — копия негодна; старее — годна;
- копия другой длины или её отсутствие — негодна;
- повторная запись заменяет копию; чтение с неверной длиной — отказ;
- копия, положенная на флешку с ПК и новее файла на карте, годна;
- время FAT переживает перевод в time_t и обратно (в том числе час перевода
  часов осенью, когда 02:30 бывает дважды); наборы волн .MWK не кэшируются;
  у копии файла без даты (время FAT 0) — время записи, и она годна;
  запись в несуществующую папку — отказ.
Без Visual Studio Build Tools тест пропускается.
"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_weather_parse import ROOT, build_exe

BUILD = ROOT / '.test-build' / 'resource_cache_host'
SCENARIOS = [
    'stored_copy_is_fresh_and_reads_back', 'newer_card_file_makes_copy_stale',
    'other_length_is_stale', 'rewrite_replaces_the_copy', 'short_or_long_read_fails',
    'user_copy_newer_is_used', 'stamp_round_trip', 'kits_are_not_cached',
    'undated_card_file_keeps_write_time', 'missing_directory_fails_cleanly',
]


class ResourceCacheHostTest(unittest.TestCase):
    def test_cache(self) -> None:
        exe = build_exe(BUILD, 'resource_cache_host.exe', [ROOT / 'include'],
                        [(ROOT / 'src' / 'resource_disk_cache.cpp', 'cache.obj'),
                         (ROOT / 'tests' / 'resource_cache_host.cpp', 'host.obj')])
        if exe is None:
            self.skipTest('нет MSVC (vcvars64.bat): стенд кэша ресурсов пропущен')
        folder = Path(tempfile.mkdtemp(prefix='zifi_usb_'))
        # Каталог кэша с длинным путём, как у папки «флешки» в эмуляторе Unreal: в
        # первой версии путь длиннее 128 знаков обрезался, и запись не удавалась.
        cache = folder / ('long_folder_name_' * 4) / ('SYSTEM_CACHE_MUSIC_' * 3)
        cache.mkdir(parents=True)
        self.assertGreater(len(str(cache)), 140)
        try:
            result = subprocess.run([str(exe), str(cache)], capture_output=True, text=True,
                                    timeout=60)
        finally:
            shutil.rmtree(folder, ignore_errors=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.split(), [word for name in SCENARIOS
                                                 for word in ('ok', name)])


if __name__ == '__main__':
    unittest.main()
