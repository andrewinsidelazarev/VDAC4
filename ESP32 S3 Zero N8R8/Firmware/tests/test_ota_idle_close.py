"""Свободный порт обновления закрывается сам (0.1.161).

update_vdac4 открывает порт 8267 (UPDATE_START) и закрывает его по клавише. Сброс Evo или другая
программа оставляли порт открытым, и до перезапуска ESP сеть отвечала «ota active» (ZiFi на плате в
Украине, 22.11.2026: «get:ota active»). Теперь сетевая команда другой программы закрывает свободный
порт (ПК не подключён); подключённый ПК — отказ, как прежде. Проверки — по исходникам.
"""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def text(*parts: str) -> str:
    return ROOT.joinpath(*parts).read_text(encoding='utf-8').replace('\r\n', '\n')


class OtaIdleCloseTest(unittest.TestCase):
    def test_refusals_go_through_idle_close(self) -> None:
        main = text('src', 'main.cpp')
        self.assertNotRegex(main, r'if \(ota_\.running\(\)\) \{\n\s+setNetworkError',
                            'отказ «ota active» — только через otaHoldsNetwork')
        self.assertEqual(len(re.findall(r'if \(otaHoldsNetwork\("[^"]+"\)\) \{\n\s+setNetworkError', main)), 9)

    def test_idle_port_closed_busy_port_kept(self) -> None:
        main = text('src', 'main.cpp')
        start = main.index('bool Application::otaHoldsNetwork(const char* who) {')
        body = main[start:main.index('\n}\n', start)]
        self.assertLess(body.index('ota_.clientActive()'), body.index('ota_.stop()'),
                        'подключённый ПК — порт не закрывать')
        self.assertIn('return true;', body)
        self.assertIn('bool clientActive() const { return clientActive_; }', text('include', 'zifi', 'ota_server.hpp'))

    def test_apps_use_the_same_rule(self) -> None:
        self.assertIn('virtual bool otaBusy() = 0;', text('include', 'zifi', 'app.hpp'))
        self.assertIn('bool otaBusy() override { return otaHoldsNetwork("app"); }', text('src', 'main.cpp'))
        for name in ('info_apps.cpp', 'vdac4_update_app.cpp', 'wc_update_app.cpp'):
            src = text('src', 'apps', name)
            self.assertIn('host_.otaBusy()', src, name)
            self.assertNotIn('otaRunning', src, name)


if __name__ == '__main__':
    unittest.main()
