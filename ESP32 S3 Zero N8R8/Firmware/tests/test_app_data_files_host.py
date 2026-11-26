"""Файлы данных приложений на флешке (src/app_data_files.cpp, 0.1.165) на ПК.

tests/app_data_files_host.cpp: «флешка» — временная папка ПК, код тот же, что в прошивке поверх FatFs
(stdio, stat, utime, rename). Проверяется:
- доставка кусками не по 4 КБ ложится под заглавным именем в папку приложения, временного файла не
  остаётся; годная копия не запрашивается снова; чтение кусками не по порядку — байт в байт, чтение за
  конец обрезается, после конца — пусто;
- кусок не по порядку, повтор и кусок за конец файла не принимаются;
- недоставленный файл не ложится, брошенная доставка не трогает прежнюю копию;
- файл на карте новее или другой длины — запрашивается снова, старее — нет;
- нет места — отказ; место прежней копии того же имени идёт в счёт;
- плохие имена приложения и файла (длинные, с путём, WRITING.TMP, нулевая длина) — отказ;
- три файла двух приложений читаются вперемешку (один открытый FILE* переключается), после closeAll —
  отказ; дескрипторов kMaxOpen, закрытый — снова даётся;
- (0.1.166) файлы на карте Evo по запросу у программы Z80 (FakeZ80 отвечает через AppDataRequest, как
  задача обмена прошивки): 400 случайных чтений байт в байт кусками DMA и кадрами, запросы — целые
  секторы до 32 КБ внутри файла, короткие подряд — из блока, отказ Z80 — kError, чужие, повторные и
  запоздавшие куски — мимо; учёт: файл на флешке читается раньше учёта (0.1.175), forgetRemote,
  вытеснение чужих, плохие имена;
- (0.1.175) поиск на флешке: папка приложения первая, потом все вложенные папки корня по алфавиту на
  любой глубине, имя без учёта регистра; первая папка с файлом — «домашняя», файлы из разных папок не
  смешиваются; ненайденное помнится до closeAll; том 0 (флешки нет) — не ищется; папка с именем файла
  — не файл; в журнал — одна строка на файл.
Без Visual Studio Build Tools тест пропускается.
"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_weather_parse import ROOT, build_exe

BUILD = ROOT / '.test-build' / 'app_data_files_host'
SCENARIOS = [
    'delivered_file_reads_back', 'put_out_of_order_is_refused', 'incomplete_delivery_leaves_nothing',
    'newer_card_file_is_sent_again', 'room_counts_the_old_copy', 'bad_names_are_refused',
    'interleaved_handles_read_their_files', 'handles_run_out',
    'remote_reads_match_the_card', 'remote_registry', 'remote_request_pieces', 'drive_search',
]


class AppDataFilesHostTest(unittest.TestCase):
    def test_files(self) -> None:
        exe = build_exe(BUILD, 'app_data_files_host.exe', [ROOT / 'include'],
                        [(ROOT / 'src' / 'app_data_files.cpp', 'files.obj'),
                         (ROOT / 'src' / 'resource_disk_cache.cpp', 'cache.obj'),
                         (ROOT / 'tests' / 'app_data_files_host.cpp', 'host.obj')])
        if exe is None:
            self.skipTest('нет MSVC (vcvars64.bat): стенд файлов приложений пропущен')
        folder = Path(tempfile.mkdtemp(prefix='zifi_appdata_'))
        root = folder / 'SYSTEM' / 'CACHE'           # как на плате: её делает usb_drive при монтировании
        root.mkdir(parents=True)                     # папку приложения begin() создаёт сам
        try:
            result = subprocess.run([str(exe), str(root)], capture_output=True, text=True, timeout=60)
        finally:
            shutil.rmtree(folder, ignore_errors=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.split(), [word for name in SCENARIOS for word in ('ok', name)])


if __name__ == '__main__':
    unittest.main()
