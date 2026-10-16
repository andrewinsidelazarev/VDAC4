"""Машинные тесты плагина VDAC4 Video Settings (Video Settings/build/vdac4/VDAC4VID.WMF).

Плагин своего окна не рисует: показывает экранное меню Tang поверх картинки HDMI и передаёт
ему клавиши Evo через регистр UI_KEY ядра Tang (1.3+), командой TANG_WRITE прошивки ESP.
Он исполняется в эмуляторе Z80 на стенде канала SPI (test_spi_link_z80.SpiHarness); за
каналом — модель прошивки ESP с TANG_READ/TANG_WRITE и регистрами ядра Tang.

Проверяется: что уходит в UI_KEY на каждую клавишу и на Esc, повтор «показать», окно WC
не открывается в обычной работе; ошибки (старое ядро, нет LINK, нет ESP, связь пропала) —
окно с текстом и выход по Esc без лишних записей.
Нужна сборка: "Video Settings\\build.bat".
"""
import struct
import unittest

import test_ftp_plugin_z80 as base
import test_spi_link_z80 as spi

ROOT = base.ROOT
VIDEO_WMF = ROOT / "Video Settings" / "build" / "vdac4" / "VDAC4VID.WMF"
VIDEO_SYM = ROOT / "Video Settings" / "build" / "vdac4" / "VDAC4VID.sym"

FN_UP, FN_DOWN, FN_LEFT, FN_RIGHT = 0x11, 0x12, 0x13, 0x14
CMD_TANG_READ, CMD_TANG_WRITE = 0x45, 0x46
RESP_TANG_READ, RESP_TANG_WRITE = 0xC5, 0xC6
UI_KEY = 0x50
NEXT, PREV, INC, DEC, SHOW, HIDE = 1, 2, 3, 4, 5, 6
SETTING_REGS = {0x3C, 0x40, 0x44, UI_KEY} | {0x60 + 4 * i for i in range(8)}


class TangEsp:
    """Прошивка ESP: TANG_READ/TANG_WRITE над моделью регистров Tang (как main.cpp)."""

    def __init__(self, version: int = 0x01010103, link_ok: bool = True,
                 fail_after: int | None = None) -> None:
        self.regs = {0x00: 0x56444334, 0x04: version}
        self.link_ok = link_ok
        self.fail_after = fail_after         # после стольких записей LINK пропадает
        self.commands: list[int] = []
        self.keys: list[int] = []

    def __call__(self, command: int, payload: bytes) -> bytes:
        self.commands.append(command)
        if command == CMD_TANG_READ:
            address, count = payload[0] | payload[1] << 8, payload[2]
            if not self.link_ok:
                return base.frame(RESP_TANG_READ, b"\0")
            words = b"".join(struct.pack("<I", self.regs.get(address + 4 * i, 0))
                             for i in range(count))
            return base.frame(RESP_TANG_READ, b"\1" + words)
        if command == CMD_TANG_WRITE:
            address, count = payload[0] | payload[1] << 8, payload[2]
            if self.fail_after is not None and len(self.keys) >= self.fail_after:
                self.link_ok = False
            ok = self.link_ok and count == 1 and len(payload) == 7 and address in SETTING_REGS
            if ok and address == UI_KEY:
                self.keys.append(struct.unpack("<I", payload[3:7])[0])
            return base.frame(RESP_TANG_WRITE, bytes([1 if ok else 0]))
        return base.frame(0xEE, b"unsupported")


class VideoHarness(spi.SpiHarness):
    """Плагин целиком: клавиши — по номеру кадра, Esc — после esc_after опросов."""

    def __init__(self) -> None:
        super().__init__(VIDEO_WMF, VIDEO_SYM)
        self.keys: dict[int, int] = {}       # кадр -> клавиша

    def handle_api(self) -> None:
        m = self.machine
        if m.a in (FN_UP, FN_DOWN, FN_LEFT, FN_RIGHT):
            self.calls.append(m.a)
            self.flags(zero=self.keys.get(self.interrupts) != m.a)
            self.ret()
        else:
            super().handle_api()

    def text(self) -> str:
        return "\n".join(self.screen.get(line, bytearray(b" " * 80)).decode("latin-1").rstrip()
                         for line in range(1, 10))

    def start(self, esp: TangEsp, keys: dict[int, int] | None = None, esc: int = 40) -> None:
        self.app = esp
        self.keys = keys or {}
        self.esc_after = esc
        self.run(base.CODE, a=3)


class VideoPluginTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        spi.need(VIDEO_WMF, VIDEO_SYM)

    def test_header_names_the_plugin(self) -> None:
        data = VIDEO_WMF.read_bytes()
        self.assertEqual(data[165:197].rstrip(b" "), b"VDAC4 Video Settings v1.1")

    def test_keys_drive_the_tang_menu(self) -> None:
        h = VideoHarness()
        esp = TangEsp()
        h.start(esp, {10: FN_DOWN, 13: FN_UP, 16: FN_RIGHT, 19: FN_LEFT, 22: FN_RIGHT}, esc=30)
        self.assertEqual(esp.commands[0], CMD_TANG_READ, "сначала ID и версия ядра")
        self.assertEqual(esp.keys, [SHOW, NEXT, PREV, INC, DEC, INC, HIDE])
        self.assertEqual(h.prwow_windows, [], "в обычной работе окна нет")
        self.assertNotIn(base.FN_RRESB, h.calls)
        self.assertFalse(h.slave.selected)

    def test_menu_kept_shown(self) -> None:
        h = VideoHarness()
        esp = TangEsp()
        h.start(esp, esc=600)                # 12 с без нажатий
        self.assertEqual(esp.keys[0], SHOW)
        self.assertEqual(esp.keys[-1], HIDE)
        self.assertGreaterEqual(esp.keys.count(SHOW), 3, "«показать» — раз в 5 с")

    def fatal(self, esp: TangEsp, text: str, *, die: bool = False) -> VideoHarness:
        h = VideoHarness()
        if die:
            h.link.die()
        h.start(esp, {10: FN_RIGHT, 12: FN_DOWN}, esc=20)
        self.assertIn(text, h.text())
        self.assertEqual(len(h.prwow_windows), 1)
        self.assertEqual(h.calls.count(base.FN_RRESB), 1, "окно ошибки закрыто при выходе")
        window = h.prwow_windows[0]
        self.assertLessEqual(window[3] + window[5], 25, "окно помещается в экран 25 строк")
        return h

    def test_old_core(self) -> None:
        esp = TangEsp(version=0x01010102)
        self.fatal(esp, "ERROR: Tang core 1.2 is older than 1.3")
        self.assertNotIn(CMD_TANG_WRITE, esp.commands)

    def test_no_link(self) -> None:
        esp = TangEsp(link_ok=False)
        self.fatal(esp, "ERROR: ESP has no LINK to the Tang")
        self.assertNotIn(CMD_TANG_WRITE, esp.commands)

    def test_link_lost(self) -> None:
        esp = TangEsp(fail_after=1)          # «показать» прошло, следующее — нет
        self.fatal(esp, "ERROR: no answer from the Tang")
        self.assertEqual(esp.keys, [SHOW])

    def test_no_esp(self) -> None:
        esp = TangEsp()
        self.fatal(esp, "ERROR: no ESP on the SPI", die=True)
        self.assertEqual(esp.commands, [])


if __name__ == "__main__":
    unittest.main()
