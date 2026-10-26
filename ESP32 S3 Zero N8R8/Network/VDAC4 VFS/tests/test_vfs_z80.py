"""VDAC4 VFS на эмуляторе Z80: настоящие страницы #05 и #02 WC Improved v1.11i из boot.$C,
собранные VFSRES.WMF и VDAC4VFS.WMF, канал SPI с моделью прошивки ESP (tests/spi_link_model.py) и
моделью службы VDISK.

Память — постраничная: окна #0000/#4000/#8000/#C000 переключают порты #10AF…#13AF (чтение
#13AF отдаёт страницу окна #C000), как TS-Config. Код WC исполняется настоящий: DOS_SWP, MNG8,
MNGC. Заглушки — только там, где нужен весь WC: DRACT (подключает страницу панели и, если
драйвер не выбран, уходит в настоящий DOS_SWP), ORIGA (DEVINI и чтение сектора 0 через
установленный драйвер), XPAN, STATUS, шлюз расширения (#5123) и SELDEV_INVALIDATE (#5BE4).

Проверяется: установка резидента; включение и выключение переключателем (сверка WC, поиск
страницы, переходник, «JP WC_TRAMP», состояние, перелёт через страницу #02 и перемонтирование
панели на SD2); DOS_SWP(6) — образ драйвера, прежние страница #C000, IX и прерывания; чтение
секторов в буфер любого окна, ожидание медленной ESP с окнами для прерываний, отказ источника,
прошивка без VDISK; отказы переключателя (чужой WC, нет резидента, занята область #02).
"""
import pathlib
import struct
import sys
import unittest

HERE = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE.parent / 'tests'))

import z80   # noqa: E402

from spi_link_model import EspSide, SlaveHd, SpiPorts   # noqa: E402
from test_spi_link_z80 import frame, take_frames   # noqa: E402

BUILD = HERE / 'build'
BREAK = 0x01
SENTINEL, API, SNIPPET, ORIGA_RESULT, STACK = 0x5F00, 0x5F10, 0x5F20, 0x5FF0, 0x5E00
RES_PAGE, TOGGLE_PAGE = 0x61, 0x62
WINDOW, WINDO2 = 0x7EA0, 0x7EC4
DRACT, ORIGA, XPAN, STATUS, GATE, SELDEV_INV = 0x6868, 0x63ED, 0x6149, 0x8723, 0x5123, 0x5BE4
SPACE, ENTER, ESC = 0x10, 0x16, 0x17


def symbols(path):
    out = {}
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        if ': EQU ' in line:
            name, value = line.split(': EQU ')
            out[name.strip()] = int(value.strip(), 16)
    return out


def wmf_page(path):
    data = path.read_bytes()
    assert data[16:32] == b'WildCommanderMDL'
    sectors = data[37]
    page = bytearray(0x4000)
    code = data[512:512 + sectors * 512]
    page[:len(code)] = code
    return page, data


class Vdisk:
    """Служба VDISK прошивки 0.1.100 (zifi/vdisk_service.hpp): сектор тома — узор от LBA; в режиме 1
    (SD2 = карта Tang) — записанное WRITE, остальное — тот же узор."""

    def __init__(self):
        self.gen = 1
        self.reads = []
        self.read_modes = []            # байт режима READ: None — без него (том VFS)
        self.commands = []
        self.modes = []                 # payload MODE по порядку
        self.writes = []                # LBA записей
        self.card = {}
        self.raw = False
        self.fail_lba = set()
        self.no_service = False
        self.no_mode = False            # прошивка до 0.1.100: MODE и WRITE без ответа
        self.no_card = False            # MODE 1: карту не отдать (ядро без моста)
        self.flags = 1                  # INFO: 1 — том собран; 0.1.97/0.1.98 на плате давали 2
        self.max_payload = 0
        self.max_request = 0

    @staticmethod
    def sector(lba):
        return bytes((lba * 131 + i * 7 + (i >> 8)) & 255 for i in range(512))

    def __call__(self, cmd, p):
        self.commands.append(cmd)
        self.max_request = max(self.max_request, len(p))
        assert len(p) <= 1024, 'кадр Z80 -> ESP длиннее 1024'
        if self.no_service:
            reply = frame(0xEE, b'unsupported:%02X\0' % cmd)
        elif cmd in (0x18, 0x19) and self.no_mode:
            return b''                  # старая прошивка: неизвестная команда — без ответа
        elif cmd == 0x19:
            assert len(p) == 1 and p[0] <= 1, p
            self.modes.append(p[0])
            status = 0
            if p[0] == 1 and self.no_card:
                status = 4
            else:
                self.raw = p[0] == 1
            reply = frame(0x99, bytes([status, self.info_flags()]) + struct.pack('<I', 0x03B00000))
        elif cmd == 0x18:
            assert len(p) == 516, len(p)
            lba = struct.unpack_from('<I', p)[0]
            status = 3 if not self.raw else 2 if lba in self.fail_lba else 0
            if status == 0:
                self.card[lba] = bytes(p[4:])
                self.writes.append(lba)
            reply = frame(0x98, bytes([status]))
        elif cmd == 0x15:
            assert p == b''
            reply = frame(0x95, bytes([0, self.info_flags()]) + struct.pack('<II', self.gen, 0x08000000) + b'\1')
        elif cmd == 0x17:
            self.gen += 1
            reply = frame(0x97, b'\0' + struct.pack('<I', self.gen))
        elif cmd == 0x16:
            assert len(p) in (7, 8), p
            lba, ofs, n = struct.unpack('<IHB', p[:7])
            assert 1 <= n <= 248 and ofs + n <= 512
            self.reads.append((lba, ofs, n))
            self.read_modes.append(p[7] if len(p) == 8 else None)
            data = self.card.get(lba, self.sector(lba)) if self.raw else self.sector(lba)
            if len(p) == 8 and p[7] != (1 if self.raw else 0):
                reply = frame(0x96, b'\5')             # не тот режим
            else:
                reply = frame(0x96, b'\2' if lba in self.fail_lba else b'\0' + data[ofs:ofs + n])
        else:
            raise AssertionError(f'команда #{cmd:02X}')
        self.max_payload = max(self.max_payload, len(reply) - 5)
        assert len(reply) - 5 <= 256, 'кадр ESP -> Z80 длиннее 256'
        return reply

    def info_flags(self):
        return 0x41 if self.raw else self.flags


class Wc:
    """Z80 и память WC: страницы, окна, заглушки, канал до ESP."""

    def __init__(self, *, resident=True):
        self.res_sym = symbols(BUILD / 'VFSRES.sym')
        self.tog_sym = symbols(BUILD / 'VDAC4VFS.sym')
        self.m = z80.Z80Machine()
        self.mem = self.m.memory
        self.pages = {0x05: bytearray((BUILD / 'wc' / 'page05.bin').read_bytes()),
                      0x02: bytearray((BUILD / 'wc' / 'page02.bin').read_bytes())}
        p5 = self.pages[0x05]
        p5[0x2006:0x2009] = bytes([0xC3, API & 255, API >> 8])   # API WC -> заглушка
        p5[API - 0x4000] = 0xC9
        snippet = bytes([0xCD, 0x03, 0x38,                        # CALL DEVINI
                         0x32, ORIGA_RESULT & 255, ORIGA_RESULT >> 8,
                         0x21, 0, 0, 0x11, 0, 0, 0xCD, 0x09, 0x38,  # XPOZI 0
                         0x21, 0x00, 0x32, 0x3E, 0x01, 0xCD, 0x0F, 0x38,   # RDDSE 1 -> #3200
                         0x32, (ORIGA_RESULT + 1) & 255, (ORIGA_RESULT + 1) >> 8,
                         0xC9])
        p5[SNIPPET - 0x4000:SNIPPET - 0x4000 + len(snippet)] = snippet
        for page in (0xF0, 0xF1):
            self.page(page)[0x38:0x3A] = bytes([0xFB, 0xC9])     # IM1: EI, RET
        self.res_page, self.res_file = wmf_page(BUILD / 'VFSRES.WMF')
        if resident:
            self.pages[RES_PAGE] = bytearray(self.res_page)
        self.tog_page, self.tog_file = wmf_page(BUILD / 'VDAC4VFS.WMF')
        self.pages[TOGGLE_PAGE] = bytearray(self.tog_page)
        self.window = [0xF0, 0x05, 0x02, 0x00]
        for w in range(4):
            self.mem[w * 0x4000:(w + 1) * 0x4000] = self.page(self.window[w])
        self.shadow_all()
        self.m.set_input_callback(self.port_in)
        self.m.set_output_callback(self.port_out)
        for address in (SENTINEL, API, DRACT, ORIGA, XPAN, STATUS, GATE, SELDEV_INV):
            self.m.set_breakpoint(address)
        self.hooks = {}
        self.calls = []
        self.api = []
        self.keys = []                  # очередь нажатий: ENTER #16, ESC #17, SPACE #10
        self.interrupts = 0
        self.ticks = 0
        # канал SPI и «прошивка»
        self.slave = SlaveHd()
        self.link = EspSide(self.slave)
        self.ports = SpiPorts(self.slave)
        self.ports.memory = self.mem
        self.link.rx = bytearray()
        self.link.tx = bytearray()
        self.vdisk = Vdisk()
        self.reply_delay = 0                       # чтений буфера до ответа службы
        self.held = []
        self._stream = bytearray()
        self.slave.on_read = self._read_started
        self.slave.on_written = self._written
        # окно #8000 — WC; выбор панелей
        for base, page in ((WINDOW, 0), (WINDO2, 1)):
            self.poke(base + 0x1B, page)
            self.poke(base + 0x1C, 1)
            self.poke(base + 0x1D, 1)

    # --- память -------------------------------------------------------------------------------
    def page(self, n):
        return self.pages.setdefault(n, bytearray(0x4000))

    def flush(self):
        for w in range(4):
            self.page(self.window[w])[:] = self.mem[w * 0x4000:(w + 1) * 0x4000]

    def map(self, w, n):
        self.page(self.window[w])[:] = self.mem[w * 0x4000:(w + 1) * 0x4000]
        self.window[w] = n
        self.mem[w * 0x4000:(w + 1) * 0x4000] = self.page(n)

    def shadow_all(self):
        for w in range(4):
            self.mem[0x6000 + w] = self.window[w]

    def peek(self, a):
        return self.mem[a]

    def poke(self, a, v):
        self.mem[a] = v & 255

    def word(self, a):
        return self.mem[a] | self.mem[a + 1] << 8

    # --- порты --------------------------------------------------------------------------------
    def port_in(self, port):
        if port & 0xFF == 0xAF and 0x10 <= port >> 8 <= 0x13:
            return self.window[(port >> 8) - 0x10]
        value = self.ports.read(port)
        return 0xFF if value is None else value

    def port_out(self, port, value):
        if port & 0xFF == 0xAF and 0x10 <= port >> 8 <= 0x13:
            self.map((port >> 8) - 0x10, value)
        self.ports.write(port, value)

    def _run_app(self):
        self.link.pump()
        self._stream += self.link.tx
        self.link.tx.clear()
        for command, payload in take_frames(self._stream):
            reply = self.vdisk(command, payload)
            if not reply:
                continue
            if self.reply_delay:
                self.held.append([self.reply_delay, reply])   # «прошивка» собирает ответ долго
            else:
                self.link.rx += reply
        self.link.pump()

    def _written(self):
        self.link._written()
        self._run_app()

    def _read_started(self, address):
        for item in self.held:
            item[0] -= 1
        while self.held and self.held[0][0] <= 0:
            self.link.rx += self.held.pop(0)[1]
        self._run_app()
        self.link._read_started(address)

    # --- исполнение ---------------------------------------------------------------------------
    def ret(self):
        m = self.m
        m.pc = self.word(m.sp)
        m.sp = (m.sp + 2) & 0xFFFF

    def stub(self):
        m = self.m
        pc = m.pc
        if pc == API:
            fn = m.a
            self.api.append(fn)
            m.f &= ~0x40
            if fn in (0x10, 0x16, 0x17):            # SPACE, ENTER, ESC: NZ — нажата
                if self.keys and self.keys[0] == fn:
                    self.keys.pop(0)
                else:
                    m.f |= 0x40
            m.step_over_breakpoint()
            return
        if pc == DRACT:
            ix = m.ix
            self.calls.append(('DRACT', ix))
            page = 0xF0 + self.peek(ix + 0x1B)
            self.map(0, page)
            self.poke(0x6000, page)
            if self.peek(ix + 0x1C) == 0:
                self.poke(ix + 0x1C, 1)
                m.a = self.peek(ix + 0x1D)
                m.pc = 0x4FF8                      # JP #401B: DOS_SWP, RET — к вызвавшему DRACT
                return
            self.ret()
            return
        if pc == ORIGA:
            self.calls.append(('ORIGA', m.ix))
            if self.mem[0x3800:0x3803] == bytes([0xC3]) + self.res_sym['D_SELDEV'].to_bytes(2, 'little'):
                self.poke(ORIGA_RESULT, 0xEE)
                self.poke(ORIGA_RESULT + 1, 0xEE)
                m.pc = SNIPPET                     # DEVINI и сектор 0 через наш драйвер
                return
            self.ret()
            return
        if pc in (XPAN, STATUS):
            self.calls.append(('XPAN' if pc == XPAN else 'STATUS', m.ix))
            self.ret()
            return
        if pc == GATE:                              # шлюз расширения: «распаковать штатный SD-ZC»
            self.calls.append(('GATE', self.peek(self.word(m.sp))))
            self.mem[0x3800:0x3804] = b'SDZC'
            m.pc = (self.word(m.sp) + 1) & 0xFFFF
            m.sp = (m.sp + 2) & 0xFFFF
            return
        if pc == SELDEV_INV:
            self.calls.append(('SELDEV', m.a))
            self.ret()
            return
        if pc in self.hooks:
            self.hooks[pc]()
            m.step_over_breakpoint()
            return
        raise AssertionError(f'точка #{pc:04X}')

    def run(self, pc, *, budget=400_000_000, **regs):
        m = self.m
        sp = STACK - 2
        self.mem[sp] = SENTINEL & 255
        self.mem[sp + 1] = SENTINEL >> 8
        m.sp = sp
        m.pc = pc
        for name, value in regs.items():
            setattr(m, name, value)
        spent = 0
        while spent < budget:
            m.ticks_to_stop = 2_000_000
            event = m.run()
            spent += 2_000_000 - m.ticks_to_stop
            if event & BREAK and m.pc == SENTINEL:
                self.ticks += spent
                return
            if m.halted:
                self.interrupts += 1
                m.on_handle_active_int()
                continue
            if event & BREAK:
                self.stub()
        raise AssertionError('Z80 не вернулся')

    # --- сценарии -----------------------------------------------------------------------------
    def install_resident(self):
        self.map(2, RES_PAGE)
        self.poke(0x6002, RES_PAGE)
        self.run(0x8000, a=0x06)
        self.map(2, 0x02)
        self.poke(0x6002, 0x02)

    def toggle(self, choice=None):
        """choice — клавиша окна выбора при включении: ENTER (карта Tang), SPACE (VFS), ESC."""
        self.map(2, TOGGLE_PAGE)
        self.poke(0x6002, TOGGLE_PAGE)
        self.keys = ([choice] if choice else []) + [ESC]   # и Esc — закрыть окно итога
        self.run(0x8000, a=0x03, ix=WINDOW)
        assert self.keys == [], ('не все клавиши взяты', self.keys)
        assert self.window[2] == TOGGLE_PAGE and self.peek(0x6002) == TOGGLE_PAGE, 'окно #8000 не вернулось плагину'
        self.flush()
        text = self.page(TOGGLE_PAGE)
        # строки окна: итог, панель, диагностика драйвера, доклад ESP, самопроверка резидента
        msg = tuple(bytes(text[self.tog_sym[name] - 0x8000:self.tog_sym[name] - 0x8000 + 44]).decode().strip()
                    for name in ('T_Line1', 'T_Line2', 'T_Line3', 'T_Line4', 'T_Line5'))
        self.map(2, 0x02)
        self.poke(0x6002, 0x02)
        return msg

    def core_bytes(self, a, n):
        self.flush()
        return bytes(self.page(0x05)[a - 0x4000:a - 0x4000 + n])


ORIGINAL = bytes(16) + bytes.fromhex('06002176 51B7281F 215D533D 28192101 563D2813 2133593D 280D043D '
                                     '28092176 513D2803 215D53DD E5110038 CD235119 CDE45B00 DDE1C9'.replace(' ', ''))


class Vfs(unittest.TestCase):
    def setUp(self):
        for name in ('VFSRES.WMF', 'VDAC4VFS.WMF', 'wc/page05.bin'):
            if not (BUILD / name).exists():
                raise unittest.SkipTest('нет сборки: ' + name)

    def installed(self, wc):
        msg = wc.toggle(SPACE)
        self.assertEqual(msg[0], 'SD2 = VDAC4 VFS (ESP: TANG = Tang microSD)', msg)
        return msg

    def test_resident_init(self):
        wc = Wc()
        wc.install_resident()
        wc.flush()
        p = wc.page(RES_PAGE)
        self.assertEqual(bytes(p[0x100:0x108]), b'VDAC4VFS')
        self.assertEqual((p[0x109], p[0x10A]), (RES_PAGE, 0))
        self.assertEqual(p[0x3000 + wc.res_sym['D_PAGE'] - 0x3800], RES_PAGE)
        self.assertEqual(int.from_bytes(p[0x10B:0x10D], 'little'), wc.res_sym['RES_SWAP'])

    def test_toggle_on_remounts_sd2_and_off_restores(self):
        wc = Wc()
        wc.install_resident()
        wc.poke(WINDOW + 0x1D, 6)                  # левая панель — на SD2
        self.assertEqual(wc.core_bytes(0x4FE8, len(ORIGINAL)), ORIGINAL)
        msg = self.installed(wc)
        # диагностика: драйвер ещё не работал; самопроверка резидента прошла BEGIN и INFO
        self.assertTrue(msg[2].startswith('DRV SW0000 IO0000 BEGIN OK'), msg)
        self.assertEqual(msg[3], '', msg)
        self.assertRegex(msg[4], r'^SELF SW0000 IO000[2-9A-F] INFO OK W0000 R0[13]$')
        tramp = wc.core_bytes(0x4FE8, 16)
        swap = wc.res_sym['RES_SWAP']
        self.assertEqual(tramp, bytes([0xED, 0x57, 0xF3, 0xF5, 0x01, 0xAF, 0x13, 0xED, 0x58, 0x3E, RES_PAGE,
                                       0xED, 0x79, 0xC3, swap & 255, swap >> 8]))
        self.assertEqual(wc.core_bytes(0x501C, 3), bytes([0xC3, 0xE8, 0x4F]))
        self.assertEqual(wc.page(RES_PAGE)[0x10A], 1)
        # старт WC (карту Tang — ESP), самопроверка, проверка прошивки, сброс, режим VFS, DEVINI
        self.assertEqual([c for c in wc.vdisk.commands if c != 0x16], [0x19, 0x15, 0x15, 0x17, 0x19, 0x15])
        self.assertEqual(wc.vdisk.modes, [0, 0])
        # перелёт: копия в странице #02, перемонтирована только панель на SD2
        r_start, r_len = wc.tog_sym['R_START'], wc.tog_sym['R_LEN']
        copy = bytes(wc.page(0x02)[r_start - 0x8000:r_start - 0x8000 + r_len])
        self.assertEqual(copy[:wc.tog_sym['R_BACK'] + 1 - r_start],
                         bytes(wc.page(TOGGLE_PAGE)[r_start - 0x8000:wc.tog_sym['R_BACK'] + 1 - 0x8000]))
        self.assertEqual(copy[wc.tog_sym['R_BACK'] + 1 - r_start], TOGGLE_PAGE)
        self.assertEqual(wc.calls[:3], [('DRACT', WINDOW), ('SELDEV', 0), ('ORIGA', WINDOW)])
        self.assertIn(('STATUS', WINDOW), wc.calls)
        self.assertEqual([c for c in wc.calls if c[0] == 'DRACT'], [('DRACT', WINDOW), ('DRACT', WINDOW)])
        # драйвер — в странице левой панели; DEVINI и сектор 0 через него
        image = bytes(wc.res_page[0x3000:0x3000 + wc.res_sym['D_END'] - 0x3800])
        panel = wc.page(0xF0)
        self.assertEqual(bytes(panel[0x3800:0x3800 + 9]), image[:9])
        self.assertEqual(panel[0x3800 + wc.res_sym['D_PAGE'] - 0x3800], RES_PAGE)
        self.assertEqual((wc.page(0x05)[ORIGA_RESULT - 0x4000], wc.page(0x05)[ORIGA_RESULT + 1 - 0x4000]), (0, 0))
        self.assertEqual(bytes(panel[0x3200:0x3400]), Vdisk.sector(0))
        self.assertLessEqual(wc.vdisk.max_payload, 249)
        self.assertEqual(wc.window[3], 0x00)
        # выключение: эталонные байты, SD2 снова штатный
        wc.calls.clear()
        msg = wc.toggle()
        self.assertEqual(msg[0], 'SD2 = second SD card again')
        # в записи — работа драйвера при перемонтировании: образ ставился, последнее — чтение сектора
        self.assertRegex(msg[2], r'^DRV SW000[1-9] IO[0-9A-F]{4} READ OK W0000 R')
        self.assertEqual(wc.core_bytes(0x4FE8, len(ORIGINAL)), ORIGINAL)
        self.assertEqual(wc.page(RES_PAGE)[0x10A], 0)
        self.assertEqual(wc.calls[:3], [('DRACT', WINDOW), ('GATE', 0x19), ('SELDEV', 1)])
        self.assertEqual(bytes(wc.page(0xF0)[0x3800:0x3804]), b'SDZC')

    def test_dos_swp_keeps_pages_ix_and_interrupts(self):
        for iff in (0, 1):
            wc = Wc()
            wc.install_resident()
            self.installed(wc)
            wc.map(3, 0x33)
            wc.poke(0x6003, 0x33)
            wc.map(0, 0xF1)
            wc.m.iff1 = wc.m.iff2 = iff
            wc.calls.clear()
            wc.run(0x4FF8, a=6, ix=0x1234)
            self.assertEqual(wc.m.ix, 0x1234)
            self.assertEqual(wc.window[3], 0x33)
            self.assertEqual((wc.m.iff1, wc.m.iff2), (iff, iff))
            self.assertEqual(wc.calls, [('SELDEV', 0)])
            wc.flush()
            image = bytes(wc.res_page[0x3000:0x3000 + wc.res_sym['D_END'] - 0x3800])
            got = bytes(wc.page(0xF1)[0x3800:0x3800 + len(image)])
            self.assertEqual(got[:wc.res_sym['D_PAGE'] - 0x3800], image[:wc.res_sym['D_PAGE'] - 0x3800])

    def driver(self, *, delay=0):
        wc = Wc()
        wc.install_resident()
        self.installed(wc)
        wc.map(0, 0xF1)
        wc.run(0x4FF8, a=6, ix=0)
        wc.reply_delay = delay
        wc.vdisk.reads.clear()
        return wc

    def test_reads_to_any_window_with_slow_esp(self):
        wc = self.driver(delay=5000)                # ответа ждать дольше одного вызова RES_IO
        waits = []
        wc.hooks[wc.res_sym['D_CALL']] = lambda: waits.append(wc.m.a)
        wc.m.set_breakpoint(wc.res_sym['D_CALL'])
        wc.map(3, 0x44)
        wc.run(0x3803)                              # DEVINI
        self.assertEqual(wc.m.f & 0x40, 0x40)
        wc.run(0x3809, hl=1000 & 0xFFFF, de=0)      # XPOZI
        wc.m.iff1 = wc.m.iff2 = 1
        wc.run(0x380F, a=3, hl=0xC100)              # RDDSE: 3 сектора в окно #C000
        self.assertEqual(wc.m.f & 0x40, 0x40)
        self.assertEqual(wc.m.hl, 0xC100 + 3 * 512)
        self.assertEqual(wc.window[3], 0x44)
        self.assertEqual((wc.m.iff1, wc.m.iff2), (1, 1))
        wc.flush()
        self.assertEqual(bytes(wc.page(0x44)[0x100:0x100 + 1536]),
                         Vdisk.sector(1000) + Vdisk.sector(1001) + Vdisk.sector(1002))
        self.assertEqual(wc.vdisk.reads, [(lba, ofs, n) for lba in (1000, 1001, 1002)
                                          for ofs, n in ((0, 248), (248, 248), (496, 16))])
        self.assertEqual(set(wc.vdisk.read_modes), {None})   # том VFS — запросы как у 0.1.99
        self.assertEqual(int.from_bytes(wc.mem[0x38D0:0x38D4], 'little'), 1003)
        self.assertEqual(wc.peek(0x384A), 0)
        self.assertIn(3, waits, 'медленная ESP: драйвер ждал повторными RES_WAIT')

    def test_source_failure_and_old_firmware(self):
        wc = self.driver()
        wc.vdisk.fail_lba = {7}
        wc.run(0x3809, hl=7, de=0)
        wc.run(0x380F, a=1, hl=0xC000)
        self.assertEqual(wc.m.f & 0x40, 0)          # NZ
        self.assertEqual(wc.peek(0x384A), 1)        # ABT
        wc.vdisk.no_service = True
        start = wc.ticks
        wc.run(0x3803)
        self.assertEqual(wc.m.f & 0x40, 0)
        self.assertLess(wc.ticks - start, 50_000_000, 'отказ прошивки — без долгого ожидания')

    def test_empty_volume_refused(self):
        # Как на плате с 0.1.98: INFO с итогом 0, но без флага «том собран» — источник TANG не
        # попал в прошивку. Переключатель не ставит драйвер, самопроверка показывает NOTREADY.
        wc = Wc()
        wc.install_resident()
        wc.vdisk.flags = 0x02
        msg = wc.toggle(SPACE)
        self.assertEqual(msg[0], 'ESP VFS is empty: needs firmware 0.1.99+', msg)
        self.assertRegex(msg[4], r'^SELF SW0000 IO0002 INFO NOTREADY W0000 R02$')
        self.assertEqual(wc.core_bytes(0x4FE8, len(ORIGINAL)), ORIGINAL)
        self.assertEqual(wc.page(RES_PAGE)[0x10A], 0)

    def test_refusals(self):
        wc = Wc()
        wc.install_resident()
        wc.flush()
        wc.page(0x05)[0x5000 - 0x4000] ^= 1          # чужая сборка WC
        wc.mem[0x5000] ^= 1
        self.assertEqual(wc.toggle()[0], 'Needs Wild Commander Improved v1.11i')
        self.assertEqual(wc.core_bytes(0x4FE8, 16), bytes(16))
        wc2 = Wc(resident=False)
        self.assertEqual(wc2.toggle()[0], 'VFSRES.WMF not loaded: add it to [PLUGINS]')
        self.assertEqual(wc2.core_bytes(0x501C, 3), bytes([0x21, 0x5D, 0x53]))
        wc3 = Wc()
        wc3.install_resident()
        wc3.vdisk.no_service = True
        self.assertEqual(wc3.toggle(SPACE)[0], 'ESP has no VFS: needs firmware vdac4-0.1.99')
        self.assertEqual(wc3.core_bytes(0x501C, 3), bytes([0x21, 0x5D, 0x53]))
        wc4 = Wc()
        wc4.install_resident()
        wc4.poke(WINDO2 + 0x1D, 6)
        wc4.map(2, 0x02)
        wc4.mem[0xAB60] ^= 0x55                       # область #02 кто-то занял
        msg = self.installed(wc4)
        self.assertEqual(msg[1], 'Choose SD2 again in the drive menu')
        self.assertNotIn('DRACT', [c[0] for c in wc4.calls])

    # --- SD2 = карта Tang с записью (0.1.100) ----------------------------------------------------
    def tang_driver(self):
        wc = Wc()
        wc.install_resident()
        msg = wc.toggle(ENTER)
        self.assertEqual(msg[0], 'SD2 = Tang microSD card, read/write', msg)
        self.assertEqual(wc.page(RES_PAGE)[0x10A], 2)
        self.assertEqual(wc.vdisk.modes, [0, 1])
        wc.map(0, 0xF1)
        wc.run(0x4FF8, a=6, ix=0)
        return wc

    def test_tang_mode_writes_and_reads_back(self):
        wc = self.tang_driver()
        data = bytes((i * 37 + 5) & 255 for i in range(1024))
        wc.map(3, 0x44)
        wc.flush()
        wc.page(0x44)[0x100:0x100 + 1024] = data
        wc.mem[0xC100:0xC100 + 1024] = data
        wc.run(0x3809, hl=5000, de=0)               # XPOZI
        wc.m.iff1 = wc.m.iff2 = 1
        wc.run(0x3812, a=2, hl=0xC100)              # SDDSE: 2 сектора из окна #C000
        self.assertEqual(wc.m.f & 0x40, 0x40)       # Z — да
        self.assertEqual(wc.peek(0x384A), 0)
        self.assertEqual(wc.m.hl, 0xC100 + 1024)
        self.assertEqual((wc.window[3], wc.m.iff1), (0x44, 1))
        self.assertEqual(int.from_bytes(wc.mem[0x38D0:0x38D4], 'little'), 5002)
        self.assertEqual(wc.vdisk.writes, [5000, 5001])
        self.assertEqual(wc.vdisk.card[5000] + wc.vdisk.card[5001], data)
        self.assertEqual(wc.vdisk.max_request, 516)
        # читается записанное
        wc.run(0x3809, hl=5000, de=0)
        wc.run(0x380F, a=2, hl=0xC800)
        wc.flush()
        self.assertEqual(bytes(wc.page(0x44)[0x800:0x800 + 1024]), data)
        self.assertEqual(set(wc.vdisk.read_modes), {1})
        # ESP перезагрузилась (режим снова 0): чтение — отказ, а не сектор чужого тома
        wc.vdisk.raw = False
        wc.run(0x3809, hl=5000, de=0)
        wc.run(0x380F, a=1, hl=0xC800)
        self.assertEqual((wc.m.f & 0x40, wc.peek(0x384A)), (0, 1))
        wc.vdisk.raw = True
        # ошибка карты — ABT
        wc.vdisk.fail_lba = {6000}
        wc.run(0x3809, hl=6000, de=0)
        wc.run(0x3812, a=1, hl=0xC100)
        self.assertEqual((wc.m.f & 0x40, wc.peek(0x384A)), (0, 1))
        # выключение: карта Tang — снова ESP
        msg = wc.toggle()
        self.assertEqual(msg[0], 'SD2 = second SD card again', msg)
        self.assertEqual(wc.vdisk.modes, [0, 1, 0])
        self.assertEqual(wc.page(RES_PAGE)[0x10A], 0)
        self.assertEqual(wc.core_bytes(0x4FE8, len(ORIGINAL)), ORIGINAL)

    def test_vfs_mode_refuses_writes(self):
        wc = self.driver()
        self.assertEqual(wc.page(RES_PAGE)[0x10A], 1)
        wc.run(0x3809, hl=10, de=0)
        wc.run(0x3812, a=1, hl=0xC000)
        self.assertEqual((wc.m.f & 0x40, wc.peek(0x384A)), (0, 1))
        self.assertEqual(wc.vdisk.writes, [])
        self.assertRegex(wc.toggle()[2], r'^DRV .* WRITE RESULT W0000 R03$')

    def test_choice_cancel_and_tang_refusals(self):
        wc = Wc()
        wc.install_resident()
        self.assertEqual(wc.toggle(ESC)[0], 'SD2 left as it was')
        self.assertEqual(wc.core_bytes(0x4FE8, 16), bytes(16))
        self.assertEqual(wc.vdisk.modes, [0])           # только освобождение при старте WC
        wc.vdisk.no_card = True
        self.assertEqual(wc.toggle(ENTER)[0], 'No Tang card: needs TS-Config 1.30+ and SD')
        self.assertEqual(wc.core_bytes(0x501C, 3), bytes([0x21, 0x5D, 0x53]))
        wc2 = Wc()
        wc2.vdisk.no_mode = True                         # прошивка 0.1.99
        start = wc2.ticks
        wc2.install_resident()
        self.assertLess(wc2.ticks - start, 80_000_000, 'старт WC без MODE не ждёт долго')
        self.assertEqual(wc2.toggle(ENTER)[0], 'SD2 = Tang card needs ESP firmware 0.1.100')
        self.assertEqual(wc2.page(RES_PAGE)[0x10A], 0)
        self.assertEqual(self.installed(wc2)[0], 'SD2 = VDAC4 VFS (ESP: TANG = Tang microSD)')

    def test_release_without_answer_still_switches_off(self):
        wc = self.tang_driver()
        wc.vdisk.no_mode = True
        msg = wc.toggle()
        self.assertEqual(msg[0], 'SD2 normal; ESP did not answer the release', msg)
        self.assertEqual(wc.page(RES_PAGE)[0x10A], 0)
        self.assertEqual(wc.core_bytes(0x4FE8, len(ORIGINAL)), ORIGINAL)

    def test_resident_start_releases_tang_card(self):
        wc = Wc()
        wc.vdisk.raw = True                              # прошлый сеанс WC оставил карту у себя
        wc.m.iff1 = wc.m.iff2 = 1
        wc.install_resident()
        self.assertEqual(wc.vdisk.modes, [0])
        self.assertFalse(wc.vdisk.raw)
        self.assertEqual((wc.m.iff1, wc.m.iff2), (1, 1))
        self.assertEqual(wc.window[3], 0x00)


if __name__ == '__main__':
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Vfs))
    sys.exit(not result.wasSuccessful())
