"""Машинные тесты плагина VDAC4 Online Update 2 (Online Update/build/vdac4/VDAC4UPD.WMF).

Плагин — форк WC Update (устройство — Online Update/DESIGN.md): перезапуск ESP и READY, zifi.ini,
VUP_START [флаги], список событиями #67/#68 в таблице страницы 2, отметки (по умолчанию — по флагу 16),
Enter — VUP_APPLY с номерами отмеченных, после итога — VUP_SYNC, Esc — VUP_STOP. Этап 7: ESP поставила
свою прошивку и перезапускается — плагин заново открывает канал, ждёт READY, шлёт zifi.ini и VUP_START.

Плагин исполняется в эмуляторе Z80 на стенде канала SPI с настоящими окнами страниц (WindowHarness):
страница 1 — окно записи vfs.asm, страница 2 — таблица. Кадровый таймер WC двигает модель: раз в
несколько опросов канала. Клавиши — по кадрам. Строки окна снимаются с PRSRW.
Нужна сборка: "Online Update\\build.bat".
"""
import struct
import unittest

import test_ftp_plugin_z80 as base
import test_spi_link_z80 as spi
from spi_link_model import LinkCore

ROOT = base.ROOT
WMF = ROOT / "Online Update" / "build" / "vdac4" / "VDAC4UPD.WMF"
SYM = ROOT / "Online Update" / "build" / "vdac4" / "VDAC4UPD.sym"
frame = base.frame

CMD_WIFI_INI, CMD_PING, CMD_SYS_RESET = 0x03, 0x04, 0x0F
RESP_WIFI_INI, RESP_READY, RESP_ACK, RESP_ERROR = 0x83, 0xF0, 0xFE, 0xEE
VUP_START, VUP_APPLY, VUP_STOP, VUP_SYNC = 0x1A, 0x1B, 0x1C, 0x1D
EVT_STATE, EVT_ENTRY = 0x67, 0x68
VFS_STAT = 0x40
PH_GITHUB, PH_LOCAL, PH_CHECK, PH_READY, PH_APPLY, PH_SYNC, PH_RESTART = 1, 2, 3, 4, 5, 6, 7
ST_UNKNOWN, ST_SAME, ST_DIFF, ST_NEW, ST_UPDATED, ST_FAILED, ST_OLDER = 0, 1, 2, 3, 8, 9, 10
F_REMOTE, F_LOCAL, F_CAN, F_AUTO, F_REBOOT = 1, 2, 8, 16, 32
K_ESP, K_BL616, K_SLOT, K_TANG, K_EVO = 1, 2, 3, 4, 5

FN_PRIAT, FN_TURBOPL = 0x04, 0x0E
FN_SPACE, FN_UP, FN_DOWN, FN_ENTER = 0x10, 0x11, 0x12, 0x16
FN_PGUP, FN_PGDN, FN_HOME, FN_END, FN_KBSCN = 0x19, 0x1A, 0x1B, 0x1C, 0x2A
FN_GFILE, FN_GDIR = 0x3E, 0x3F
KEYS = (FN_SPACE, FN_UP, FN_DOWN, FN_ENTER, FN_PGUP, FN_PGDN, FN_HOME, FN_END)


def entry(index: int, row: dict) -> bytes:
    data = (bytes([index, row["status"], row["flags"], row["kind"]]) + row["cur"].encode() + b"\0" +
            row["avail"].encode() + b"\0" + row["name"].encode() + b"\0")
    assert len(data) <= 63, "событие ESP -> Z80 не длиннее 63 байт"
    return frame(EVT_ENTRY, data)


def state(phase: int, current: int, total: int, percent: int, text: str) -> bytes:
    data = bytes([phase]) + struct.pack("<HH", current, total) + bytes([percent]) + text.encode() + b"\0"
    assert len(data) <= 63
    return frame(EVT_STATE, data)


def rows_before() -> list[dict]:
    """Плата до обновления: ESP и слот 1 старее, BL616 опубликована старше, CORES — новый файл."""
    return [
        dict(status=ST_DIFF, flags=F_REMOTE | F_LOCAL | F_CAN | F_AUTO | F_REBOOT, kind=K_ESP,
             cur="0.1.123", avail="0.1.124", name="ESP32-S3 firmware"),
        dict(status=ST_OLDER, flags=F_REMOTE | F_LOCAL, kind=K_BL616, cur="42", avail="41", name="BL616 firmware"),
        dict(status=ST_SAME, flags=F_REMOTE | F_LOCAL | F_CAN, kind=K_SLOT, cur="1.46", avail="1.46",
             name="FPGA slot 0 TS-Config"),
        dict(status=ST_DIFF, flags=F_REMOTE | F_LOCAL | F_CAN | F_AUTO, kind=K_SLOT, cur="?", avail="1.45",
             name="FPGA slot 1 FT812"),
        dict(status=ST_NEW, flags=F_REMOTE | F_CAN, kind=K_TANG, cur="-", avail="886K", name="Tang SD: CORES/MD.BIN"),
        dict(status=ST_DIFF, flags=F_REMOTE | F_LOCAL | F_CAN | F_AUTO, kind=K_EVO, cur="17K", avail="17K",
             name="WC/VDAC4COR.WMF"),
    ]


class VupEsp:
    """Прошивка ESP с обновлятором платы: READY, Wi-Fi, VUP_* и события по DESIGN.md."""

    def __init__(self, rows: list[dict] | None = None, *, start_refused: bool = False) -> None:
        self.rows = rows if rows is not None else rows_before()
        self.start_refused = start_refused
        self.commands: list[int] = []
        self.resets = 0
        self.inis: list[bytes] = []
        self.starts: list[bytes] = []
        self.applies: list[list[int]] = []
        self.syncs = 0
        self.stops = 0
        self.restarted = False             # этап 7 был: следующая проверка — с новой прошивкой
        self.ready_sent = 0                # сколько раз сообщён итог проверки
        self.stat_path: bytes | None = None    # файловый запрос к карте Evo во время проверки
        self.vfs_replies: list[bytes] = []

    def ready(self, text: str = "check done") -> bytes:
        can = sum(1 for r in self.rows if r["flags"] & F_CAN)
        self.ready_sent += 1
        return state(PH_READY, can, len(self.rows), 100, text)

    def __call__(self, command: int, payload: bytes) -> bytes:
        self.commands.append(command)
        if command == CMD_SYS_RESET:
            self.resets += 1
            return b""
        if command == CMD_PING:
            return frame(RESP_READY)
        if command == CMD_WIFI_INI:
            self.inis.append(payload)
            return frame(RESP_ACK) + frame(RESP_WIFI_INI, b"\1")
        if command == VUP_START:
            self.starts.append(payload)
            if self.start_refused:
                return frame(RESP_ERROR, b"unsupported:1a")
            if self.restarted:
                # новая прошивка: ESP совпала, слот 1 уже поставлен
                self.rows[0].update(status=ST_SAME, flags=F_REMOTE | F_LOCAL | F_CAN | F_REBOOT, cur="0.1.124")
            out = frame(RESP_ACK) + frame(0x9A, b"\1")
            out += state(PH_GITHUB, 0, 0, 0, "Reading GitHub")
            if self.stat_path is not None:
                out += frame(VFS_STAT, self.stat_path + b"\0")    # опись карты Evo: ответит плагин
            for index, row in enumerate(self.rows):
                out += entry(index, row)
            return out + self.ready()
        if command == VUP_APPLY:
            indices = list(payload)
            self.applies.append(indices)
            out = frame(RESP_ACK) + frame(0x9B, b"\1")
            out += state(PH_APPLY, 0, len(indices), 0, "Installing")
            esp = False
            for index in indices:
                row = self.rows[index]
                if row["kind"] == K_ESP:
                    esp = True
                    continue                # ESP — последней
                row.update(status=ST_UPDATED, flags=F_REMOTE | F_LOCAL, cur=row["avail"])
                out += entry(index, row)
            if esp:
                self.rows[0].update(status=ST_UPDATED, flags=F_REMOTE | F_LOCAL, cur="0.1.124")
                out += entry(0, self.rows[0])
                self.restarted = True
                return out + state(PH_RESTART, 0, 0, 100, "vdac4-0.1.124")
            return out + self.ready("update done")
        if command == VUP_SYNC:
            self.syncs += 1
            out = state(PH_SYNC, 0, len(self.rows), 0, "")
            for index, row in enumerate(self.rows):
                out += entry(index, row)
            return out + self.ready()
        if command == VFS_STAT:
            self.vfs_replies.append(payload)
            return b""
        if command == VUP_STOP:
            self.stops += 1
            return frame(RESP_ACK) + frame(0x9C, b"\1")
        return frame(RESP_ERROR, b"unsupported")


class VupHarness(spi.WindowHarness):
    """Плагин целиком: zifi.ini «на карте», клавиши WC по кадрам, строки окна с PRSRW."""

    INI = b"SSID: home\r\npassword: secret\r\n"
    TXT_PAGE = 0x0E                         # страница текстового экрана WC

    def __init__(self) -> None:
        super().__init__(WMF, SYM, LinkCore(from_capacity=16384))
        self.files = {b"zifi": (0, True), b"zifi.ini": (len(self.INI), False)}
        self.file = self.INI
        self.keys: dict[int, int] = {}      # кадр WC -> клавиша (FN_*)
        self.text_rows: dict[int, bytearray] = {}   # строка окна -> знаки (PRSRW пишет с колонки E)
        self.plan = None                    # вызывается раз в кадр: сценарий клавиш
        self.esp = self.tick

    def tick(self, _h) -> None:
        # Кадр WC — раз в 40 опросов канала: столько главный цикл плагина успевает за кадр.
        if self.rx_polls % 40 == 0:
            self.next_frame()
            if self.plan is not None:
                self.plan(self)

    @property
    def frame_no(self) -> int:
        return self.word(base.WC_FRAMES)

    def handle_api(self) -> None:
        m = self.machine
        api = m.a
        if api == base.FN_ESC and self.plan is not None:
            self.plan(self)                 # ожидание Esc с HALT канал не опрашивает
        if api in KEYS:
            self.calls.append(api)
            self.flags(zero=self.keys.get(self.frame_no) != api)
            self.ret()
        elif api in (FN_GFILE, FN_GDIR, FN_PRIAT, FN_TURBOPL, FN_KBSCN):
            self.calls.append(api)
            self.flags(zero=True)
            self.ret()
        elif api == spi.FN_MNGC_PL and self.memory[base.ALT_A_CELL] == 0xFE:
            # A' = #FE — текстовый экран WC в #C000 (PRSRW пишет прямо туда), не страница плагина
            self.calls.append(api)
            self.map_page(0xC000, self.TXT_PAGE)
            self.ret()
        elif api == base.FN_PRSRW:
            self.calls.append(api)
            row = self.text_rows.setdefault(m.d, bytearray(b" " * 80))
            row[m.e - 1:m.e - 1 + m.bc] = bytes(self.memory[m.hl:m.hl + m.bc])
            self.flags(zero=True)
            self.ret()
        else:
            super().handle_api()

    @property
    def lines(self) -> dict[int, str]:
        return {line: bytes(text).decode("latin-1").rstrip() for line, text in self.text_rows.items()}

    def press(self, key: int, after: int = 3) -> None:
        self.keys[self.frame_no + after] = key

    def table_row(self, index: int) -> bytes:
        page = self.pages.get(self.PLUGIN_PAGE + 2, bytes(spi.WINDOW))
        if self.mapped[0xC000] == self.PLUGIN_PAGE + 2:
            page = bytes(self.memory[0xC000:0x10000])
        return page[index * 64:index * 64 + 64]

    def start(self, esp: VupEsp) -> None:
        esp_wrapper = esp
        self.app = esp_wrapper
        self.run(base.CODE, a=3)


class OnlineUpdatePluginTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        spi.need(WMF, SYM)

    def test_header(self) -> None:
        data = WMF.read_bytes()
        self.assertEqual(data[165:197].rstrip(b" "), b"VDAC4 Online Update v2.0")
        self.assertEqual(data[34], 3, "страницы: код, окно записи, таблица")

    def run_session(self, esp: VupEsp, scenario) -> VupHarness:
        h = VupHarness()
        h.plan = scenario
        h.start(esp)
        return h

    def test_list_marks_apply_and_restart_after_esp(self) -> None:
        """Список, отметки по умолчанию, Enter — APPLY отмеченных; ESP поставлена — этап 7, новый
        сеанс канала, READY, Wi-Fi и START заново; затем Esc — STOP."""
        esp = VupEsp()
        steps = {"enter": False, "esc": False}

        def scenario(h: VupHarness) -> None:
            if not steps["enter"] and esp.ready_sent >= 1 and VUP_APPLY not in esp.commands:
                steps["enter"] = True
                h.press(FN_ENTER, after=20)
            elif steps["enter"] and not steps["esc"] and len(esp.starts) >= 2 and esp.ready_sent >= 2:
                steps["esc"] = True
                h.esc_after = h.esc_polls + 2

        h = self.run_session(esp, scenario)
        self.assertEqual(esp.resets, 1, "SYS_RESET — один раз, в начале")
        self.assertEqual(esp.starts, [b"\1", b"\1"], "START с флагом «и карта Evo»; второй — после перезапуска ESP")
        self.assertEqual(len(esp.inis), 2, "zifi.ini — и после перезапуска ESP")
        self.assertTrue(esp.inis[0].startswith(b"SSID: home"))
        self.assertEqual(esp.applies, [[0, 3, 5]],
                         "отмечены по умолчанию ESP, слот 1 и плагин; новый файл, совпавший слот и OLDER — нет")
        self.assertEqual(esp.stops, 1)
        # после второго START строка ESP — same с новой версией
        row = h.table_row(0)
        self.assertEqual(row[0], ST_SAME)
        self.assertEqual(row[3:3 + 7], b"0.1.124")
        self.assertEqual(row[2], K_ESP)

    def test_window_shows_current_available_and_state(self) -> None:
        esp = VupEsp()
        seen = {}

        def scenario(h: VupHarness) -> None:
            drawn = "\n".join(h.lines.values())
            if "rows" not in seen and "CORES/MD.BIN" in drawn and "VDAC4COR.WMF" in drawn:
                seen["rows"] = dict(h.lines)
                h.esc_after = h.esc_polls + 2

        h = self.run_session(esp, scenario)
        lines = "\n".join(seen.get("rows", h.lines).values())
        self.assertIn("Current", lines)
        self.assertIn("Available", lines)
        self.assertRegex(lines, r"\[x\] ESP32-S3 firmware\s+0\.1\.123\s+0\.1\.124\s+DIFFERS")
        self.assertRegex(lines, r"BL616 firmware\s+42\s+41\s+OLDER")
        self.assertRegex(lines, r"\[ \] FPGA slot 0 TS-Config\s+1\.46\s+1\.46\s+same")
        self.assertRegex(lines, r"\[ \] Tang SD: CORES/MD\.BIN\s+-\s+886K\s+NEW")
        bl616 = [line for line in lines.splitlines() if "BL616" in line][0]
        self.assertNotIn("[", bl616.split("BL616")[0], "OLDER не отмечается и отметки не имеет")

    def test_old_firmware_refuses_start(self) -> None:
        esp = VupEsp(start_refused=True)

        def scenario(h: VupHarness) -> None:
            if h.esc_after is None and "ERROR" in "\n".join(h.lines.values()):
                h.esc_after = h.esc_polls + 2

        h = self.run_session(esp, scenario)
        text = "\n".join(h.lines.values())
        self.assertIn("unsupported:1a", text)
        self.assertNotIn(VUP_STOP, esp.commands, "START не узнан — STOP не нужен")


    def test_apply_without_esp_refreshes_the_list(self) -> None:
        """Пробел снимает отметку ESP; Enter — APPLY слота и плагина; итог без этапа 7 — SYNC списка."""
        esp = VupEsp()
        steps = {"space": False, "enter": False, "esc": False}

        def scenario(h: VupHarness) -> None:
            if not steps["space"] and esp.ready_sent >= 1:
                steps["space"] = True
                h.press(FN_SPACE, after=20)          # курсор на строке ESP
            elif steps["space"] and not steps["enter"] and h.frame_no > max(h.keys) + 20:
                steps["enter"] = True
                h.press(FN_ENTER, after=5)
            elif steps["enter"] and not steps["esc"] and esp.syncs >= 1 and esp.ready_sent >= 3:
                steps["esc"] = True
                h.esc_after = h.esc_polls + 2

        h = self.run_session(esp, scenario)
        self.assertEqual(esp.applies, [[3, 5]], "ESP снята пробелом")
        self.assertEqual(len(esp.starts), 1, "без этапа 7 ESP не перезапускается — START один")
        self.assertGreaterEqual(esp.syncs, 1, "после итога установки — повтор списка")
        self.assertEqual(h.table_row(3)[0], ST_UPDATED)
        self.assertEqual(h.table_row(0)[0], ST_DIFF)

    def test_evo_file_request_is_served(self) -> None:
        """Файловые запросы ESP к карте Evo (вид 5) обслуживает плагин — как у WC Update."""
        esp = VupEsp()
        esp.stat_path = b"/WC/VDAC4COR.WMF"

        def scenario(h: VupHarness) -> None:
            if esp.ready_sent >= 1 and h.esc_after is None:
                h.esc_after = h.esc_polls + 2

        h = VupHarness()
        h.files.update({b"WC": (0, True), b"VDAC4COR.WMF": (17408, False)})
        h.plan = scenario
        h.start(esp)
        self.assertEqual(len(esp.vfs_replies), 1)
        reply = esp.vfs_replies[0]
        self.assertEqual(reply[0], 0, "файл найден")
        self.assertEqual(int.from_bytes(reply[2:6], "little"), 17408)


if __name__ == "__main__":
    unittest.main()
