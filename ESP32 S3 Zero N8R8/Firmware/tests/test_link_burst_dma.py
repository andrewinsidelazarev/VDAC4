"""Запись в окно ядра по LINK длинными транзакциями DMA (0.1.176, ABI приложений 1.22, 27.11.2026).

Запрос сессии Quake 2: link_burst шлёт слова транзакциями по 16 через буфер контроллера W0…W15, и на
плате Q2 так получил ~1,75 слова/мкс при LINK 40 МГц (8,3 мс на 14 800 слов кадра), а линия даёт ~5.
link_burst_dma шлёт транзакции до 256 слов через GDMA (в 0.1.176 — 512), буферы — два по 1 КБ во внутреннем ОЗУ
(пока DMA шлёт один кусок, процессор копирует следующий), флаг ZM_LINK_NOWAIT — не ждать конца
последней транзакции.

Что проверяется:
- стенд MSVC (tests/link_spi3_host.cpp): src/link_spi3.cpp целиком с моделью SPI3 и GDMA
  (tests/stubs_link_spi3) — слова на линии, рамки CS, ожидание «хвоста» любым следующим обменом,
  сроки и ошибка хвоста, память и канал;
- по исходникам: поля ABI 1.22 в конце таблицы ядра, их заполнение, проверка аргументов, буферы
  берутся по просьбе приложения и отдаются при выгрузке, всё — под замком LINK, строка консоли «tang».
Плату стенд не заменяет: регистры GDMA и SPI3 настоящего ESP32-S3 он не видит.
"""
import re
import subprocess
import unittest

from test_weather_parse import ROOT, build_exe

BUILD = ROOT / '.test-build' / 'link_spi3_host'


def text(*parts: str) -> str:
    return ROOT.joinpath(*parts).read_text(encoding='utf-8').replace('\r\n', '\n')


def function_body(source: str, head: str) -> str:
    start = source.index(head)
    return source[start:source.index('\n}\n', start)]


class LinkBurstDmaTest(unittest.TestCase):
    def test_host(self) -> None:
        exe = build_exe(BUILD, 'link_spi3_host.exe', [ROOT / 'tests' / 'stubs_link_spi3', ROOT / 'include'],
                        [(ROOT / 'tests' / 'link_spi3_host.cpp', 'host.obj')])
        if exe is None:
            self.skipTest('нет MSVC (vcvars64.bat)')
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), 'OK')

    def test_abi(self) -> None:
        sdk = text('modules', 'sdk', 'include', 'zm_app.h')
        self.assertRegex(sdk, r'#define ZM_APP_ABI_MINOR (2[2-9]|[3-9]\d)\b')
        self.assertRegex(sdk, r'#define ZM_LINK_DMA_WORDS 256u\b')
        self.assertRegex(sdk, r'#define ZM_LINK_NOWAIT 0x01u\b')
        core = sdk[sdk.index('struct zm_core {'):]
        core = core[:core.index('\n};')]
        dma = 'int (*link_burst_dma)(uint32_t address, const uint32_t* words, uint32_t count, uint32_t flags);'
        wait = 'int (*link_burst_wait)(void);'
        checked = 'int (*link_read_checked)(uint32_t address, uint32_t* words, uint32_t count);'
        # новые поля — после полей 1.21, по порядку: у старых приложений прежние смещения
        self.assertLess(core.index(checked), core.index(dma))
        self.assertLess(core.index(dma), core.index(wait))
        # размер транзакции прошивки и ABI — одно число
        spi = text('include', 'zifi', 'link_spi3.hpp')
        self.assertIn('constexpr size_t kDmaWords = 256;', spi)

    def test_module_app(self) -> None:
        modules = text('src', 'apps', 'module_app.cpp')
        self.assertIn('core_.link_burst_dma = coreLinkBurstDma;', modules)
        self.assertIn('core_.link_burst_wait = coreLinkBurstWait;', modules)
        body = function_body(modules, 'int ModuleApp::coreLinkBurstDma(')
        self.assertIn('(address & 3) != 0', body)
        self.assertIn('(flags & ~ZM_LINK_NOWAIT) != 0', body, 'неизвестные флаги — отказ')
        self.assertIn('return ZM_NO_MEMORY;', body)
        self.assertIn('kLinkDmaKeep = 12 * 1024', body, 'запас внутреннего ОЗУ — как у code_ram')
        self.assertIn('(flags & ZM_LINK_NOWAIT) == 0', body)
        stop = function_body(modules, 'void ModuleApp::stopApp()')
        # буферы — назад, когда задачи приложения уже сняты (иначе DMA мог бы ещё идти из них)
        self.assertLess(stop.index('vTaskDelete'), stop.index('tang::releaseBurstDma();'))

    def test_locking(self) -> None:
        link = text('src', 'tang_link.cpp')
        for head in ('bool writeBurstDma(uint32_t address', 'bool waitBurstDma(', 'void releaseBurstDma(',
                     'bool prepareBurstDma('):
            body = function_body(link, head)
            self.assertIn('takeLock(', body, head)
            self.assertIn('giveLock()', body, head)
        spi = text('src', 'link_spi3.cpp')
        # каждый обмен модуля сначала дожидается хвоста DMA
        for head in ('bool setClock(uint32_t hz)', 'bool transfer(', 'bool writeRepeat(', 'bool writeBurst(uint8_t'):
            self.assertIn('finishPending()', function_body(spi, head), head)
        body = function_body(spi, 'bool writeBurstDma(')
        # порядок конвейера: копирование куска — до ожидания прошлой транзакции
        self.assertLess(body.index('__builtin_bswap32'), body.index('finishPending()'))
        self.assertLess(body.index('gdma_reset'), body.index('spi_hal_hw_prepare_tx'))
        self.assertLess(body.index('spi_hal_hw_prepare_tx'), body.index('gdma_start'))
        self.assertLess(body.index('gdma_start'), body.index('spi_ll_user_start'))
        finish = function_body(spi, 'bool finishPending()')
        self.assertIn('spi_ll_dma_tx_enable(hw, false);', finish, 'после DMA — снова W0…W15')
        cmake = text('main', 'CMakeLists.txt')
        self.assertRegex(cmake, r'\n\s+esp_driver_dma\b')

    def test_console(self) -> None:
        console = text('src', 'net_console.cpp')
        helper = function_body(console, 'bool sendLinkDma(int client)')
        self.assertIn('link_spi3::dmaStats()', helper)
        self.assertIn('tang LINK DMA: calls %u, transactions %u, words %llu, starved %u, failed %u\\n', helper)
        tang = function_body(console, 'bool runTang(int client, const char* arguments)')
        self.assertEqual(len(re.findall(r'sendCheckedReads\(client\) && sendLinkDma\(client\)', tang)), 2)


if __name__ == '__main__':
    unittest.main()
