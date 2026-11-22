"""Долгие чтение и запись онлайн-обновления отдают ядро (0.1.163).

На плате 22.11.2026 сверка слота 3 flash Tang (мегабайт кусками по 64 байта через LINK, активное
ожидание SPI) держала ядро 0 больше 5 с: сторож задач перезапустил ESP («Task watchdog … task
vdac4-update»), плагин написал «list not refreshed». Проверки — по исходникам: передышка breathe()
(тик сна раз в 100 мс) стоит в каждом долгом цикле платы.
"""
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def text(*parts: str) -> str:
    return ROOT.joinpath(*parts).read_text(encoding='utf-8').replace('\r\n', '\n')


class BoardBreatheTest(unittest.TestCase):
    def test_breathe_sleeps_a_tick(self) -> None:
        src = text('src', 'vdac4_board_esp.cpp')
        start = src.index('void breathe() {')
        body = src[start:src.index('\n}\n', start)]
        self.assertIn('vTaskDelay(1);', body)
        self.assertIn('pdMS_TO_TICKS(100)', body)

    def test_every_long_loop_breathes(self) -> None:
        src = text('src', 'vdac4_board_esp.cpp')
        for call in ('esp_partition_read(running, done, chunk, part)',
                     'return tang_flash::read(address, data, length);',
                     'tang_flash::eraseBlock(base + block * tang_flash::kBlockBytes)',
                     'tang_flash::program(base + done, image + done, part)',
                     'if (!tang_flash::read(base + done, back, part))',
                     'fread(data + done, 1, want, f)',
                     'fwrite(data + done, 1, want, f)'):
            at = src.index(call)
            line_start = src.rindex('\n', 0, at)
            previous = src[src.rindex('\n', 0, line_start) + 1:line_start].strip()
            self.assertEqual(previous, 'breathe();', call)


if __name__ == '__main__':
    unittest.main()
