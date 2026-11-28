"""ft_open и core_load ждут поднятия LINK после перезапуска ESP (0.1.181–0.1.182, 28.11.2026).

После сброса Evo программы SPG (BOING, DEMO3D, WALL-E) давали чёрный экран или красный бордюр. Журнал
ESP: «demo3d: FT812 is not available (-3)» через 4,5 с от старта ESP, «roomtest: cannot open FT812»
через 7,5 с, а «LINK step: board is powered, starting SPI3» — через 10,3 с (kStartDelayMs = 10 с) и
«LINK ready» — через ~14 с. ft_open и core_load при неготовом LINK сразу отказывали, хотя DEMO3D и BOING
зовут ft_open(15000).

Что проверяется (по исходникам; сама прошивка на ПК не собирается):
- tang::waitReady ждёт, только пока LINK поднимается (tang::starting), и выходит сразу, если канал
  готов, Tang нет или окно запуска прошло;
- ft_open и core_load перед отказом ждут его, и окно ожидания покрывает весь запуск LINK;
- срок приложения у ft_open по-прежнему уходит на смену ядра Tang.
"""
import re
import unittest

from test_weather_parse import ROOT
from test_wifi_power_save import function_body


def text(*parts: str) -> str:
    return ROOT.joinpath(*parts).read_text(encoding='utf-8').replace('\r\n', '\n')


class LinkStartWaitTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.link = text('src', 'tang_link.cpp')
        cls.modules = text('src', 'apps', 'module_app.cpp')

    def test_wait_ready(self) -> None:
        body = function_body(self.link, 'bool waitReady(uint32_t maxWaitMs)')
        # 0.1.182: ждать, пока канал поднимается или готов, но задача ещё не кончила запуск
        self.assertIn('!(ready() && g_settled.load()) && (starting() || ready()) && waited < maxWaitMs', body)
        self.assertIn('return ready();', body)

    def test_settled_after_marker(self) -> None:
        # флажок конца запуска — сразу за меткой SCRATCH: раньше смену ядра задача LINK не увидит
        # (на плате 0.1.181 метка легла в ядро FT812, и ft_open ждал ядро 02 до срока)
        marker = 'lockedWrite(tang::kRegScratch, &marker, 1);' + chr(10) + '  g_settled = true;'
        self.assertIn(marker, self.link)
        self.assertEqual(self.link.count('g_settled = true;'), 1)
        self.assertIn('std::atomic<bool> g_settled{false};', self.link)
        header = text('include', 'zifi', 'tang_link.hpp')
        self.assertIn('bool waitReady(uint32_t maxWaitMs);', header)

    def test_wait_covers_link_start(self) -> None:
        delay = int(re.search(r'constexpr uint32_t kStartDelayMs = (\d+);', self.link).group(1))
        window = int(re.search(r'constexpr uint32_t kStartWindowMs = (\d+);', self.link).group(1))
        wait = int(re.search(r'constexpr uint32_t kLinkStartWaitMs = (\d+);', self.modules).group(1))
        # приложение стартует не раньше старта ESP: ожидание до конца окна запуска LINK его покрывает
        self.assertGreaterEqual(wait, delay + window)

    def test_ft_open_waits(self) -> None:
        body = function_body(self.modules, 'int ModuleApp::coreFtOpen(uint32_t timeoutMs)')
        self.assertIn('if (!tang::waitReady(kLinkStartWaitMs)) {', body)
        self.assertNotIn('if (!tang::ready()) {', body, 'прежний немедленный отказ')
        # ожидание LINK — до смены ядра, срок приложения — смене ядра целиком
        self.assertLess(body.index('tang::waitReady('), body.index('ft_host::switchToFtCore(timeoutMs)'))

    def test_core_load_waits(self) -> None:
        body = function_body(self.modules, 'int ModuleApp::coreCoreLoad(const char* name, uint32_t* version)')
        self.assertIn('!tang::waitReady(kLinkStartWaitMs)', body)
        self.assertNotIn('!tang::ready()', body)


if __name__ == '__main__':
    unittest.main()
