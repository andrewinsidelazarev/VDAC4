"""Проверенное чтение регистров ядра по LINK (0.1.168, ABI приложений 1.21, 24.11.2026).

Чтение по LINK ничем не защищено: нет контрольной суммы, а ядра отдают регистр из своего домена тактов
«как есть», без синхронизации (fb_link.sv: «счётчик, пойманный посреди смены, ESP прочитает повторно»).
24.11.2026 на плате Quake 2 прочитал FIFO_FREE = 0x858 при пределе очереди 0x800. Пользователь выбрал
защиту в прошивке ESP повторным чтением («Да, способ 1»).

Что проверяется:
- стенд MSVC (tests/link_vote_host.cpp): логика include/zifi/link_vote.hpp — два совпавших чтения или
  медиана трёх, число обменов, отказ обмена, 16 слов, свойства медианы;
- по исходникам: tang::readChecked держит один замок LINK на все чтения и читает прямо через SPI3;
  приложению функция дана полем link_read_checked в конце таблицы ядра (ABI 1.21).
"""
import subprocess
import unittest

from test_weather_parse import ROOT, build_exe

BUILD = ROOT / '.test-build' / 'link_vote_host'


def text(*parts: str) -> str:
    return ROOT.joinpath(*parts).read_text(encoding='utf-8').replace('\r\n', '\n')


def function_body(source: str, head: str) -> str:
    start = source.index(head)
    return source[start:source.index('\n}\n', start)]


class LinkVoteTest(unittest.TestCase):
    def test_host(self) -> None:
        exe = build_exe(BUILD, 'link_vote_host.exe', [ROOT / 'include'],
                        [(ROOT / 'tests' / 'link_vote_host.cpp', 'host.obj')])
        if exe is None:
            self.skipTest('нет MSVC (vcvars64.bat)')
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), 'OK')

    def test_firmware_reads_under_one_lock(self) -> None:
        body = function_body(text('src', 'tang_link.cpp'), 'bool readChecked(uint32_t address,')
        # замок берётся один раз на все чтения, отдаётся после них
        self.assertEqual(body.count('takeLock('), 1)
        self.assertEqual(body.count('giveLock()'), 1)
        self.assertLess(body.index('takeLock('), body.index('link_vote::readVoted('))
        self.assertLess(body.index('link_vote::readVoted('), body.index('giveLock()'))
        # чтения — прямо через SPI3, а не через lockedRead (тот брал бы замок на каждое чтение)
        self.assertIn('link_spi3::transfer(kCmdRead, address, out, count, true, g_dummy)', body)
        self.assertNotIn('lockedRead', body)
        self.assertIn('if (outcome == link_vote::Outcome::kFailed)', body)

    def test_counter_in_ram_only(self) -> None:
        # 0.1.169 (пользователь: «Счётчик не будет перетирать флеш-память? Он в RAM?» → «Добавь счётчик»):
        # числа — atomic в ОЗУ, растут после чтения; в журнал (он во флеше) readChecked не пишет
        source = text('src', 'tang_link.cpp')
        body = function_body(source, 'bool readChecked(uint32_t address,')
        for name in ('g_checkedReads', 'g_checkedThird', 'g_checkedOutliers'):
            self.assertIn(f'std::atomic<uint32_t> {name}{{0}};', source)
            self.assertIn(f'++{name};', body)
        self.assertLess(body.index('giveLock()'), body.index('++g_checkedReads;'))
        self.assertIn('words, count, &outlier);', body)
        for forbidden in ('bringupLog', 'ESP_LOG', 'diagnosticLog', 'nvs_', 'fopen'):
            self.assertNotIn(forbidden, body)
        stats = function_body(source, 'CheckedReadStats checkedReadStats()')
        self.assertNotIn('bringupLog', stats)

    def test_console_prints_counter(self) -> None:
        console = text('src', 'net_console.cpp')
        helper = function_body(console, 'bool sendCheckedReads(int client)')
        self.assertIn('tang::checkedReadStats()', helper)
        self.assertIn('tang checked reads %u, third read %u, outliers', helper)
        # строка — в обеих ветках сводки «tang»: ядро TS-Config и ядро из кэша BL616 (Q2 — оттуда)
        tang = function_body(console, 'bool runTang(int client, const char* arguments)')
        self.assertEqual(tang.count('sendCheckedReads(client)'), 2)

    def test_app_function(self) -> None:
        sdk = text('modules', 'sdk', 'include', 'zm_app.h')
        self.assertRegex(sdk, r'#define ZM_APP_ABI_MINOR (2[1-9]|[3-9]\d)\b')
        core = sdk[sdk.index('struct zm_core {'):]
        core = core[:core.index('\n};')]
        field = 'int (*link_read_checked)(uint32_t address, uint32_t* words, uint32_t count);'
        self.assertIn(field, core)
        # новое поле — после полей 1.20: старые приложения видят прежние смещения
        self.assertLess(core.index('void (*data_close)(void* file);'), core.index(field))
        modules = text('src', 'apps', 'module_app.cpp')
        self.assertIn('core_.link_read_checked = coreLinkReadChecked;', modules)
        body = function_body(modules, 'int ModuleApp::coreLinkReadChecked(')
        self.assertIn('count <= 16', body)
        self.assertIn('(address & 3) == 0', body)
        self.assertIn('tang::readChecked(address, words, count)', body)


if __name__ == '__main__':
    unittest.main()
