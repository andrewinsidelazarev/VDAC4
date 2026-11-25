"""Строка UNC3 окна SMB-плагина — шара USB (флешка в USB-C ESP32), плагин 0.5.22 + прошивка ESP 0.1.172.

Пользователь (25.11.2026, про окно плагина): «И тут писать UNC3 (если он есть)». ESP с 0.1.172 шестым
байтом ответа SMB_START сообщает, открыта ли шара USB; плагин показывает её строкой UNC3 или прочерком.
Чтобы строка поместилась, zifi.ini теперь читается в VfsBuf (страница #8000 была полна).

Собрать: SMB Server/build.bat vdac4; запускать Python с пакетом z80 (E:/zx/esp/pio-venv).
"""
from pathlib import Path
import re
import unittest

from test_context_z80 import Commander, SYMBOLS

SRC = Path(__file__).resolve().parents[1] / 'src'


def field(machine, symbol, width):
    start = SYMBOLS[symbol]
    return bytes(machine.cpu.memory[start:start + width]).decode('ascii')


class Unc3Test(unittest.TestCase):
    def setUp(self):
        self.m = Commander(1, {}, {})

    def build(self, present):
        self.m.cpu.memory[SYMBOLS['LinkUsb']] = 1 if present else 0
        self.m.call('Ui_BuildUsb')
        return field(self.m, 'UiUnc3Field', 32)

    def test_share_shown_when_present(self):
        self.assertEqual(self.build(True), '\\\\ZX-Evo\\USB  (USB flash ESP32) ')

    def test_dash_when_absent(self):
        self.assertEqual(self.build(False), '-  (no USB flash)'.ljust(32))

    def test_rebuild_clears_the_longer_text(self):
        # страница плагина живёт между запусками: прочерк после шары не оставляет хвоста
        self.build(True)
        self.assertEqual(self.build(False), '-  (no USB flash)'.ljust(32))

    def test_reply_byte_read_only_when_sent(self):
        source = (SRC / 'smb_server.asm').read_text(encoding='utf-8')
        part = source[source.index('.tang_done:'):source.index('.usb_done:')]
        # прошивка до 0.1.172 отвечает пятью байтами: шестой берётся, только если он пришёл
        self.assertLess(part.index('ld (LinkUsb),a'), part.index('cp 6'))
        self.assertLess(part.index('cp 6'), part.index('ld a,(ProtoBuf+5)'))
        self.assertIn('call Ui_BuildUsb', source)

    def test_window_fits_three_shares(self):
        ui = (SRC / 'ui.asm').read_text(encoding='utf-8')
        self.assertIn('db 52,20', ui)
        text = ui[ui.index('UiText:'):ui.index('db #0E,"ESC - stop server')]
        rows = text.count('#0D') + 1                 # последняя строка — подсказка Esc
        self.assertLessEqual(rows, 20 - 2, 'строки текста — внутри рамки окна')
        self.assertIn('db "UNC3   : "', ui)

    def test_ini_buffer_is_vfs_buffer(self):
        # zifi.ini — в VfsBuf: тот нужен только файловым запросам, после Link_Start
        self.assertEqual(SYMBOLS['IniBuffer'], SYMBOLS['VfsBuf'])
        self.assertLessEqual(SYMBOLS['mainEnd'], 0xC000)
        main = (SRC / 'main.asm').read_text(encoding='utf-8')
        self.assertLess(main.index('DEFINE CONFIG_INI_BUFFER_AT VfsBuf'), main.index('INCLUDE "config.asm"'))
        loop = (SRC / 'smb_server.asm').read_text(encoding='utf-8')
        self.assertLess(loop.index('call Config_Load'), loop.index('call Link_Start'))
        self.assertLess(loop.index('call Link_Start'), loop.index('call Vfs_Serve'))


if __name__ == '__main__':
    unittest.main()
