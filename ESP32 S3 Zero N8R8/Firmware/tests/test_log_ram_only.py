"""Журнал прошивки — в ОЗУ, во flash — только запись о падении (0.1.183, 28.11.2026).

До 0.1.183 каждая строка журнала не позже чем через секунду уходила в кольцевой файл LittleFS во flash
ESP. LittleFS на каждую запись переписывает блок flash в 4 КБ, а кэш обоих ядер ESP на время записи
стоит — десятки и сотни миллисекунд. WALL-E пишет строку замера раз в 2 с: около 1800 стираний блока
в час и рывок каждые 2 с. Пользователь: «не нужно писать во флеш, а то он умрёт от перетирания»,
«во флеш пишем только при падениях программ — чтобы после сброса можно было прочитать, из-за чего
они упали».

Что проверяется:
- стенд MSVC (tests/diagnostic_log_host.cpp): src/diagnostic_log.cpp целиком с моделью LittleFS,
  которая считает операции с flash, — обычная работа flash не пишет, запуск после падения пишет один
  файл с хвостом прошлого запуска из RTC-памяти, обычный перезапуск его только читает;
- по исходникам: переноса журнала во flash больше нет нигде (сетевой цикл, OTA, шаги LINK, сторож),
  запись о падении — после crashRecordReport, сторож обмена с Z80 перед перезапуском ставит отметку.
"""
import re
import subprocess
import unittest

from test_weather_parse import ROOT, build_exe
from test_wifi_power_save import function_body

BUILD = ROOT / '.test-build' / 'diagnostic_log_host'


def text(*parts: str) -> str:
    return ROOT.joinpath(*parts).read_text(encoding='utf-8').replace('\r\n', '\n')


class LogRamOnlyTest(unittest.TestCase):
    def test_host(self) -> None:
        exe = build_exe(BUILD, 'diagnostic_log_host.exe', [ROOT / 'tests' / 'stubs_diagnostic_log', ROOT / 'include'],
                        [(ROOT / 'tests' / 'diagnostic_log_host.cpp', 'host.obj')])
        if exe is None:
            self.skipTest('нет MSVC (vcvars64.bat)')
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), 'OK')

    def test_no_flush_anywhere(self) -> None:
        for folder in ('src', 'include', 'main'):
            for path in ROOT.joinpath(folder).rglob('*'):
                if path.suffix in ('.cpp', '.hpp', '.h', '.c', '.txt'):
                    source = path.read_text(encoding='utf-8', errors='replace')
                    for name in ('diagnosticLogFlush', 'blocksFlashWrites', 'appsBlockFlashWrites'):
                        self.assertNotIn(name, source, f'{name} в {path.name}')

    def test_flash_only_in_crash_file(self) -> None:
        log = text('src', 'diagnostic_log.cpp')
        # flash — только в saveCrash, loadCrash и в монтировании раздела для них
        for head in ('void enqueueEvent(', 'bool diagnosticLogBegin()', 'bool diagnosticLogSnapshot(',
                     'void diagnosticLogHangRestart()', 'void tailPut('):
            body = function_body(log, head)
            self.assertNotIn('LittleFS', body, head)
            self.assertNotIn('flashFs', body, head)
        crash = function_body(log, 'void diagnosticLogCrashFile()')
        self.assertIn('flashFsMount(false)', crash, 'раздел с кэшем модулей не форматировать')
        self.assertLess(crash.index('flashFsMount(false)'), crash.index('saveCrash(reason)'))
        self.assertLess(crash.index('loadCrash()'), crash.index('flashFsUnmount()'))
        self.assertEqual(len(re.findall(r'LittleFS\.open\(kCrashPath, "w"\)', log)), 1)
        self.assertIn('RTC_NOINIT_ATTR Tail g_tail;', log)
        self.assertIn('constexpr size_t kTailCount = 32;', log)
        self.assertIn('constexpr size_t kRingCount = 256;', log)

    def test_boot_order(self) -> None:
        main = text('src', 'main.cpp')
        report = main.index('  crashRecordReport();\n')
        self.assertLess(main.index('diagnosticLogBegin()'), report)
        # запись о падении — когда причина паники уже в журнале
        self.assertEqual(main.index('diagnosticLogCrashFile();', report) > report, True)
        self.assertEqual(main.count('diagnosticLogCrashFile();'), 1)
        loop = function_body(main, 'void Application::networkTaskLoop()')
        # строки событий до цикла — можно; в самом цикле журнал больше не трогается (был перенос раз в 1 с)
        self.assertNotIn('diagnosticLog', loop[loop.index('for (;;)'):])
        self.assertNotIn('lastDiagnosticFlushMs', main)

    def test_hang_restart_marked(self) -> None:
        watchdog = text('src', 'link_watchdog.cpp')
        self.assertLess(watchdog.index('diagnosticLogHangRestart();'), watchdog.index('esp_restart();'))
        link = text('src', 'tang_link.cpp')
        self.assertNotIn('diagnosticLog', function_body(link, 'void step(const char* text)'))


if __name__ == '__main__':
    unittest.main()
