"""Машинные тесты плагина VDAC4 Cores (VDAC4 Cores/build/vdac4/VDAC4COR.WMF).

Плагин просит у ESP список ядер на microSD Tang (CORE_LIST #2C), показывает меню (↑/↓, Enter,
Esc), запускает слой ESP выбранного ядра (CORE_APP #2D: ИМЯ.ELF с SD Tang или общий слой
прошивки), просит загрузить ядро (START #70) и ждёт его по STATUS (#72); пока ядро работает, раз
в кадр шлёт INPUT (#71): джойстик Kempston, кнопки и счётчики мыши Kempston, байты очереди PS/2
AVR (регистры Gluk); F12 — EXIT (#73), ожидание слота 0, APP_STOP. Плагин исполняется в
эмуляторе Z80 на стенде канала SPI (test_spi_link_z80.SpiHarness); за каналом — модель прошивки
ESP с ядрами и слоем ядра.

Проверяется: меню (имена, размеры в КБ, «own ESP layer», «>» у выбранного, прокрутка), CORE_APP
и START — с именем выбранного; окно закрыто, пока ядро работает; в INPUT — порты Kempston и
байты PS/2 по порядку; выход — только по самому F12; Esc в меню — без запуска; ошибки ESP (нет
SD, нет ядер, слой не запустился, ядро не загрузилось), нет ESP — окно с текстом и выход по Esc;
Esc после START — не выход (0.4): загрузка идёт дальше, ядро работает, выход — по F12. Обработчик прерывания WC (I_N_T → PSD) в модели забирает
из очереди AVR по событию клавиши за кадр, пока плагин не выключит это функцией INT_PL (A' = 2):
пока ядро работает, все байты очереди — только в INPUT; ошибка ядра после этого — окно и Esc из
самой очереди PS/2.
С 0.16 — экран на HDMI Tang (COREMENU.ELF, команды #47…#49): окна WC нет, меню, «Loading» и ошибки
рисует экран; нет COREMENU.ELF или Tang без оверлеев — окно WC, как прежде.
Нужна сборка: "VDAC4 Cores\\build.bat".
"""
import struct
import unittest

import test_ftp_plugin_z80 as base
import test_spi_link_z80 as spi

ROOT = base.ROOT
CORES_WMF = ROOT / "VDAC4 Cores" / "build" / "vdac4" / "VDAC4COR.WMF"
CORES_SYM = ROOT / "VDAC4 Cores" / "build" / "vdac4" / "VDAC4COR.sym"

CMD_APP_STOP = 0x6D
CMD_CORE_LIST, CMD_CORE_APP = 0x2C, 0x2D
CMD_START, CMD_INPUT, CMD_STATUS, CMD_EXIT, CMD_TIME = 0x70, 0x71, 0x72, 0x73, 0x74
CMD_SD_ON, CMD_SD_DONE, CMD_SD_PART = 0x75, 0x76, 0x77
CMD_SCR_LIST, CMD_SCR_STATE, CMD_SCR_LEAVE = 0x47, 0x48, 0x49      # экран COREMENU.ELF
SCR_F_TANG, SCR_F_SHOWN, SCR_F_DRAWN, SCR_F_GONE = 1, 2, 4, 8
SCR_MENU, SCR_LOADING, SCR_ERROR, SCR_ERR_TEXT, SCR_ERR_CORE = 2, 5, 7, 6, 4
SD_READ, SD_WRITE = 7, 8
NEXT_SD = 0x03010027                     # VERSION ядра Next 0.39: часы и SD1
RDD, SDD = 0x400C, 0x400F                # внутренние входы ядра WildDOS: абсолютные сектора
STREAM1_PAGE = 0xF4                      # страница потока 1 (API 57 кладёт её в #600B)
PANEL_PAGE = 0xF0                        # что стоит в окне #0000 при входе плагина


def sector(lba: int) -> bytes:
    """Сектор карты Evo в модели драйвера WC."""
    return bytes((lba * 31 + (lba >> 8) + i * 7 + (i >> 8)) & 0xFF for i in range(512))
FN_UP, FN_DOWN, FN_ENTER, FN_WAITUP = 0x11, 0x12, 0x16, 0x2E
FN_MNGV_PL, FN_GVMOD = 0x40, 0x42      # текст WC (A' = 0); видеорежим плагина (A')
frame = base.frame


class CoresEsp:
    """Прошивка ESP: список \\CORES\\, CORE_APP и слой ядра с командами #70…#73."""

    def __init__(self, cores=(("NEXT", 907418, True), ("DEMO", 300000, False)), *, list_error=None,
                 app_error=None, load_polls=5, core_error=None, back_polls=3, run_error=None,
                 run_inputs=20, version=NEXT_SD, sd_requests=(), part_fail=False, screen=False,
                 tang=True) -> None:
        self.version = version
        # экран COREMENU.ELF: есть ли он на карте Tang и Tang с оверлеями
        self.screen = screen
        self.tang = tang
        self.app_cores: list[str] = []        # все CORE_APP по порядку
        self.scr_lists: list[bytes] = []
        self.scr_states: list[bytes] = []
        self.scr_leaves: list[int] = []
        self.scr_shown = False
        self.scr_gone = True
        self.scr_polls = 0
        # запросы SD1 ядра: (операция, номер, LBA, сектор записи) — по одному в ответе INPUT
        self.sd_queue = [list(r) for r in sd_requests]
        self.sd_on: list[bytes] = []          # SD_ON
        self.sd_done: list[bytes] = []        # SD_DONE своего номера
        self.sd_parts: list[bytes] = []       # SD_PART
        self.part_fail = int(part_fail)       # столько первых SD_PART — «нет такого запроса»
        self.sd_enabled = False
        self.input_frames: list[int] = []     # кадр (прерываний) каждого INPUT
        self.harness = None
        self.cores = list(cores)
        self.run_error = run_error           # ядро работало и сломалось после run_inputs INPUT
        self.run_inputs = run_inputs
        self.list_error = list_error
        self.app_error = app_error
        self.load_polls = load_polls
        self.core_error = core_error
        self.back_polls = back_polls
        self.app = None                      # имя запущенного слоя
        self.app_core = None                 # CORE_APP [имя]
        self.core = None                     # START [имя]
        self.state = 0
        self.inputs: list[bytes] = []
        self.commands: list[int] = []
        self.exit_seen = False
        self.times: list[bytes] = []          # TIME: часы Evo для часов ядра

    def __call__(self, command: int, payload: bytes) -> bytes:
        self.commands.append(command)
        if command == CMD_CORE_LIST:
            if self.list_error:
                return frame(0xAC, b"\0" + self.list_error.encode() + b"\0")
            body = bytes([1, len(self.cores)])
            for name, size, elf in self.cores:
                body += name.encode().ljust(16, b"\0") + struct.pack("<I", size) + bytes([1 if elf else 0])
            return frame(0xAC, body)
        if command == CMD_CORE_APP:
            self.app_core = payload[:payload.index(0)].decode()
            self.app_cores.append(self.app_core)
            if self.app_core == "COREMENU":
                if not self.screen:           # COREMENU.ELF нет — общий слой прошивки
                    self.app = "CORE"
                    return frame(0xAD, b"\0core\0" + b"0.1\0")
                self.app = "COREMENU"
                self.scr_shown, self.scr_gone, self.scr_polls = False, True, 0
                return frame(0xAD, b"\0coremenu\0" + b"0.1\0")
            if self.app_error:
                return frame(0xAD, b"\xff\0" + self.app_error.encode() + b"\0")
            own = any(n == self.app_core and elf for n, _, elf in self.cores)
            self.app = f"{self.app_core}.ELF" if own else "CORE"
            return frame(0xAD, b"\0" + (b"next" if own else b"core") + b"\0" + b"0.1\0")
        if command == CMD_APP_STOP:
            self.app = None
            return frame(CMD_APP_STOP | 0x80, b"\1")
        if self.app is None:
            return frame(0xEE, b"no app")
        if self.app == "COREMENU":
            return self.screen_command(command, payload)
        if command == CMD_START:
            self.core = payload[:payload.index(0)].decode()
            self.state = 1
            return frame(0xF0, b"\1")
        if command == CMD_STATUS:
            if self.state == 1:
                self.load_polls -= 1
                if self.load_polls <= 0:
                    self.state = 255 if self.core_error else 2
            elif self.state == 2 and self.exit_seen:
                self.back_polls -= 1
                if self.back_polls <= 0:
                    self.state = 3
            text = (self.core_error or self.run_error or "").encode() + b"\0"
            return frame(0xF2, bytes([self.state]) + struct.pack("<I", self.version) + text)
        if command == CMD_INPUT:
            self.inputs.append(payload)
            if self.harness is not None:
                self.input_frames.append(self.harness.interrupts)
            if self.run_error and len(self.inputs) >= self.run_inputs and self.state == 2:
                self.state = 255
            if self.sd_enabled and self.state == 2 and self.sd_queue:
                op, tag, lba = self.sd_queue[0][:3]
                return frame(0xF1, bytes([1, op, tag]) + struct.pack("<I", lba))
            return frame(0xF1, b"\1")
        if command == CMD_TIME:
            self.times.append(payload)
            return frame(0xF4, b"\1")
        if command == CMD_SD_ON:
            self.sd_on.append(payload)
            self.sd_enabled = (self.version & 0xFFFF) >= (NEXT_SD & 0xFFFF) and payload[:1] == b"\1"
            return frame(0xF5, b"\1" if self.sd_enabled else b"\0")
        if command == CMD_SD_PART:
            self.sd_parts.append(payload)
            head = self.sd_queue[0] if self.sd_queue else None
            if self.part_fail:
                self.part_fail -= 1
            elif head and head[0] == SD_WRITE and payload[0] == head[1] and payload[1] < 4:
                k = payload[1]
                return frame(0xF7, payload[:2] + head[3][k * 128:(k + 1) * 128])
            return frame(0xF7, bytes([0xFF, payload[1]]))
        if command == CMD_SD_DONE:
            head = self.sd_queue[0] if self.sd_queue else None
            if head and payload[0] == head[1] and struct.unpack("<I", payload[2:6])[0] == head[2]:
                self.sd_done.append(payload)
                self.sd_queue.pop(0)
                return frame(0xF6, b"\1")
            return frame(0xF6, b"\0")
        if command == CMD_EXIT:
            self.exit_seen = True
            if self.state in (0, 1):
                self.state = 3
            return frame(0xF3, b"\1")
        return frame(0xEE, b"unsupported")

    def screen_flags(self) -> int:
        if not self.tang:
            return SCR_F_GONE
        if self.scr_gone:
            return SCR_F_TANG | SCR_F_GONE
        return SCR_F_TANG | (SCR_F_SHOWN | SCR_F_DRAWN if self.scr_shown else 0)

    def screen_command(self, command: int, payload: bytes) -> bytes:
        """COREMENU.ELF: экран проявляется за три STATE и уходит за три LEAVE 0."""
        if command == CMD_SCR_LIST:
            self.scr_lists.append(payload)
            return frame(0xC7, b"\1")
        if command == CMD_SCR_STATE:
            self.scr_states.append(payload)
            if self.tang and self.scr_lists:
                if self.scr_gone:
                    self.scr_gone, self.scr_polls = False, 0
                self.scr_polls += 1
                self.scr_shown = self.scr_shown or self.scr_polls >= 3 or bool(payload[0] & 0x80)
            return frame(0xC8, bytes([self.screen_flags()]))
        if command == CMD_SCR_LEAVE:
            self.scr_leaves.append(payload[0])
            if payload[0] == 2:
                flags = self.screen_flags()
                self.app = None               # app_done: ядро ESP выгружает экран
                return frame(0xC9, bytes([flags]))
            if payload[0] == 0 and not self.scr_gone:
                self.scr_polls -= 1
                if self.scr_polls <= 0:
                    self.scr_gone, self.scr_shown = True, False
            return frame(0xC9, bytes([self.screen_flags()]))
        return frame(0xEE, b"unsupported")


def last(items: list, value) -> int:
    """Индекс последнего value."""
    return len(items) - 1 - items[::-1].index(value)


NES_CORE = 0x04010008                    # VERSION ядра NES 0.8 (личность 04)
NES_BROWSER_AND_IMAGE = {0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A}   # браузер SD Tang плагина, образ, CAPS


class NesMenuEsp(CoresEsp):
    """Слой NES.ELF 0.7: START без образа — ядро с меню F1 (картридж выбирают в самом ядре); refuse —
    слой до 0.7 такой START отвергает. #7B — учёт сохранений Evo после старта (Nes_SaveTrack): нет."""

    def __init__(self, refuse: bool = False, **kwargs) -> None:
        super().__init__(cores=(("NES", 907418, True), ("NEXT", 907418, True)), version=NES_CORE, **kwargs)
        self.refuse = refuse

    def __call__(self, command: int, payload: bytes) -> bytes:
        if self.app is not None and command == 0x7B:
            self.commands.append(command)
            return frame(0xFB, b"\1" + bytes(9))
        if self.app is not None and command == CMD_START and self.refuse:
            self.commands.append(command)
            return frame(0xF0, b"\0")
        return super().__call__(command, payload)


AMIGA_CORE = 0x0501000B                  # VERSION ядра Amiga 0.11 (личность 05)
CMD_SYS_CLOCK, CMD_EV_LIST = 0x29, 0x6E


class AmigaEsp(CoresEsp):
    """Слой AMIGA.ELF 0.18: карта Evo — том EVO в Workbench, только чтение. В ответе INPUT — по одному
    запросу из очереди: ("sd", номер, LBA, число) — [1][7][номер][LBA LE32][число], на каждый сектор
    плагин шлёт SD_DONE подряд; ("list",) — LIST корня, как меню F11 (ответ плагина — #6E)."""

    def __init__(self, requests=(), version=AMIGA_CORE, **kwargs) -> None:
        super().__init__(cores=(("AMIGA", 907418, True),), version=version, **kwargs)
        self.requests = [list(r) for r in requests]
        self.got: list[bytes] = []           # SD_DONE как пришли
        self.lists: list[bytes] = []         # ответы LIST (#6E)
        self.batches: list[int] = []         # секторов на каждый запрос в ответе INPUT

    def __call__(self, command: int, payload: bytes) -> bytes:
        if self.app is None:
            return super().__call__(command, payload)
        if command == CMD_SYS_CLOCK:
            self.commands.append(command)
            return frame(0xA9, b"\1")
        if command == CMD_INPUT and self.state == 2 and self.requests:
            self.commands.append(command)
            self.inputs.append(payload)
            if self.harness is not None:
                self.input_frames.append(self.harness.interrupts)
            head = self.requests[0]
            if head[0] == "list":
                self.requests.pop(0)
                return frame(0xF1, bytes([1, 1, 0, 0, 8]) + b"\0")
            _, tag, lba, count = head
            self.batches.append(0)
            return frame(0xF1, bytes([1, SD_READ, tag]) + struct.pack("<I", lba) + bytes([count]))
        if command == CMD_EV_LIST:
            self.commands.append(command)
            self.lists.append(payload)
            return frame(0xEE, b"\1")
        if command == CMD_SD_DONE:
            self.commands.append(command)
            self.got.append(payload)
            head = self.requests[0] if self.requests and self.requests[0][0] == "sd" else None
            ok = head is not None and payload[0] == head[1] and struct.unpack("<I", payload[2:6])[0] == head[2]
            if ok and payload[1]:
                self.requests.pop(0)         # отказ: в модели запрос снят
            elif ok:
                self.batches[-1] += 1
                head[2] += 1
                head[3] -= 1
                if head[3] <= 0:
                    self.requests.pop(0)
            return frame(0xF6, b"\1" if ok else b"\0")
        return super().__call__(command, payload)


class CoresHarness(spi.SpiHarness):
    """Плагин целиком: клавиши WC — по кадрам, очередь PS/2 Gluk и порты Kempston."""

    def __init__(self) -> None:
        super().__init__(CORES_WMF, CORES_SYM)
        self.keys: dict[int, int] = {}       # кадр -> клавиша WC (FN_UP/FN_DOWN/FN_ENTER)
        self.ps2: dict[int, bytes] = {}      # кадр -> байты, что встанут в очередь AVR
        self.queue = bytearray()
        self.gluk_reg = 0
        # часы Gluk (MC146818, BCD, 24 ч): 27.10.2026 12:34:56, обновления нет
        self.rtc = {0x00: 0x56, 0x02: 0x34, 0x04: 0x12, 0x07: 0x27, 0x08: 0x10, 0x09: 0x26, 0x0A: 0x00, 0x0B: 0x02}
        self.queue_resets = 0
        self.queue_enables = 0
        self.joy = 0x00
        self.mouse = (0xFF, 0x10, 0x20)      # кнопки (нажатая — 0), X, Y
        self.waitups = 0
        self.video: list[tuple[int, int]] = []   # (функция, A') — смена видеорежима WC
        self.int_pl: list[int] = []          # A' вызовов INT_PL
        self.wc_ps2 = True                   # WC разбирает очередь PS/2 в прерывании (флаг #7F4E)
        self.wc_events = bytearray()         # байты, что забрал обработчик прерывания WC
        self.isr_seen = 0                    # прерываний, обработанных моделью WC
        # потоки WC (API 57) и драйвер: RDD/SDD с абсолютным LBA, страница потока — в окне #0000
        self.streams: list[tuple[int, int, int]] = []   # (D, B, C) вызовов STREAM
        self.stream1_created = False
        self.page0 = PANEL_PAGE              # последнее значение порта #10AF
        self.memory[0x6000] = PANEL_PAGE
        self.memory[0x600B] = 0
        self.sd_io: list[tuple[str, int, int, int]] = []  # (RDD/SDD, LBA, буфер, страница #0000)
        self.sd_written: dict[int, bytes] = {}
        self.rdd_fail = False
        for address in (RDD, SDD):
            self.machine.set_memory_block(address, b"\xC9")     # RET после модели
            self.machine.set_breakpoint(address)
        self.hooks[RDD] = lambda: self.sector_io("RDD")
        self.hooks[SDD] = lambda: self.sector_io("SDD")

    def sector_io(self, name: str) -> None:
        """WildDOS RDD/SDD (docs/04 WC Improved, 8.5): DE:HL — LBA, BC — буфер, A — секторов; A = ABT,
        Z — вышло. Только с потоком SD-ZC, чья страница стоит в окне #0000 (и в тени #6000)."""
        m = self.machine
        lba = m.de << 16 | m.hl
        page = self.memory[0x6000]
        self.sd_io.append((name, lba, m.bc, page))
        good = (m.a == 1 and page == STREAM1_PAGE and self.page0 == STREAM1_PAGE and self.stream1_created
                and 0x8000 <= m.bc <= 0xC000 - 512)
        if good and name == "RDD" and not self.rdd_fail:
            self.machine.set_memory_block(m.bc, sector(lba))
        elif good and name == "SDD":
            self.sd_written[lba] = bytes(self.memory[m.bc:m.bc + 512])
        else:
            good = False
        m.a = 0 if good else 1
        self.flags(zero=good)

    def handle_api(self) -> None:
        m = self.machine
        if m.a in (FN_UP, FN_DOWN, FN_ENTER):
            self.calls.append(m.a)
            self.flags(zero=self.keys.get(self.interrupts) != m.a)
            self.ret()
        elif m.a == FN_WAITUP:
            self.calls.append(m.a)
            self.waitups += 1
            self.flags(zero=True)
            self.ret()
        elif m.a in (FN_GVMOD, FN_MNGV_PL):
            self.calls.append(m.a)
            self.video.append((m.a, m.alt_a))
            self.ret()
        elif m.a == base.FN_STREAM:
            # D=1, B=0 — создать поток 1 на SD-ZC; D=1, BC=#FFFF — выбрать его (#600B — его страница)
            self.calls.append(m.a)
            d, b, c = m.de >> 8, m.bc >> 8, m.bc & 0xFF
            self.streams.append((d, b, c))
            if d == 1 and m.bc != 0xFFFF:
                self.stream1_created = b == 0
            if d == 1:
                self.memory[0x600B] = STREAM1_PAGE
            self.flags(zero=True)
            self.ret()
        elif m.a == base.FN_INT_PL:
            # MD20.ASM INT_PL: A' = 0 — всё выключить, 1 — часы, 2 — разбор PS/2
            self.calls.append(m.a)
            self.int_pl.append(m.alt_a)
            if m.alt_a in (0, 2):
                self.wc_ps2 = False
            self.flags(zero=True)
            self.ret()
        else:
            super().handle_api()

    def wc_interrupts(self) -> None:
        """Прерывания с прошлого чтения очереди: байты кадра — в очередь AVR, и обработчик WC
        (I_N_T → L7143_101 → PSD, пока флаг #7F4E не снят) берёт из неё одно событие —
        префиксы E0/F0 и код клавиши."""
        while self.isr_seen < self.interrupts:
            self.isr_seen += 1
            for at in sorted(k for k in self.ps2 if k <= self.isr_seen):
                self.queue += self.ps2.pop(at)
            while self.wc_ps2 and self.queue:
                byte = self.queue.pop(0)
                self.wc_events.append(byte)
                if byte not in (0xE0, 0xF0):
                    break

    def port_in(self, port: int) -> int:
        if port & 0xFF == 0xF7:
            if port >> 8 == 0xBF and self.gluk_reg == 0xF0:
                self.wc_interrupts()
                return self.queue.pop(0) if self.queue else 0
            if port >> 8 == 0xBF and self.gluk_reg in self.rtc:
                return self.rtc[self.gluk_reg]
            return 0xFF if port >> 8 != 0xBF else 0
        if port & 0xFF == 0x1F:
            return self.joy
        if port == 0xFADF:
            return self.mouse[0]
        if port == 0xFBDF:
            return self.mouse[1]
        if port == 0xFFDF:
            return self.mouse[2]
        return super().port_in(port)

    def port_out(self, port: int, value: int) -> None:
        if port == 0x10AF:
            self.page0 = value
            return
        if port == 0xEFF7:
            return
        if port == 0xDFF7:
            self.gluk_reg = value
            return
        if port == 0xBFF7:
            if self.gluk_reg == 0x0C and value == 1:
                self.queue_resets += 1
                self.queue.clear()
            if self.gluk_reg == 0xF0 and value == 2:
                self.queue_enables += 1
            return
        super().port_out(port, value)

    def text(self) -> str:
        return "\n".join(self.screen.get(line, bytearray(b" " * 80)).decode("latin-1").rstrip()
                         for line in range(1, 20))

    def start(self, esp: CoresEsp, keys=None, ps2=None, esc: int | None = None) -> None:
        esp.harness = self
        self.app = esp
        self.keys = dict(keys or {})
        self.ps2 = dict(ps2 or {})
        self.esc_after = esc
        self.run(base.CODE, a=3)


F12 = b"\x07"


class CoresPluginTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        spi.need(CORES_WMF, CORES_SYM)

    def test_header_names_the_plugin(self) -> None:
        data = CORES_WMF.read_bytes()
        self.assertEqual(data[165:197].rstrip(b" "), b"VDAC4 Cores v0.16")
        self.assertEqual(data[34], 1, "одна страница")
        self.assertEqual(len(data) % 512, 0, "файл — целыми секторами")

    def test_next_session(self) -> None:
        h = CoresHarness()
        esp = CoresEsp()
        h.joy, h.mouse = 0x11, (0xFE, 0x55, 0xAA)
        # Enter на 5-м кадре — первое ядро (NEXT); ядро работает с ~60-го кадра
        h.start(esp, keys={5: FN_ENTER},
                ps2={80: b"\x1c\xf0\x1c", 83: b"\xe0\x75", 86: b"\xe0\xf0\x75", 90: F12})
        self.assertEqual(h.waitups, 1, "меню ждёт отпускания Enter запуска плагина")
        text = h.text()
        self.assertIn("> NEXT", text)
        self.assertIn("886 KB  own ESP layer", text)
        self.assertIn("  DEMO", text)
        self.assertIn("292 KB", text)
        self.assertIn("Loading NEXT into the Tang...", text)
        self.assertEqual(esp.app_core, "NEXT")
        self.assertEqual(esp.core, "NEXT")
        self.assertEqual(len(h.prwow_windows), 1)
        self.assertEqual(h.calls.count(base.FN_RRESB), 1, "окно закрыто, пока ядро работает")
        self.assertTrue(esp.inputs, "INPUT не шёл")
        self.assertTrue(all(p[:4] == bytes([0x11, 0xFE, 0x55, 0xAA]) for p in esp.inputs))
        keys = b"".join(p[4:] for p in esp.inputs)
        self.assertEqual(keys, b"\x1c\xf0\x1c\xe0\x75\xe0\xf0\x75" + F12)
        self.assertEqual(h.wc_events, b"", "пока ядро работает, WC очередь PS/2 не трогает")
        self.assertEqual(h.int_pl, [1, 2], "разбор PS/2 в WC выключен на время ядра")
        self.assertLess(h.calls.index(FN_GVMOD), len(h.calls) - h.calls[::-1].index(base.FN_INT_PL),
                        "INT_PL 2 — когда ядро уже работает")
        self.assertEqual((h.queue_resets, h.queue_enables), (1, 1), "очередь AVR включена один раз")
        self.assertLess(esp.commands.index(CMD_EXIT), last(esp.commands, CMD_APP_STOP))
        # экрана нет (COREMENU.ELF нет): общий слой снят сразу, дальше — окно WC
        self.assertEqual(esp.app_cores, ["COREMENU", "NEXT"])
        self.assertEqual(esp.commands[:2], [CMD_CORE_APP, CMD_APP_STOP])
        # время Evo — один раз, когда ядро уже работает, до первого INPUT
        self.assertEqual(esp.times, [bytes([56, 34, 12, 27, 10, 26])])
        self.assertLess(esp.commands.index(CMD_START), esp.commands.index(CMD_TIME))
        self.assertLess(esp.commands.index(CMD_TIME), esp.commands.index(CMD_INPUT))
        # ядро работает — Evo в режиме FT812 (видеошина молчит, M_SEL = 1); выход — текст WC
        self.assertEqual(h.video, [(FN_GVMOD, 0x04), (FN_MNGV_PL, 0)])
        self.assertLess(h.calls.index(base.FN_RRESB), h.calls.index(FN_GVMOD), "режим — после окна")
        self.assertEqual(esp.state, 3)
        self.assertIsNone(esp.app, "APP_STOP после выхода")
        self.assertFalse(h.slave.selected)

    def sd_session(self, esp: CoresEsp, h: CoresHarness | None = None) -> CoresHarness:
        h = h or CoresHarness()
        h.start(esp, keys={5: FN_ENTER}, ps2={150: F12})
        return h

    def test_next_sd_read_write(self) -> None:
        data = bytes(((i * 5 + 3) ^ (i >> 3)) & 0xFF for i in range(512))
        esp = CoresEsp(sd_requests=[(SD_READ, 0x11, 0x12345), (SD_WRITE, 0x12, 0x777, data),
                                    (SD_READ, 0x13, 0)])
        h = self.sd_session(esp)
        # поток 1 — заново на SD-ZC (не клон панели), затем SD_ON [1]
        self.assertIn((1, 0, 0), h.streams)
        self.assertEqual(esp.sd_on, [b"\1"])
        self.assertLess(esp.commands.index(CMD_TIME), esp.commands.index(CMD_SD_ON))
        self.assertLess(esp.commands.index(CMD_SD_ON), esp.commands.index(CMD_INPUT))
        self.assertEqual(esp.sd_queue, [], "все запросы исполнены")
        self.assertEqual(esp.sd_done[0], bytes([0x11, 0]) + struct.pack("<I", 0x12345) + sector(0x12345))
        self.assertEqual(esp.sd_parts, [bytes([0x12, k]) for k in range(4)])
        self.assertEqual(h.sd_written, {0x777: data})
        self.assertEqual(esp.sd_done[1], bytes([0x12, 0]) + struct.pack("<I", 0x777))
        self.assertEqual(esp.sd_done[2], bytes([0x13, 0]) + struct.pack("<I", 0) + sector(0))
        self.assertEqual([(n, lba, page) for n, lba, _, page in h.sd_io],
                         [("RDD", 0x12345, STREAM1_PAGE), ("SDD", 0x777, STREAM1_PAGE), ("RDD", 0, STREAM1_PAGE)])
        self.assertEqual((h.memory[0x6000], h.page0), (PANEL_PAGE, PANEL_PAGE), "окно #0000 — прежнее")
        self.assertEqual(h.machine.a, 3, "Next писал на карту Evo — WC перечитает панели")
        # после запроса опрос — без кадров: три запроса подряд, затем ещё 40 ВВОДов (SD_BURST)
        same = sum(1 for a, b in zip(esp.input_frames, esp.input_frames[1:]) if a == b)
        self.assertEqual(same, 2 + 1 + 40)
        self.assertEqual(esp.state, 3)

    def test_next_sd_read_only_session_returns_zero(self) -> None:
        esp = CoresEsp(sd_requests=[(SD_READ, 0x21, 5)])
        h = self.sd_session(esp)
        self.assertEqual(esp.sd_done, [bytes([0x21, 0]) + struct.pack("<I", 5) + sector(5)])
        self.assertEqual(h.machine.a, 0, "только чтение — WC перечитывать нечего")

    def test_next_sd_driver_failure(self) -> None:
        esp = CoresEsp(sd_requests=[(SD_READ, 0x31, 9)])
        h = CoresHarness()
        h.rdd_fail = True
        self.sd_session(esp, h)
        self.assertEqual(esp.sd_done, [bytes([0x31, 2]) + struct.pack("<I", 9)], "отказ — без сектора")

    def test_next_sd_part_refused_then_written_once(self) -> None:
        # кусок не пришёл (ESP: «нет такого запроса») — карту не трогать; ESP предлагает запрос
        # снова — сектор пишется один раз, целиком
        data = bytes(range(256)) * 2
        esp = CoresEsp(sd_requests=[(SD_WRITE, 0x41, 0x99, data)], part_fail=1)
        h = self.sd_session(esp)
        self.assertEqual(esp.sd_parts, [bytes([0x41, 0])] + [bytes([0x41, k]) for k in range(4)])
        self.assertEqual([n for n, *_ in h.sd_io], ["SDD"])
        self.assertEqual(h.sd_written, {0x99: data})
        self.assertEqual(esp.sd_done, [bytes([0x41, 0]) + struct.pack("<I", 0x99)])
        self.assertEqual(h.machine.a, 3)

    def test_old_next_core_gets_no_time_and_no_sd(self) -> None:
        esp = CoresEsp(version=0x03010025)
        h = self.sd_session(esp)
        self.assertEqual((esp.times, esp.sd_on), ([], []))
        self.assertNotIn((1, 0, 0), h.streams)
        self.assertEqual(h.machine.a, 0)
        esp = CoresEsp(version=0x03010026)
        h = self.sd_session(esp)
        self.assertEqual((len(esp.times), esp.sd_on), (1, []), "0.38: часы есть, SD1 нет")

    def test_other_core_gets_no_time_and_no_sd(self) -> None:
        esp = CoresEsp(version=0x04010027)
        h = self.sd_session(esp)
        self.assertEqual((esp.times, esp.sd_on), ([], []), "у слоя NES #74…#79 — свои команды")
        self.assertNotIn((1, 0, 0), h.streams)

    def amiga_reads(self, h: CoresHarness) -> list[int]:
        return [lba for n, lba, _, page in h.sd_io if n == "RDD" and page == STREAM1_PAGE]

    def test_amiga_reads_evo_sectors(self) -> None:
        # 0.15: Amiga — карта Evo томом EVO; сектора подряд по запросу, LBA переходит через #FFFF
        esp = AmigaEsp(requests=[("sd", 0x51, 0x2000, 3), ("sd", 0x52, 0xFFFF, 2)])
        h = self.sd_session(esp)
        self.assertEqual(esp.requests, [], "все запросы исполнены")
        want = [(0x51, 0x2000), (0x51, 0x2001), (0x51, 0x2002), (0x52, 0xFFFF), (0x52, 0x10000)]
        self.assertEqual(esp.got, [bytes([tag, 0]) + struct.pack("<I", lba) + sector(lba) for tag, lba in want])
        self.assertEqual(self.amiga_reads(h), [lba for _, lba in want])
        self.assertEqual([n for n, *_ in h.sd_io].count("SDD"), 0, "записи нет")
        self.assertEqual(h.streams.count((1, 0, 0)), 1, "поток 1 на SD-ZC создан один раз")
        self.assertEqual(esp.sd_on, [], "SD_ON — только Next")
        self.assertEqual((h.memory[0x6000], h.page0), (PANEL_PAGE, PANEL_PAGE), "окно #0000 — прежнее")
        self.assertEqual(h.machine.a, 0, "карта не менялась — WC перечитывать нечего")
        # после запроса — 40 ВВОДов без кадра (SD_BURST): второй запрос и хвост
        same = sum(1 for a, b in zip(esp.input_frames, esp.input_frames[1:]) if a == b)
        self.assertEqual(same, 1 + 1 + 40)
        self.assertEqual(esp.state, 3)

    def test_amiga_sector_count_limits(self) -> None:
        esp = AmigaEsp(requests=[("sd", 0x61, 100, 0), ("sd", 0x62, 200, 40)])
        h = self.sd_session(esp)
        # 0 — один сектор; больше 16 — по 16 за запрос, остальное ESP спрашивает снова
        self.assertEqual(esp.batches, [1, 16, 16, 8])
        self.assertEqual(self.amiga_reads(h), [100] + list(range(200, 240)))

    def test_amiga_driver_failure_stops_request(self) -> None:
        esp = AmigaEsp(requests=[("sd", 0x71, 9, 4)])
        h = CoresHarness()
        h.rdd_fail = True
        self.sd_session(esp, h)
        self.assertEqual(esp.got, [bytes([0x71, 2]) + struct.pack("<I", 9)], "отказ — без сектора, дальше не читать")
        self.assertEqual(self.amiga_reads(h), [9])

    def test_amiga_menu_clone_then_sector_recreates_stream(self) -> None:
        # меню F11 клонирует панель в поток 1 — следующее чтение снова создаёт поток на SD-ZC
        esp = AmigaEsp(requests=[("sd", 0x81, 5, 1), ("list",), ("sd", 0x82, 6, 1)])
        h = self.sd_session(esp)
        self.assertEqual(esp.requests, [])
        self.assertEqual(len(esp.lists), 1)
        created = [i for i, s in enumerate(h.streams) if s == (1, 0, 0)]
        clones = [i for i, s in enumerate(h.streams) if s[0] == 0xFE]
        self.assertEqual(len(created), 2)
        self.assertEqual(len(clones), 1)
        self.assertLess(created[0], clones[0])
        self.assertLess(clones[0], created[1])
        self.assertEqual(self.amiga_reads(h), [5, 6])

    def test_other_core_ignores_amiga_sector_request(self) -> None:
        esp = AmigaEsp(requests=[("sd", 0x91, 5, 1)], version=NES_CORE)
        h = self.sd_session(esp)
        self.assertEqual((esp.got, h.sd_io), ([], []), "операция 7 не у Amiga — не исполняется")

    def test_nes_from_menu_starts_without_browser(self) -> None:
        # 0.13: NES из меню — сразу START без образа; картридж выбирают в меню F1 ядра (NES.ELF 0.7)
        esp = NesMenuEsp()
        h = self.sd_session(esp)
        self.assertEqual((esp.app_core, esp.core), ("NES", "NES"))
        start = esp.commands.index(CMD_START)
        self.assertEqual(esp.commands[start - 1], CMD_CORE_APP, "между CORE_APP и START — ничего")
        self.assertFalse(set(esp.commands) & NES_BROWSER_AND_IMAGE, "ни браузера SD Tang, ни образа, ни TIME")
        self.assertEqual(esp.sd_on, [])
        self.assertTrue(esp.inputs, "ядро работает — INPUT шёл")
        self.assertEqual(b"".join(p[4:] for p in esp.inputs), F12)
        self.assertNotIn("NES cartridges", h.text(), "окна браузера плагина нет")
        self.assertEqual(esp.state, 3)
        self.assertIsNone(esp.app, "APP_STOP после выхода")

    def test_nes_from_menu_old_layer_explains_update(self) -> None:
        esp = NesMenuEsp(refuse=True)
        self.fatal(esp, "ERROR: update CORES/NES.ELF to 0.7+")
        self.assertEqual(esp.inputs, [])
        self.assertIn(CMD_APP_STOP, esp.commands)

    def test_refused_start_of_other_core_keeps_its_text(self) -> None:
        # отказ START у не-NES — прежний текст (подсказка про NES.ELF только для NES из меню)
        esp = NesMenuEsp(refuse=True)
        self.fatal(esp, "ERROR: the ESP layer did not start", keys={4: FN_DOWN, 6: FN_ENTER})
        self.assertEqual(esp.app_core, "NEXT")
        self.assertIn(CMD_START, esp.commands)

    def test_menu_down_selects_second_core(self) -> None:
        h = CoresHarness()
        esp = CoresEsp()
        h.start(esp, keys={4: FN_DOWN, 6: FN_DOWN, 8: FN_ENTER}, ps2={90: F12})
        self.assertEqual(esp.app_core, "DEMO", "↓ дальше последнего не уходит")
        self.assertEqual(esp.app, None)
        self.assertEqual(esp.core, "DEMO")
        self.assertIn("> DEMO", h.text())

    def test_menu_scrolls(self) -> None:
        h = CoresHarness()
        cores = [(f"CORE{i:02d}", 100 * 1024 * (i + 1), i % 2 == 0) for i in range(12)]
        esp = CoresEsp(cores)
        keys = {4 + 2 * i: FN_DOWN for i in range(10)}
        keys[30] = FN_UP
        keys[32] = FN_ENTER
        h.start(esp, keys=keys, ps2={100: F12})
        self.assertEqual(esp.app_core, "CORE09")
        text = h.text()
        self.assertIn("> CORE09", text)
        self.assertIn("  CORE10", text, "видно 8 строк вокруг выбранного")
        self.assertIn("  CORE03", text)
        self.assertNotIn("CORE02", text, "строки выше окна меню не видно")

    def test_esc_in_menu(self) -> None:
        h = CoresHarness()
        esp = CoresEsp()
        h.start(esp, esc=10)
        self.assertEqual(esp.app_cores, ["COREMENU"], "слоя ядра нет")
        self.assertNotIn(CMD_START, esp.commands)
        self.assertEqual(h.calls.count(base.FN_RRESB), 1)

    def fatal(self, esp: CoresEsp, text: str, keys=None, esc=200) -> CoresHarness:
        h = CoresHarness()
        h.start(esp, keys=keys or {5: FN_ENTER}, esc=esc)
        self.assertIn(text, h.text())
        self.assertEqual(h.calls.count(base.FN_RRESB), 1, "окно ошибки закрыто при выходе")
        window = h.prwow_windows[0]
        self.assertLessEqual(window[3] + window[5], 25, "окно помещается в экран 25 строк")
        return h

    def test_list_error(self) -> None:
        esp = CoresEsp(list_error="the Tang core has no SD bridge")
        self.fatal(esp, "ERR: the Tang core has no SD bridge", esc=5)
        self.assertEqual(esp.app_cores, ["COREMENU"], "слоя ядра нет")

    def test_no_cores(self) -> None:
        esp = CoresEsp(cores=())
        self.fatal(esp, "ERROR: no cores in \\CORES on the Tang SD", esc=5)

    def test_app_error(self) -> None:
        esp = CoresEsp(app_error="core ELF is not loadable")
        self.fatal(esp, "ERR: core ELF is not loadable")
        self.assertNotIn(CMD_START, esp.commands)
        self.assertNotIn(CMD_EXIT, esp.commands, "слой не запущен — выходить нечем")

    def test_core_error(self) -> None:
        esp = CoresEsp(core_error="core not found in the BL616 cache")
        self.fatal(esp, "ERR: core not found in the BL616 cache")
        self.assertIn(CMD_EXIT, esp.commands)
        self.assertIn(CMD_APP_STOP, esp.commands)
        self.assertEqual(esp.inputs, [])

    def test_esc_after_start_does_not_exit(self) -> None:
        h = CoresHarness()
        esp = CoresEsp(load_polls=8)
        # Esc «нажат» с 20-го опроса WC_ESC: в меню их меньше (Enter на 5-м кадре), дальше плагин
        # Esc не спрашивает — загрузка идёт, ядро работает, выход — по F12
        h.start(esp, keys={5: FN_ENTER}, esc=20, ps2={150: F12})
        self.assertLess(h.calls.count(base.FN_ESC), 20, "после START Esc не спрашивается")
        self.assertEqual(esp.core, "NEXT")
        self.assertTrue(esp.inputs, "ядро заработало — INPUT шёл")
        self.assertEqual(b"".join(p[4:] for p in esp.inputs), F12)
        self.assertEqual(h.video, [(FN_GVMOD, 0x04), (FN_MNGV_PL, 0)])
        self.assertEqual(h.int_pl, [1, 2])
        self.assertLess(esp.commands.index(CMD_EXIT), last(esp.commands, CMD_APP_STOP))
        self.assertEqual(esp.state, 3)
        self.assertEqual(h.calls.count(base.FN_RRESB), 1)

    def test_f12_release_and_e0_07_do_not_exit(self) -> None:
        h = CoresHarness()
        esp = CoresEsp()
        h.start(esp, keys={5: FN_ENTER}, ps2={70: b"\xf0\x07", 80: b"\xe0\x07", 110: F12})
        keys = b"".join(p[4:] for p in esp.inputs)
        self.assertEqual(keys, b"\xf0\x07\xe0\x07" + F12, "выход только по самому F12")
        self.assertGreater(len(esp.inputs), 30)

    def test_core_error_while_running_waits_esc_from_ps2(self) -> None:
        h = CoresHarness()
        esp = CoresEsp(run_error="core stopped")
        # WC клавиш уже не видит (INT_PL 2): WC_ESC не нажимается никогда, Esc — в очереди PS/2
        h.start(esp, keys={5: FN_ENTER}, ps2={250: b"\xf0\x76", 260: b"\xe0\x76", 300: b"\x76"})
        self.assertIn("ERR: core stopped", h.text())
        self.assertIn(CMD_EXIT, esp.commands)
        self.assertIn(CMD_APP_STOP, esp.commands)
        self.assertGreaterEqual(h.interrupts, 300, "выход — только по самому Esc (#76 без префиксов)")
        self.assertEqual(h.wc_events, b"")

    # --- экран на HDMI Tang (COREMENU.ELF, 0.16) ---

    def test_screen_menu_and_core(self) -> None:
        h = CoresHarness()
        esp = CoresEsp(screen=True)
        h.start(esp, keys={8: FN_DOWN, 12: FN_UP, 16: FN_ENTER}, ps2={150: F12})
        self.assertEqual(h.prwow_windows, [], "окна WC нет: всё рисует экран")
        self.assertEqual(esp.app_cores, ["COREMENU", "NEXT"])
        body = b"".join(n.encode().ljust(16, b"\0") + struct.pack("<I", s) + bytes([1 if e else 0])
                        for n, s, e in esp.cores)
        self.assertEqual(esp.scr_lists, [bytes([len(esp.cores)]) + body], "LIST — тело CORE_LIST")
        states = [(p[0], p[1]) for p in esp.scr_states]
        self.assertEqual(states[0], (SCR_MENU, 0))
        self.assertIn((SCR_MENU, 1), states, "↓ — выбор у экрана")
        self.assertEqual(states[-1], (SCR_LOADING, 0), "перед запуском — «Loading» выбранного")
        self.assertGreaterEqual(states.count((SCR_MENU, 0)) + states.count((SCR_MENU, 1)), 14,
                                "STATE — раз в кадр меню")
        self.assertTrue(all(p[2] == 0xFF and p[-1] == 0 for p in esp.scr_states))
        # картинка остаётся: LEAVE 1 до «кадр на экране», затем CORE_APP ядра; LEAVE 2 нет
        self.assertEqual(set(esp.scr_leaves), {1})
        self.assertLess(last(esp.commands, CMD_SCR_LEAVE), last(esp.commands, CMD_CORE_APP))
        self.assertEqual(esp.core, "NEXT")
        self.assertTrue(esp.inputs, "ядро работало")
        self.assertEqual(esp.state, 3)
        self.assertIsNone(esp.app)

    def test_screen_esc(self) -> None:
        h = CoresHarness()
        esp = CoresEsp(screen=True)
        h.start(esp, esc=10)
        self.assertEqual(h.prwow_windows, [])
        self.assertEqual(esp.app_cores, ["COREMENU"])
        self.assertNotIn(CMD_START, esp.commands)
        # уход плавно: LEAVE 0, пока экран не скажет «нет», затем LEAVE 2 — выгрузка
        self.assertEqual(esp.scr_leaves[-1], 2)
        self.assertGreaterEqual(esp.scr_leaves.count(0), 3)
        self.assertNotIn(1, esp.scr_leaves)
        self.assertIsNone(esp.app)

    def test_screen_without_tang_falls_back_to_window(self) -> None:
        h = CoresHarness()
        esp = CoresEsp(screen=True, tang=False)
        h.start(esp, keys={70: FN_ENTER}, ps2={200: F12})
        self.assertEqual(len(h.prwow_windows), 1, "Tang без оверлеев — окно WC")
        self.assertIn("Loading NEXT into the Tang...", h.text())
        self.assertEqual(esp.scr_leaves[-1:], [2], "экран выгружен до окна")
        self.assertLess(last(esp.commands, CMD_SCR_LEAVE), last(esp.commands, CMD_CORE_APP))
        self.assertEqual(esp.app_cores, ["COREMENU", "NEXT"])
        self.assertEqual(esp.core, "NEXT")

    def test_screen_core_error_shows_on_screen(self) -> None:
        h = CoresHarness()
        esp = CoresEsp(screen=True, core_error="core not found in the BL616 cache")
        h.start(esp, keys={8: FN_ENTER}, esc=40)
        self.assertEqual(h.prwow_windows, [], "ошибка — экраном, без окна WC")
        # слой ядра снят, экран — снова, сразу непрозрачным (картинка «Loading» ещё на HDMI)
        self.assertEqual(esp.app_cores, ["COREMENU", "NEXT", "COREMENU"])
        self.assertLess(last(esp.commands, CMD_APP_STOP), last(esp.commands, CMD_CORE_APP))
        error = esp.scr_states[-1]
        self.assertEqual(error[0], SCR_ERROR | 0x80)
        self.assertEqual(error[3], SCR_ERR_TEXT)
        self.assertTrue(error[4:].startswith(b"ERR: core not found in the BL616 cache"))
        self.assertEqual(len(esp.scr_lists), 2, "новому экрану — список снова")
        self.assertEqual(esp.scr_leaves[-1], 2)
        self.assertIsNone(esp.app)

    def test_screen_list_error(self) -> None:
        h = CoresHarness()
        esp = CoresEsp(screen=True, list_error="no SD card in the Tang")
        h.start(esp, esc=20)
        self.assertEqual(h.prwow_windows, [])
        self.assertEqual(esp.scr_lists, [b"\0"], "список пуст, экрану он нужен всё равно")
        error = esp.scr_states[-1]
        self.assertEqual((error[0], error[3]), (SCR_ERROR, SCR_ERR_TEXT))
        self.assertTrue(error[4:].startswith(b"ERR: no SD card in the Tang"))
        self.assertEqual(esp.scr_leaves[-1], 2)

    def test_screen_no_cores_has_own_text(self) -> None:
        h = CoresHarness()
        esp = CoresEsp(screen=True, cores=())
        h.start(esp, esc=20)
        self.assertEqual(h.prwow_windows, [])
        error = esp.scr_states[-1]
        self.assertEqual((error[0], error[3], error[4:]), (SCR_ERROR, 0, b"\0"), "код «нет ядер», текст у экрана свой")

    def test_no_esp(self) -> None:
        h = CoresHarness()
        h.link.die()
        esp = CoresEsp()
        h.start(esp, esc=5)
        self.assertIn("ERROR: no ESP on the SPI", h.text())
        self.assertEqual(esp.commands, [])


if __name__ == "__main__":
    unittest.main()
