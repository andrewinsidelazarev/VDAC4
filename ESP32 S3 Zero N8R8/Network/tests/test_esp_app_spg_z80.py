"""Машинные тесты запускателя приложений ESP (ESP App SPG/build/esp_app.bin, ИМЯ.SPG).

Код SPG исполняется в эмуляторе Z80 на «голой» машине: окно #8000 — код (страница 2),
окно #C000 — страница, которую выбирает порт PAGE3 (#13AF; ELF лежит со страницы #20).
За портами #57/#77 — модель общего буфера ESP (spi_link_model), за ней — «прошивка» с
командами модулей: MOD_QUERY, MOD_PUT, MOD_COMMIT, APP_START, APP_STOP.

Проверяется: ELF доходит до ESP байт в байт и ровно один раз; приложение запускается
по своему имени; монитор переключается на FT812 (VCONFIG #04) до APP_START — M_SEL держит
ядро FT812 в Tang, пока приложение его включает; по клавише приложение останавливается и
монитор возвращается (VCONFIG #00); если ELF уже в кэше ESP, он не передаётся; если ESP
не взяла ELF — красный бордюр, экран не трогается; приложение не запустилось — красный
бордюр и монитор обратно у TS-Conf.
Нужна сборка: "ESP App SPG\\build.bat" [ИМЯ.ELF] (по умолчанию — BOING.ELF демы
demo\\boing рабочего дерева); проверяется приложение последней сборки — его имя в build\\app_cfg.inc.
"""
import re
import struct
import unittest

import z80

import test_ftp_plugin_z80 as base
from test_spi_link_z80 import SpiLinkMixin, need

ROOT = base.ROOT
SPG_DIR = ROOT / "ESP App SPG" / "build"
BIN = SPG_DIR / "esp_app.bin"
SYM = SPG_DIR / "esp_app.sym"
CFG = SPG_DIR / "app_cfg.inc"


def app_name() -> str:
    """Имя ELF последней сборки запускателя (build.bat пишет его в app_cfg.inc)."""
    m = re.search(r'APP_NAME db "([^"]+)"', CFG.read_text(encoding="ascii")) if CFG.exists() else None
    return m.group(1) if m else "BOING.ELF"


ELF = SPG_DIR / app_name()

VCONFIG, PAGE3 = 0x00AF, 0x13AF
APP_PAGE0 = 0x20
CMD_MOD_QUERY, CMD_MOD_PUT, CMD_MOD_COMMIT, CMD_APP_START, CMD_APP_STOP = 0x69, 0x6A, 0x6B, 0x6C, 0x6D
frame = base.frame


class ModuleEsp:
    """Прошивка ESP: кэш модулей и приложение (как ModuleApp)."""

    def __init__(self, cached: bool = False, refuse: bool = False, start_fail: bool = False) -> None:
        self.cached = cached
        self.refuse = refuse
        self.start_fail = start_fail
        self.query = None
        self.data = bytearray()
        self.puts = 0
        self.committed = False
        self.started = None
        self.stopped = False
        self.commands: list[int] = []

    def __call__(self, command: int, payload: bytes) -> bytes:
        self.commands.append(command)
        if command == CMD_MOD_QUERY:
            flags, length, time = payload[0], *struct.unpack("<II", payload[1:9])
            name = payload[9:payload.index(0, 9)].decode()
            self.query = (flags, length, time, name)
            status = 2 if self.refuse else (0 if self.cached else 1)
            return frame(CMD_MOD_QUERY | 0x80, bytes([status]) + struct.pack("<I", 1 << 20))
        if command == CMD_MOD_PUT:
            offset = struct.unpack("<I", payload[:4])[0]
            assert offset == len(self.data), f"кусок с {offset}, ждали {len(self.data)}"
            self.data += payload[4:]
            self.puts += 1
            return frame(CMD_MOD_PUT | 0x80, b"\1" + struct.pack("<I", len(self.data)))
        if command == CMD_MOD_COMMIT:
            self.committed = True
            return frame(CMD_MOD_COMMIT | 0x80, b"\1")
        if command == CMD_APP_START:
            if self.start_fail:                     # итог int8 −2: приложение не поднялось
                return frame(CMD_APP_START | 0x80, b"\xfe" + payload + b"1.0\0")
            self.started = payload[:payload.index(0)].decode()
            return frame(CMD_APP_START | 0x80, b"\0" + payload + b"1.0\0")
        if command == CMD_APP_STOP:
            self.stopped = True
            return frame(CMD_APP_STOP | 0x80, b"\1")
        return frame(0xEE, b"unsupported")


class SpgMachine(SpiLinkMixin):
    """Запускатель SPG: код с #8000, страницы #20… — ELF, клавиатура и VCONFIG — модели."""

    PLUGIN_PAGE = None

    def __init__(self) -> None:
        text = SYM.read_text(encoding="utf-8", errors="replace")
        self.symbols = {name: int(value, 16) for name, value in
                        re.findall(r"^([\w.]+):\s+EQU\s+0x([0-9A-Fa-f]+)", text, re.M)}
        self.machine = z80.Z80Machine()
        self.memory = self.machine.memory
        self.machine.set_memory_block(0x8000, BIN.read_bytes())
        elf = ELF.read_bytes()
        self.pages = {APP_PAGE0 + i: elf[i * 16384:(i + 1) * 16384] for i in range((len(elf) + 16383) // 16384)}
        self.machine.set_input_callback(self.port_in)
        self.machine.set_output_callback(self.port_out)
        self.machine.set_breakpoint(self.symbols["Halt"])
        self.rx = bytearray()
        self.tx = bytearray()
        self.rx_polls = 0
        self.esp = None
        self.vconfig: list[int] = []
        self.vconfig_at: list[int] = []     # сколько команд ESP уже получила к записи VCONFIG
        self.border: list[int] = []
        self.key_polls = 0
        self.attach_link()

    def port_in(self, port: int) -> int:
        if port & 0xFF == 0xFE:
            # клавиша нажата, только когда монитор уже у FT812 и прошло немного опросов
            self.key_polls += 1
            pressed = bool(self.vconfig) and self.vconfig[-1] == 0x04 and self.key_polls > 50
            return 0xFF & ~(0x01 if pressed else 0)
        return super().port_in(port)

    def port_out(self, port: int, value: int) -> None:
        if port == PAGE3:
            data = self.pages.get(value, b"")
            self.memory[0xC000:0xC000 + 16384] = data + bytes(16384 - len(data))
            return
        if port == VCONFIG:
            self.vconfig.append(value)
            self.vconfig_at.append(len(self.app.commands))
            return
        if port & 0xFF == 0xFE:
            self.border.append(value & 7)
            return
        super().port_out(port, value)

    def run(self) -> None:
        m = self.machine
        m.pc = self.symbols["Start"]
        for _ in range(400_000):
            m.ticks_to_stop = 5_000_000
            event = m.run()
            if event & base.BREAKPOINT_HIT and m.pc == self.symbols["Halt"]:
                return
        raise AssertionError("запускатель не дошёл до конца")


class EspAppSpgTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        need(BIN, SYM, ELF)

    def launch(self, esp: ModuleEsp) -> SpgMachine:
        h = SpgMachine()
        h.app = esp
        h.run()
        return h

    def test_elf_goes_to_esp_and_app_runs_on_ft812(self) -> None:
        esp = ModuleEsp()
        h = self.launch(esp)
        elf = ELF.read_bytes()
        self.assertEqual(esp.query[3], ELF.name)
        self.assertEqual(esp.query[1], len(elf))
        self.assertEqual(bytes(esp.data), elf, "ELF дошёл до ESP не байт в байт")
        self.assertTrue(esp.committed)
        self.assertEqual(esp.started, ELF.name)
        self.assertTrue(esp.stopped, "по клавише приложение не остановлено")
        self.assertEqual(h.vconfig, [0x04, 0x00], "экран: FT812, затем обратно TS-Conf")
        start, stop = esp.commands.index(CMD_APP_START), esp.commands.index(CMD_APP_STOP)
        self.assertLess(start, stop)
        self.assertEqual(h.vconfig_at, [start, stop + 1],
                         "VCONFIG #04 — до APP_START, VCONFIG #00 — после APP_STOP")
        self.assertEqual(h.border[-1], 0, "в конце — чёрный бордюр, не красный")

    def test_cached_elf_is_not_sent_again(self) -> None:
        esp = ModuleEsp(cached=True)
        h = self.launch(esp)
        self.assertEqual(esp.puts, 0)
        self.assertFalse(esp.committed)
        self.assertEqual(esp.started, ELF.name)
        self.assertEqual(h.vconfig, [0x04, 0x00])

    def test_refused_elf_shows_red_border(self) -> None:
        esp = ModuleEsp(refuse=True)
        h = self.launch(esp)
        self.assertIsNone(esp.started)
        self.assertNotIn(0x04, h.vconfig, "без приложения экран у FT812 не отбирается у TS-Conf")
        self.assertEqual(h.border[-1], 2, "ошибка — красный бордюр")

    def test_app_start_failure_returns_monitor(self) -> None:
        esp = ModuleEsp(cached=True, start_fail=True)
        h = self.launch(esp)
        self.assertIsNone(esp.started)
        self.assertNotIn(CMD_APP_STOP, esp.commands)
        self.assertEqual(h.vconfig, [0x04, 0x00], "монитор — обратно TS-Conf")
        self.assertEqual(h.border[-1], 2, "ошибка — красный бордюр")


if __name__ == "__main__":
    unittest.main()
