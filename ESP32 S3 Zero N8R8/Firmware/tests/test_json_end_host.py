"""Конец JSON ответа API GitHub (0.1.162, include/zifi/json_end.hpp).

Свежий ответ API GitHub на большой запрос (дерево папки после пуша) приходит без Content-Length, конец —
закрытие соединения, а NetClient на TLS его не замечал: VDAC4 Online Update ждал 30 с и писал
«GitHub tree: body timeout» (22.11.2026). Теперь NetWcFetcher кончает чтение ответа api.github.com на
последней скобке JSON. Стенд MSVC проверяет JsonEnd на кусках любой длины, проверки по исходникам —
что так делает загрузчик и только для API.
"""
import subprocess
import unittest

from test_weather_parse import ROOT, build_exe

BUILD = ROOT / '.test-build' / 'json_end_host'


def text(*parts: str) -> str:
    return ROOT.joinpath(*parts).read_text(encoding='utf-8').replace('\r\n', '\n')


class JsonEndTest(unittest.TestCase):
    def test_host(self) -> None:
        exe = build_exe(BUILD, 'json_end_host.exe', [ROOT / 'include'],
                        [(ROOT / 'tests' / 'json_end_host.cpp', 'host.obj')])
        if exe is None:
            self.skipTest('нет MSVC (vcvars64.bat)')
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), 'OK')

    def test_fetcher_stops_at_json_end_for_api_only(self) -> None:
        src = text('src', 'wc_update_service.cpp')
        start = src.index('bool NetWcFetcher::get(')
        body = src[start:src.index('\n}\n', start)]
        self.assertIn('const bool api = strcmp(host, "api.github.com") == 0;', body)
        self.assertIn('jsonEnd.feed(buffer + length - received, received, used)', body)
        self.assertLess(body.index('jsonEnd.feed'), body.index('if (received == 0 && !eof)'))


if __name__ == '__main__':
    unittest.main()
