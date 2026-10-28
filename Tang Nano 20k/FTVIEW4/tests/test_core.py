"""Банк ядра видео VDAC4 (страница 11 FTVIEW4.WMF, src/core.c) на модели Z80.

Плагин исполняется процессором; WC API, SD (LOAD512, GIPAG, CURIT, каталог) и функции SDK FT812 —
модели test_ftview.Machine. За портами #57/#77, пока выбран ESP (#13), — модель канала SPI платы
VDAC4 (spi_link_model из тестов Network проекта VDAC4: общий буфер ведомого ESP и прошивка обмена)
и модель приложения VIDEO.ELF (команды #2D/#6D прошивки и #70…#78 слоя ядра видео); за SPI FT812
— модель ядра видео: REG_ID, REG_CPURESET (бит 0 — сброс потока и новая эпоха, бит 2 — пауза),
медиа-FIFO 32 КиБ (окно #E0000, REG_MEDIAFIFO_READ/WRITE), блок состояния «VID1»; разбор AVI:
слоты — видеочанки потока, пока нет паузы.

Проверяется:
- нет ESP — возврат в банк изображений без обмена с ядром; нет VIDEO.ELF (CORE_APP запустил
  общий слой) и ядро не встало (STATUS 255) — APP_STOP и возврат; шлюз core_try переносит кадр
  банка изображений в банк ядра;
- ролик целиком: CORE_APP и START «VIDEO», STATUS до «работает», режим FT812 (GVmod #87), REG_ID
  и «VID1»; HDR — ровно заголовок до movi; REG_CPURESET 1 до HDR, STATUS с началом потока, 0 —
  перед подачей; поток — ровно файл до конца movi; STATUS с телом не реже раза в 6 тиков WC; в
  конце — список (сначала «Reading folder...», затем строки, имена в CP866), Esc в списке, Esc в
  плеере — EXIT, слот 0, APP_STOP, текст WC (API 80 и MNGV_PL);
- пауза: Space — REG_CPURESET 4 и флаг паузы в STATUS, слоты стоят; Space — 0;
- перемотка: Right — «Building seek index...», таблица по idx1 в страницах 12…15, поток с
  записи: заголовок до movi, JUNK до позиции записи, файл с неё; STATUS — кадр и байты звука
  записи, совпадающие с файлом;
- список: Enter на другом ролике — его HDR и поток с начала.

Путь к тестам Network VDAC4 — переменная VDAC4_NETWORK_TESTS; без них тесты пропускаются.
"""
import os
import random
import struct
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
NETWORK_TESTS = Path(os.environ.get('VDAC4_NETWORK_TESTS',
                                    str(HERE.parents[2] / 'ESP32 S3 Zero N8R8' / 'Network' / 'tests')))
sys.path.insert(0, str(NETWORK_TESTS))

import avigen  # noqa: E402
import test_ftview  # noqa: E402
from test_ftview import Machine, list_volume  # noqa: E402

try:
    from spi_link_model import EspSide, SlaveHd, SpiPorts  # noqa: E402
except ImportError:  # нет проекта VDAC4 рядом
    EspSide = None

API_SPKE, API_RGGG, API_ENKE, API_ESC, API_DWWW = 0x10, 0x14, 0x16, 0x17, 0x12


ELF_FILE = bytes(range(256)) * 5 + b'VIDEO.ELF end'   # 1293 байта: три сектора, последний неполный
ELF_TIME, ELF_DATE = 0x6B2A, 0x5B5C
STREAM_PAGE = {0: 0xF0, 1: 0xF1, 'wc': 0xF5}               # страницы WildDOS потоков WC


def frame(command, payload=b''):
    check = command ^ (len(payload) & 0xFF) ^ (len(payload) >> 8)
    for b in payload:
        check ^= b
    return bytes([0x5A, command, len(payload) & 0xFF, len(payload) >> 8]) + payload + bytes([check])


def take_frames(stream):
    out = []
    while len(stream) >= 5:
        assert stream[0] == 0x5A, 'кадр должен начинаться с SYNC'
        n = stream[2] | stream[3] << 8
        if len(stream) < 5 + n:
            break
        check = 0
        for b in stream[1:4 + n]:
            check ^= b
        assert stream[4 + n] == check, 'контрольная сумма кадра'
        out.append((stream[1], bytes(stream[4:4 + n])))
        del stream[:5 + n]
    return out


class AviParser:
    """Разбор потока, как vp_avi ядра: RIFF, LIST — внутрь, прочие чанки — мимо; кадр — ##dc/##db."""

    def __init__(self):
        self.buf, self.state, self.skip, self.frames, self.ids = bytearray(), 'riff', 0, 0, []

    def feed(self, b):
        if self.skip:
            self.skip -= 1
            return
        self.buf.append(b)
        if self.state == 'riff' and len(self.buf) == 12:
            assert self.buf[:4] == b'RIFF' and self.buf[8:12] == b'AVI ', bytes(self.buf)
            self.buf.clear()
            self.state = 'chunk'
        elif self.state == 'chunk' and len(self.buf) == 8:
            cid, size = bytes(self.buf[:4]), int.from_bytes(self.buf[4:8], 'little')
            self.buf.clear()
            if cid == b'LIST':
                self.state = 'list'
            else:
                self.ids.append(cid)
                if cid[2:4] in (b'dc', b'db'):
                    self.frames += 1
                self.skip = size + (size & 1)
        elif self.state == 'list' and len(self.buf) == 4:
            self.buf.clear()
            self.state = 'chunk'


class VideoCore:
    """Ядро видео на SPI FT812: регистры хоста, медиа-FIFO и блок состояния (docs/video-core.md).

    fifo — размер медиа-FIFO (ядро до 1.38 — 32 КиБ, 1.38 — 2 МБ в SDRAM); drain — сколько байтов
    разбор берёт за чтение регистра (None — всё сразу): медленный разбор копит FIFO."""
    FIFO = 32768

    def __init__(self, fifo=None, drain=None):
        if fifo:
            self.FIFO = fifo
        self.drain = drain
        self.creset = self.pause = self.epoch = 0
        self.fifo = bytearray(self.FIFO)
        self.wr = self.rd = self.slots = 0
        self.max_level = 0                 # больше всего байтов в FIFO
        self.streams = [bytearray()]       # принятое разбором в каждой эпохе
        self.parser = AviParser()
        self.cpureset = []                 # записи REG_CPURESET

    def write(self, addr, data):
        if addr == 0x302020:
            v = data[0]
            self.cpureset.append(v)
            if v & 1 and not self.creset:
                self.epoch += 1
                self.streams.append(bytearray())
                self.parser = AviParser()
                self.rd = self.wr = self.slots = 0
            self.creset, self.pause = v & 1, v >> 2 & 1
        elif addr == 0x309018:
            if not self.creset:
                self.wr = int.from_bytes(data, 'little') & (self.FIFO - 1)
        elif 0xE0000 <= addr < 0xE0000 + self.FIFO:
            for i, b in enumerate(data):
                self.fifo[(addr - 0xE0000 + i) & (self.FIFO - 1)] = b
        else:
            raise AssertionError(('запись мимо ядра видео', hex(addr), len(data)))

    def level(self):
        return (self.wr - self.rd) & (self.FIFO - 1)

    def consume(self):
        self.max_level = max(self.max_level, self.level())
        n = self.drain if self.drain is not None else self.FIFO
        while self.rd != self.wr and not self.creset and n:
            n -= 1
            b = self.fifo[self.rd]
            self.rd = (self.rd + 1) & (self.FIFO - 1)
            self.streams[-1].append(b)
            self.parser.feed(b)
        if not self.pause and not self.creset:
            self.slots = self.parser.frames

    def read(self, addr, n):
        self.consume()
        if addr == 0x302000:
            v = 0x7C
        elif addr == 0x309014:
            v = self.rd
        elif addr == 0x302020:
            v = self.creset | self.pause << 2
        elif addr < 0x40 and not addr & 3:
            words = [0x31444956, self.FIFO, self.slots, self.parser.frames, 0, 0, 0,
                     (self.rd == self.wr) << 2 | self.pause << 3 | self.creset]
            v = words[addr // 4] if addr // 4 < len(words) else 0
        else:
            raise AssertionError(('чтение мимо ядра видео', hex(addr), n))
        return v.to_bytes(4, 'little')[:n]


class VideoElf:
    """Прошивка ESP и приложение VIDEO.ELF (Cores/VIDEO/esp/main/app.c): команды и ответы."""

    def __init__(self, *, app='video', load_polls=3, core_error=False, cached=True):
        self.app_name, self.load_polls, self.core_error = app, load_polls, core_error
        # кэш модулей ESP: (имя, длина, время FAT); True — там уже то, что спросят первым
        self.cached = cached
        self.queries, self.puts, self.app_starts = [], 0, []
        self.delivery = self.stored = None
        self.app = None
        self.state = 0
        self.exit_seen = False
        self.commands, self.status, self.lists, self.msgs, self.rows_frames = [], [], [], [], []
        self.hdrs = []                     # заголовки, по ролику
        self.start = None
        self.machine = None

    def __call__(self, cmd, payload):
        self.commands.append(cmd)
        if cmd == 0x69:                    # MOD_QUERY [флаги][длина][время FAT][имя, 0]
            flags, size, stamp = struct.unpack_from('<BII', payload)
            key = (payload[9:].split(b'\0')[0].decode(), size, stamp)
            self.queries.append((flags,) + key)
            if self.cached is True:
                self.cached = key
            if self.cached == key:
                return frame(0xE9, b'\0' + struct.pack('<I', 1 << 20))
            self.delivery, self.delivery_key = bytearray(), key
            return frame(0xE9, b'\1' + struct.pack('<I', 1 << 20))
        if cmd == 0x6A:                    # MOD_PUT [смещение][данные]
            off = struct.unpack_from('<I', payload)[0]
            assert self.delivery is not None and off == len(self.delivery), ('MOD_PUT не по порядку', off)
            self.delivery += payload[4:]
            self.puts += 1
            return frame(0xEA, b'\1' + struct.pack('<I', len(self.delivery)))
        if cmd == 0x6B:                    # MOD_COMMIT
            ok = self.delivery is not None and len(self.delivery) == self.delivery_key[1]
            if ok:
                self.cached, self.stored = self.delivery_key, bytes(self.delivery)
            self.delivery = None
            return frame(0xEB, b'\1' if ok else b'\0')
        if cmd == 0x6C:                    # APP_START [имя, 0]
            name = payload.split(b'\0')[0].decode()
            self.app_starts.append(name)
            if not isinstance(self.cached, tuple) or self.cached[0] != name:
                return frame(0xEC, b'\xfc\0module not in the cache\0')
            self.app = self.app_name
            return frame(0xEC, b'\0' + self.app_name.encode() + b'\0' + b'0.1\0')
        if cmd == 0x6D:
            self.app = None
            return frame(0xED, b'\1')
        if self.app is None:
            return frame(0xEE, b'no app')
        if cmd == 0x70:
            self.start = payload
            self.state = 1
            return frame(0xF0, b'\1')
        if cmd == 0x72:
            if len(payload) >= 18:
                self.status.append((self.machine.ticks(), self.machine.core.epoch, payload))
            if self.state == 1:
                self.load_polls -= 1
                if self.load_polls <= 0:
                    self.state = 255 if self.core_error else 2
            elif self.state == 2 and self.exit_seen:
                self.state = 3
            return frame(0xF2, bytes([self.state]) + struct.pack('<IIHB', 0x05010001, 0, 0, 0) + b'\0')
        if cmd == 0x73:
            self.exit_seen = True
            return frame(0xF3, b'\1')
        if cmd == 0x76:
            off, last = struct.unpack_from('<IB', payload)
            if off == 0:
                self.hdrs.append(bytearray())
            assert off == len(self.hdrs[-1]), ('HDR не по порядку', off, len(self.hdrs[-1]))
            self.hdrs[-1] += payload[5:]
            return frame(0xF6, b'\1')
        if cmd == 0x77:
            self.lists.append(payload)
            return frame(0xF7, b'\1')
        if cmd == 0x78:
            self.msgs.append(payload)
            return frame(0xF8, b'\1')
        if cmd == 0x79:                    # VC_ROWS [первая][строки] — до VC_LIST
            self.rows_frames.append(payload)
            return frame(0xF9, b'\1')
        return frame(0xEE, b'unknown')


class CoreMachine(Machine):
    """Банк ядра видео: ESP — за портами #57/#77 (выбор #13), ядро видео — chip модели FT812.

    script — шаги [(условие(машина), действие)]: действие — набор кодов клавиш WC, которые держатся
    два тика, или функция. Шаг ждёт своего условия; следующий — после конца предыдущего."""
    list_model = True

    def __init__(self, data, *, elf=None, esp=True, script=(), bank='core', fifo=None, drain=None,
                 elf_file=ELF_FILE, wc_service=True, panel_stream=0, **kw):
        # /WC/ESP/VIDEO.ELF на томе WC (None — нет); wc_service — служебный поток WC (#FD);
        # panel_stream — поток ролика (0 — левая панель, 1 — правая)
        self.elf_file, self.wc_service, self.panel_stream = elf_file, wc_service, panel_stream
        self.stream_log, self.fentries = [], []
        self.cwd, self.found, self.reading_elf, self.elf_pos = 'panel', None, False, 0
        self.core = VideoCore(fifo, drain)
        super().__init__(data, chip=self.core, bank=bank, **kw)
        self.elf = elf or VideoElf()
        self.elf.machine = self
        # поток WC — страница WildDOS: API берёт её из #600B, прямой GIPAG — из окна #0000 (#6000)
        self.cpu.memory[0x6000] = self.cpu.memory[0x600B] = STREAM_PAGE[panel_stream]
        self.slave = SlaveHd()
        self.link = EspSide(self.slave)
        self.ports = SpiPorts(self.slave)
        self.ports.memory = self.cpu.memory
        self.link.rx, self.link.tx = bytearray(), bytearray()
        self.stream = bytearray()
        self.slave.on_written = self._written
        self.slave.on_read = self._read_started
        if not esp:
            self.link.die()
            self.slave.buffer[:] = bytes(len(self.slave.buffer))
        self.script = list(script)
        self.held, self.held_from, self.ready = (), 0, None
        self.wc_calls = []
        self.page_log = []                 # A' каждого вызова API (страница у API 0 и 79)
        # wc_exit: SP входа WC — адрес возврата 0x7000, который кладёт call()
        self.set('_ret_sp', 0x5D00, 2)

    def ticks(self):
        return self.cpu.memory[0x6009]

    def _app(self):
        self.link.pump()
        self.stream += self.link.tx
        self.link.tx.clear()
        for cmd, payload in take_frames(self.stream):
            self.link.rx += self.elf(cmd, payload)
        self.link.pump()

    def _written(self):
        self.link._written()
        self._app()

    def _read_started(self, address):
        self._app()
        self.link._read_started(address)

    def output(self, port, value):
        low = port & 0xFF
        if low == 0x77:
            self.ports.write(port, value)
            super().output(port, value)
        elif low == 0x57 and self.slave.selected:
            self.ports.write(port, value)
        else:
            super().output(port, value)

    def input(self, port):
        if port & 0xFF == 0x57 and self.slave.selected:
            return self.ports.read(port)
        return super().input(port)

    def step_script(self):
        if self.held and (self.ticks() - self.held_from) & 255 >= 2:
            self.held = ()
            self.script.pop(0)
        if not self.held and self.script:
            cond, action = self.script[0]
            # условие выполнилось — нажатие через 3 тика: окно успевает начать опрос
            # клавиш, и клавиша для него — новое нажатие, а не удержанная
            if self.ready is None and cond(self):
                self.ready = self.ticks()
            if self.ready is not None and (self.ticks() - self.ready) & 255 >= 3:
                self.ready = None
                if callable(action):
                    action(self)
                    self.script.pop(0)
                else:
                    self.held, self.held_from = tuple(action), self.ticks()

    def api(self):
        m = self.cpu
        a = m.a
        if 0x10 <= a <= 0x1C or a == 0x2D:
            self.step_script()
            pressed = a in self.held or (a == 0x2D and bool(self.held))
            m.a, m.f = 0, 0 if pressed else 0x40
            self.api_calls.append(a)
            self.page_log.append(None)
            self.ret()
        elif a in (0x40, 0x50):
            self.wc_calls.append((a, m._Z80State__alt_af[1]))
            m.a, m.f = 0, 0x40
            self.api_calls.append(a)
            self.page_log.append(None)
            self.ret()
        elif a == 0x30 and not self.reading_elf and m.memory[0x6000] != m.memory[0x600B]:
            raise AssertionError(('LOAD512 ролика: страница API #600B не та, что в окне #0000',
                                  hex(m.memory[0x600B]), hex(m.memory[0x6000])))
        elif a in (0x39, 0x3B, 0x33, 0x3E, 0x3F) or (a == 0x30 and self.reading_elf):
            self.wc_files(a)
            self.api_calls.append(a)
            self.page_log.append(None)
            self.ret()
        else:
            self.page_log.append(m._Z80State__alt_af[1])
            super().api()

    def wc_files(self, a):
        """STREAM, FENTRY, TENTRY, GFILE, GDIR и LOAD512 файла /WC/ESP/VIDEO.ELF (WC Improved)."""
        m = self.cpu
        if a == 0x39:                      # STREAM: #FD — поток и каталог WC, #FE — клон панели, #FF — корень
            d = (m.de >> 8) & 0xFF
            self.stream_log.append(d)
            if d == 0xFD and not self.wc_service:
                m.a, m.f = 2, 0
                return
            self.cwd = {0xFD: '/wc', 0xFE: 'panel', 0xFF: '/'}.get(d, self.cwd)
            # активный поток: #FD — служебный, #FE — клон панели в 0 и 1 с выбором 0, 0/1 — выбор
            page = {0xFD: STREAM_PAGE['wc'], 0xFE: STREAM_PAGE[0], 0: STREAM_PAGE[0], 1: STREAM_PAGE[1]}
            m.memory[0x600B] = page.get(d, m.memory[0x600B])
            m.a, m.f = 0, 0x40
        elif a == 0x3B:                    # FENTRY [флаг][имя][0]
            flag = m.memory[m.hl]
            name = bytes(m.memory[m.hl + 1:m.hl + 40]).split(b'\0')[0].decode().upper()
            self.fentries.append((self.cwd, flag, name))
            key = (self.cwd, flag, name)
            if key in (('/', 0x10, 'WC'), ('/wc', 0x10, 'ESP')):
                self.found, size = self.cwd.rstrip('/') + '/' + name.lower(), 0
            elif key == ('/wc/esp', 0, 'VIDEO.ELF') and self.elf_file is not None:
                self.found, size = 'file', len(self.elf_file)
            else:
                self.found = None
            if self.found:
                m.hl, m.de, m.f = size & 0xFFFF, size >> 16, 0
            else:
                m.f = 0x40
        elif a == 0x33:                    # TENTRY: запись каталога, время и дата FAT — +22, +24
            m.set_memory_block(m.de, b'VIDEO   ELF' + bytes(11) + struct.pack('<HHHI', ELF_TIME, ELF_DATE, 5,
                                                                          len(self.elf_file)))
        elif a == 0x3F:                    # GDIR
            assert self.found and self.found != 'file', ('GDIR без каталога', self.found)
            self.cwd = self.found
        elif a == 0x3E:                    # GFILE
            assert self.found == 'file', ('GFILE без файла', self.found)
            self.reading_elf, self.elf_pos = True, 0
        else:                              # LOAD512 из VIDEO.ELF
            count = m.b * 512
            m.set_memory_block(m.hl, self.elf_file[self.elf_pos:self.elf_pos + count].ljust(count, b'\xA5'))
            self.elf_pos += count
            m.hl = (m.hl + count) & 0xFFFF
            m.a, m.f = 0, 0x40

    def stub(self, name):
        self.step_script()
        if name == '@GIPAG':
            self.reading_elf = False       # поток — снова на ролике
            m = self.cpu
            assert m.memory[0x6000] == m.memory[0x600B], (
                'GIPAG ставит страницу окна #0000, LOAD512 читает #600B', hex(m.memory[0x6000]),
                hex(m.memory[0x600B]))
        super().stub(name)

    def call(self, name, args=b'', limit=30000):
        """Как Machine.call, но таймер WC (#6009) — по концу кадра эмулятора (прерывание), а не
        по точкам останова: банк ядра ждёт тиков в циклах без вызовов WC."""
        m = self.cpu
        m.sp = 0x5D00
        m.set_memory_block(m.sp, b'\x00\x70' + args)
        m.pc = self.s[name]
        for _ in range(limit):
            m.ticks_to_stop = 200000
            event = m.run()
            if event & 8:
                m.memory[0x6009] = (m.memory[0x6009] + 1) & 255
            if m.pc == 0x7000:
                return
            if event & 1:
                if m.pc == 0x6006:
                    self.api()
                elif m.pc in self.stubs:
                    self.stub(self.stubs[m.pc])
        raise AssertionError(('Execution limit', name, hex(m.pc)))


def avi_file(frames=40, **kw):
    data, pcm, info = avigen.make_avi(frames, **kw)
    return data, info


def bg_state(m):
    """BG_FAR->state (core_bg.c): блок в странице 1 по #FE40 — в окне #C000 или в сохранённой."""
    page1 = m.cpu.memory[0xC000:] if m.cpu.memory[0x6003] == 0x8A else m.banks[0x8A]
    return page1[0xFE40 - 0xC000]


def movi_end(data, info):
    """Конец LIST movi: за его 8-байтовым заголовком — размер (с FOURCC 'movi')."""
    return info['movi_start'] - 4 + struct.unpack_from('<I', data, info['movi_start'] - 8)[0]


@unittest.skipIf(EspSide is None, 'нет тестов Network VDAC4 (VDAC4_NETWORK_TESTS)')
class CoreTests(unittest.TestCase):

    def run_core(self, m, limit=6000):
        m.call('_far_entry', limit=limit)
        return m

    def check_exit(self, m):
        e = m.elf
        self.assertIn(0x73, e.commands)
        self.assertEqual(e.commands[-1], 0x6D, 'последней — APP_STOP')
        self.assertEqual(e.state, 3)
        self.assertEqual(m.wc_calls[-2:], [(0x50, 0), (0x40, 0)], 'текст WC')

    def test_core_try_carries_the_frame_and_returns(self):
        """Шлюз банка изображений: в банк 11 с общим кадром (размер файла — по нему разбор
        заголовка), без VIDEO.ELF — назад в банк изображений, за вызов core_try."""
        data, info = avi_file()
        m = CoreMachine(data, elf=VideoElf(app='core'), bank='image')
        m.call('_core_try', limit=3000)
        pages = [p for a, p in zip(m.api_calls, m.page_log) if a == 79]
        self.assertEqual(pages, [11, 0])
        self.assertEqual(m.bank, 'image')
        self.assertEqual(m.elf.commands, [0x69, 0x6C, 0x6D], 'банк ядра разобрал файл и спросил ESP')

    def test_no_esp_returns_without_core(self):
        data, info = avi_file()
        m = self.run_core(CoreMachine(data, esp=False))
        self.assertEqual(m.elf.commands, [])
        self.assertEqual(m.core.cpureset, [])
        self.assertEqual(m.writes, [])

    def test_other_app_under_the_name_is_stopped(self):
        data, info = avi_file()
        m = self.run_core(CoreMachine(data, elf=VideoElf(app='core')))
        self.assertEqual(m.elf.commands, [0x69, 0x6C, 0x6D])
        self.assertEqual(m.core.cpureset, [])

    def test_without_video_elf_in_wc_esp_goes_to_ft812(self):
        data, info = avi_file()
        m = self.run_core(CoreMachine(data, elf_file=None))
        self.assertEqual(m.elf.commands, [], 'без VIDEO.ELF ESP не зовётся')
        self.assertEqual(m.core.cpureset, [])
        self.assertEqual(m.cpu.memory[0x600B], STREAM_PAGE[0], 'поток — тот, с которым запущен плагин')

    def test_video_elf_delivered_from_wc_esp(self):
        """VIDEO.ELF — из /WC/ESP: MOD_QUERY (длина, время записи), его нет в кэше ESP — MOD_PUT по
        сектору, MOD_COMMIT, APP_START; ролик — после, с потока клона панели. Второй раз — из кэша."""
        for service, panel in ((True, 0), (True, 1), (False, 0), (False, 1)):
            with self.subTest(wc_service=service, panel_stream=panel):
                data, info = avi_file(40)
                elf = VideoElf(cached=False)
                script = [(lambda m: len(m.elf.status) >= 3, lambda m: m.cpu.memory.__setitem__(0x6004, 1))]
                m = self.run_core(CoreMachine(data, elf=elf, script=script, wc_service=service,
                                              panel_stream=panel))
                puts = -(-len(ELF_FILE) // 512)
                self.assertEqual(elf.commands[:puts + 4], [0x69] + [0x6A] * puts + [0x6B, 0x6C, 0x70])
                self.assertEqual(elf.queries, [(0, 'VIDEO.ELF', len(ELF_FILE), ELF_DATE << 16 | ELF_TIME)])
                self.assertEqual(elf.stored, ELF_FILE)
                self.assertEqual(elf.app_starts, ['VIDEO.ELF'])
                if service:
                    self.assertEqual(m.stream_log[0], 0xFD)
                    self.assertEqual([f[2] for f in m.fentries], ['ESP', 'VIDEO.ELF'])
                else:
                    # старый WC: от корня тома панели, как плеер
                    self.assertEqual(m.stream_log[:7], [0xFD, 0xFE, 0, 0xFF, 1, 0xFF, 0])
                    self.assertEqual([f[2] for f in m.fentries], ['WC', 'ESP', 'VIDEO.ELF'])
                self.assertEqual(m.stream_log[-1], 0xFE if not service else 0xFD)
                self.assertEqual(m.cpu.memory[0x600B], STREAM_PAGE[panel], 'поток — тот, с которым запущен плагин')
                self.assertGreater(m.core.parser.frames, 0, 'ролик пошёл после доставки')
                self.check_exit(m)
                # второй ролик: ELF уже в кэше — без MOD_PUT
                elf.commands.clear()
                elf.exit_seen, elf.load_polls = False, 3
                m = self.run_core(CoreMachine(data, elf=elf, script=script, wc_service=service,
                                              panel_stream=panel))
                self.assertEqual(elf.commands[:3], [0x69, 0x6C, 0x70])
                self.assertEqual(elf.puts, puts)

    def test_core_that_does_not_load_returns_to_ft812(self):
        data, info = avi_file()
        m = self.run_core(CoreMachine(data, elf=VideoElf(core_error=True)))
        self.assertEqual(m.elf.commands[:3], [0x69, 0x6C, 0x70])
        self.assertEqual(m.elf.commands[-1], 0x6D)
        self.assertEqual(m.core.cpureset, [])
        # отказ не молчит: окно WC перед путём FT812
        self.assertTrue(any('Video core did not start.' in w[2] for w in m.windows), m.windows)

    def test_whole_video_list_at_end_and_exit(self):
        data, info = avi_file(40)
        end = movi_end(data, info)
        script = [
            # конец ролика: окно списка со строками — Esc в нём
            (lambda m: len(m.elf.lists) >= 2, (API_ESC,)),
            # список закрыт — Esc в плеере (ABT)
            (lambda m: m.elf.lists and m.elf.lists[-1][0] == 0, lambda m: m.cpu.memory.__setitem__(0x6004, 1)),
        ]
        m = CoreMachine(data, script=script)
        list_volume(m)
        self.run_core(m)
        e = m.elf
        self.assertEqual(e.commands[:3], [0x69, 0x6C, 0x70])
        self.assertEqual(e.start, b'VIDEO\0')
        self.assertEqual(len(e.hdrs), 1)
        self.assertEqual(bytes(e.hdrs[0]), data[:info['movi_start']], 'HDR — заголовок до movi')
        # эпоха: сброс до HDR, затем 0; поток — файл до конца movi
        self.assertEqual(m.core.cpureset[:3], [1, 1, 0])
        self.assertEqual(bytes(m.core.streams[-1]), data[:end])
        self.assertEqual(m.core.parser.frames, 40)
        # STATUS с телом: начало 0, кадров 40, период 40 000, байтов звука 0
        body = e.status[0][2]
        self.assertEqual(struct.unpack_from('<IIII', body, 2), (0, 40, 40000, 0))
        ticks = [t for t, _, _ in e.status]
        gaps = [(b - a) & 255 for a, b in zip(ticks, ticks[1:])]
        # раз в 5 тиков; дольше — только пока банк списка читает каталог (окно на экране)
        self.assertEqual(sorted(gaps)[len(gaps) // 2], 5, 'STATUS раз в 100 мс')
        self.assertLessEqual(max(gaps), 25, 'молчание ESP считает выходом через 2 с')
        # список: «Reading folder...», затем строки — имена в CP866, текущий не отмечен (его нет)
        self.assertIn(b'Reading folder...', e.lists[0])
        rows = e.lists[1][9:].split(b'\0')
        self.assertIn('Супер клип'.encode('cp866'), rows)
        self.assertIn('Ёлка'.encode('cp866'), rows)
        self.assertNotIn(b'XVID', rows)
        self.check_exit(m)

    def test_exit_at_timer_wrap(self):
        """Выход (Esc; так же — PgDn/PgUp за краем списка роликов): ядро — назад (EXIT, ждать
        состояния 3), APP_STOP. Таймер WC около 250: ожидание 10 тиков в core_leave считалось в int и
        при t 246…255 не кончалось (плагин вставал после EXIT, 28.10.2026: PgUp/PgDn и Esc)."""
        for at in range(240, 256, 3):
            with self.subTest(at=at):
                data, info = avi_file(60)

                def esc(m, at=at):
                    m.cpu.memory[0x6009] = at
                    m.cpu.memory[0x6004] = 1

                m = self.run_core(CoreMachine(data, script=[(lambda m: len(m.elf.status) >= 3, esc)]))
                self.check_exit(m)
                self.assertEqual(m.cpu.a, 0, 'код выхода — WC_EXIT')

    def test_pgdn_pgup_neighbour_video_in_the_same_core(self):
        """PgDn/PgUp — соседний ролик папки (список — как по Enter) тем же ядром: без EXIT, APP_STOP и
        нового START, поток и HDR соседнего ролика. Прежде — выход в WC с соседним файлом, ядро — в
        слот 0 и снова из слота 3 (пользователь, 28.10.2026). Порядок и курсор — из окна списка (Enter,
        Esc): PgDn — строка за курсором, PgUp с неё — снова строка курсора."""
        data, info = avi_file(80)
        script = [
            (lambda m: len(m.elf.status) >= 3, (API_ENKE,)),
            (lambda m: len(m.elf.lists) >= 2, (API_ESC,)),
            (lambda m: m.elf.lists and m.elf.lists[-1][0] == 0, (0x1A,)),                 # PgDn
            (lambda m: len(m.elf.hdrs) >= 2 and len(m.elf.status) >= 14, (0x19,)),       # PgUp
            (lambda m: len(m.elf.hdrs) >= 3 and len(m.elf.status) >= 22,
             lambda m: m.cpu.memory.__setitem__(0x6004, 1)),
        ]
        m = CoreMachine(data, script=script)
        files = list_volume(m)
        self.run_core(m)
        e = m.elf
        self.assertEqual(len(e.hdrs), 3)
        self.assertEqual(e.commands.count(0x70), 1, 'ядро не грузилось заново')
        self.assertEqual(e.commands.count(0x73), 1, 'EXIT — только на выходе')
        self.assertEqual(e.commands.count(0x6D), 1, 'APP_STOP — только на выходе')
        page = e.lists[1]
        sel = page[5]
        rows = page[9:].split(b'\0')[1:10] + e.rows_frames[0][1:].split(b'\0')[:-1]
        sfn = {'01 intro': b'01INTR~1AVI', 'A very long video file name that exceeds the limit': b'AVERYL~1AVI',
               'alpha video': b'ALPHAV~1AVI', 'BETA': b'BETA    AVI', 'GAMMA': b'GAMMA   AVI',
               'Zeta clip': b'ZETACL~1AVI'}

        def obj(row):
            cluster = files[sfn[row.decode('cp866')]][0]
            return next(d for ch, d, _ in m.objects if ch[0] == cluster)
        self.assertEqual(bytes(e.hdrs[1]), obj(rows[sel + 1])[:len(e.hdrs[1])], 'PgDn — строка за курсором')
        self.assertEqual(bytes(e.hdrs[2]), obj(rows[sel])[:len(e.hdrs[2])], 'PgUp — снова строка курсора')
        self.check_exit(m)

    def test_list_long_cyrillic_name_whole(self):
        """Имя кириллицей — целиком: банк списка считал ширину имени по ширинам ASCII шрифта FT812
        (font_w[128]), и байты CP866 #80… читали за таблицей чужую память — длинное русское имя
        обрывалось с «...» на двадцати буквах («КИНО Мама, мы все тя...», 28.10.2026). В банке ядра
        видео по ширине режет ESP."""
        from avigen import make_avi
        data, info = avi_file(40)
        script = [
            (lambda m: len(m.elf.status) >= 3, (API_ENKE,)),
            (lambda m: len(m.elf.lists) >= 2, (API_ESC,)),
            (lambda m: m.elf.lists and m.elf.lists[-1][0] == 0, lambda m: m.cpu.memory.__setitem__(0x6004, 1)),
        ]
        m = CoreMachine(data, script=script)
        name = 'КИНО_Мама,_мы_все_тяжело_больны_Live_2020'
        d = bytearray(m.dir_entry(b'.          ', 0, 0, 0x10) + m.dir_entry(b'..         ', 0, 0, 0x10))
        clip = make_avi(3, audio=False)[0]
        c = m.add_object(clip)
        d.extend(test_ftview.lfn_entries(name + '.avi', b'KINO_M~1AVI'))
        d.extend(m.dir_entry(b'KINO_M~1AVI', c, len(clip)))
        d.extend(bytes(-len(d) % 512 + 512))
        m.dir_cluster = m.add_object(bytes(d), eoc=True)
        self.run_core(m)
        rows = m.elf.lists[1][9:].split(b'\0')
        self.assertIn(name.replace('_', ' ').encode('cp866'), rows, rows)
        self.check_exit(m)

    def test_list_rows_whole_names_in_two_frames(self):
        """Строки списка — имена целиком (по ширине строки режет ESP): страница больше 9 строк — строки
        с 9-й командой VC_ROWS до VC_LIST, у VC_LIST в «показ» — флаг #80 (оставить их)."""
        data, info = avi_file(40)
        script = [
            (lambda m: len(m.elf.status) >= 3, (API_ENKE,)),
            (lambda m: len(m.elf.lists) >= 2, (API_ESC,)),
            (lambda m: m.elf.lists and m.elf.lists[-1][0] == 0, lambda m: m.cpu.memory.__setitem__(0x6004, 1)),
        ]
        m = CoreMachine(data, script=script)
        list_volume(m)
        self.run_core(m)
        e = m.elf
        page = e.lists[1]
        self.assertEqual(page[0], 0x81, 'показ и флаг #80')
        self.assertEqual(len(e.rows_frames), 1)
        self.assertEqual(e.rows_frames[0][0], 9, 'строки с 9-й')
        first = page[9:].split(b'\0')[1:10]
        rest = e.rows_frames[0][1:].split(b'\0')[:-1]
        rows = first + rest
        self.assertEqual(len(rows), page[1], 'все строки страницы')
        self.assertIn('A very long video file name that exceeds the limit'.encode('cp866'), rows)
        self.assertIn('Супер клип'.encode('cp866'), rows)
        self.check_exit(m)

    def test_pause_holds_the_core_and_tells_esp(self):
        data, info = avi_file(60)
        script = [
            (lambda m: len(m.elf.status) >= 3, (API_SPKE,)),
            (lambda m: m.core.pause and len(m.elf.status) >= 8, (API_SPKE,)),
            (lambda m: not m.core.pause and len(m.elf.status) >= 12,
             lambda m: m.cpu.memory.__setitem__(0x6004, 1)),
        ]
        m = self.run_core(CoreMachine(data, script=script))
        self.assertIn(4, m.core.cpureset)
        i = m.core.cpureset.index(4)
        self.assertEqual(m.core.cpureset[i + 1], 0, 'Space снова — пауза снята')
        flags = [body[1] for _, _, body in m.elf.status]
        self.assertIn(3, flags, 'пауза и панель в STATUS')
        self.check_exit(m)

    def test_seek_streams_from_index_entry(self):
        frames = 300                       # 12 с: Right — на 10 с вперёд
        data, info = avi_file(frames)
        end = movi_end(data, info)
        script = [
            (lambda m: len(m.elf.status) >= 1, (API_RGGG,)),
            # эпоха 1 — поток с начала, 2 — с записи перемотки
            (lambda m: m.core.epoch >= 2 and len(m.elf.status) >= 6,
             lambda m: m.cpu.memory.__setitem__(0x6004, 1)),
        ]
        m = CoreMachine(data, script=script)
        list_volume(m)
        self.run_core(m)
        e = m.elf
        # Окна «Building seek index...» нет: таблица строится в фоне (core_bg.c); Right сразу после
        # начала — ожидание с процентами в панели (VS_BUSY, текст в поле имени STATUS)
        busy = [body for _, _, body in e.status if body[1] & 16]
        self.assertTrue(busy, 'ожидание таблицы — в панели')
        self.assertTrue(all(b[18:].startswith(b'Indexing ') and b[18:].rstrip(b'\0').endswith(b'%') for b in busy))
        self.check_seek(m, data, info)

    def test_seek_after_background_index_does_not_wait(self):
        frames = 300
        data, info = avi_file(frames)
        mark = []
        script = [
            (lambda m: len(m.elf.status) >= 3, (API_SPKE,)),
            # на паузе фоновая таблица идёт шагами подряд: готова — Right без ожидания
            (lambda m: bg_state(m) == 2, (API_RGGG,)),
            (lambda m: m.core.epoch >= 2, lambda m: mark.append(len(m.elf.status))),
            (lambda m: True, (API_SPKE,)),
            (lambda m: len(m.elf.status) >= mark[0] + 8, lambda m: m.cpu.memory.__setitem__(0x6004, 1)),
        ]
        m = CoreMachine(data, script=script)
        list_volume(m)
        self.run_core(m, limit=20000)
        self.assertFalse([body for _, _, body in m.elf.status if body[1] & 16], 'таблица была готова')
        self.check_seek(m, data, info)

    def test_background_index_on_fragmented_file(self):
        # кластер 1 КиБ, цепочка вразброс: фоновый читатель idx1 идёт по FAT (fat_walk) и
        # перемежается с потоком; Right сразу — ожидание, затем поток с записи
        frames = 300
        data, info = avi_file(frames)
        count = -(-len(data) // 1024)
        chain = list(range(3, 3 + count))
        random.Random(5).shuffle(chain)
        script = [
            (lambda m: len(m.elf.status) >= 2, (API_RGGG,)),
            (lambda m: m.core.epoch >= 2 and len(m.elf.status) >= 6,
             lambda m: m.cpu.memory.__setitem__(0x6004, 1)),
        ]
        m = CoreMachine(data, script=script, spc=2, chain=chain)
        list_volume(m)
        self.run_core(m, limit=20000)
        self.check_seek(m, data, info)

    def check_seek(self, m, data, info, whole=True):
        # whole=False: медленный разбор — поток после перемотки к выходу подан не весь, сверяется начало
        e = m.elf
        end = movi_end(data, info)
        self.assertFalse(any(b'seek index' in x for x in e.msgs))
        # поток до перемотки — ровно файл: чтения idx1 на том же потоке WildDOS его не сбили
        first = bytes(m.core.streams[0])
        self.assertEqual(first, data[:len(first)])
        # STATUS новой эпохи: кадр записи и байты звука до неё — как в файле
        later = [body for _, ep, body in e.status if ep == m.core.epoch]
        base, total, period, audio = struct.unpack_from('<IIII', later[0], 2)
        self.assertGreater(base, 0)
        chunks = info['chunks']           # (FOURCC, смещение заголовка чанка в файле, длина)
        # запись таблицы — чанк, до которого ровно base кадров и audio байтов звука
        entry = next(c for c in chunks if
                     sum(1 for d in chunks if d[0] == b'00dc' and d[1] < c[1]) == base and
                     sum(d[2] for d in chunks if d[0] == b'01wb' and d[1] < c[1]) == audio)
        seek_pos = entry[1]
        fill = -(info['movi_start'] + 8) & 3
        junk = b'JUNK' + struct.pack('<I', fill + (seek_pos & 3)) + bytes(fill)
        expect = data[:info['movi_start']] + junk + data[seek_pos & ~3:end]
        got = bytes(m.core.streams[-1])
        if whole:
            self.assertEqual(got, expect)
        else:
            self.assertGreater(len(got), info['movi_start'] + 4096)
            self.assertEqual(got, expect[:len(got)])
        self.check_exit(m)

    def test_big_fifo_background_index_while_playing(self):
        """Ядро 1.38: FIFO 2 МБ. Шаги фоновой таблицы идут и без паузы, пока в FIFO больше
        256 КиБ: таблица готова раньше, чем подан весь ролик, Right — без ожидания."""
        frames = 300
        data, info = avi_file(frames, frame=lambda n: struct.pack('<I', n) + bytes((n + i) & 255 for i in range(3000)))
        mark = []
        script = [
            (lambda m: bg_state(m) == 2,
             lambda m: mark.append((m.core.level(), len(m.core.streams[-1]), list(m.core.cpureset)))),
            (lambda m: True, (API_RGGG,)),
            (lambda m: m.core.epoch >= 2 and len(m.elf.status) >= 6,
             lambda m: m.cpu.memory.__setitem__(0x6004, 1)),
        ]
        m = CoreMachine(data, script=script, fifo=1 << 21, drain=1500)
        list_volume(m)
        self.run_core(m, limit=60000)
        self.assertTrue(mark, 'таблица не достроилась')
        level, taken, resets = mark[0]
        self.assertGreater(level, 0x40000, 'в FIFO при готовой таблице — запас больше порога')
        self.assertLess(taken + level, len(data), 'таблица готова до конца подачи')
        self.assertNotIn(4, resets, 'до готовой таблицы паузы не было')
        self.assertFalse([body for _, _, body in m.elf.status if body[1] & 16], 'Right без ожидания')
        self.check_seek(m, data, info, whole=False)

    def test_hd_frame_goes_to_core(self):
        """Кадр больше 1024×768 (до 2048×1536) — ядру видео 1.38 (уменьшит вдвое); прежде HD шёл
        путём FT812 — экран 4:3, кадр обрезан. Больше 2048 — путь FT812, ядро не грузится."""
        data, info = avi_file(30, width=1280, height=720)
        script = [(lambda m: len(m.elf.lists) >= 2, (API_ESC,)),
                  (lambda m: m.elf.lists and m.elf.lists[-1][0] == 0,
                   lambda m: m.cpu.memory.__setitem__(0x6004, 1))]
        m = CoreMachine(data, script=script)
        list_volume(m)
        self.run_core(m)
        self.assertIn(0x70, m.elf.commands, 'START ядра видео')
        self.assertEqual(bytes(m.core.streams[-1]), data[:movi_end(data, info)])
        data, info = avi_file(30, width=2047, height=1537)
        m = self.run_core(CoreMachine(data))
        self.assertEqual(m.elf.commands, [], 'больше 2048×1536 — не ядру')

    def test_second_audio_track_goes_to_core(self):
        """Вторая звуковая дорожка (поток 3) ролик не отводит: ядро 1.38 берёт только 01wb."""
        data, info = avi_file(30)
        data = bytearray(data)
        hdrl = struct.unpack_from('<I', data, 16)[0]
        ah = struct.pack('<4s4sIHHIIIIIIIIhhhh', b'auds', bytes(4), 0, 0, 0, 0, 1, 11025, 0,
                         100, 0, 0xFFFFFFFF, 1, 0, 0, 0, 0)
        af = struct.pack('<HHIIHH', 1, 1, 11025, 11025, 1, 8)
        strl = avigen.list_chunk(b'strl', avigen.chunk(b'strh', ah) + avigen.chunk(b'strf', af))
        at = 20 + hdrl
        data[at:at] = strl
        struct.pack_into('<I', data, 16, hdrl + len(strl))
        struct.pack_into('<I', data, 4, len(data) - 8)
        data = bytes(data)
        start = info['movi_start'] + len(strl)
        end = start - 4 + struct.unpack_from('<I', data, start - 8)[0]
        script = [(lambda m: len(m.elf.lists) >= 2, (API_ESC,)),
                  (lambda m: m.elf.lists and m.elf.lists[-1][0] == 0,
                   lambda m: m.cpu.memory.__setitem__(0x6004, 1))]
        m = CoreMachine(data, script=script)
        list_volume(m)
        self.run_core(m)
        self.assertIn(0x70, m.elf.commands, 'START ядра видео')
        self.assertEqual(bytes(m.core.streams[-1]), data[:end])

    def test_enter_list_and_choose_another_video(self):
        data, info = avi_file(80)
        script = [
            (lambda m: len(m.elf.status) >= 3, (API_ENKE,)),
            (lambda m: len(m.elf.lists) >= 2, (API_DWWW,)),
            (lambda m: len(m.elf.lists) >= 3, (API_ENKE,)),
            # окна: чтение, строки, выбор, убрать; в конце второго ролика — снова чтение и
            # строки: Esc в окне, затем Esc в плеере
            (lambda m: len(m.elf.lists) >= 6, (API_ESC,)),
            (lambda m: len(m.elf.lists) >= 7 and m.elf.lists[-1][0] == 0,
             lambda m: m.cpu.memory.__setitem__(0x6004, 1)),
        ]
        m = CoreMachine(data, script=script)
        files = list_volume(m)
        self.run_core(m)
        e = m.elf
        self.assertEqual(len(e.hdrs), 2)
        # выбранный ролик — следующий за первым в отсортированном списке; его HDR и поток
        sel = e.lists[2][5]
        rows = e.lists[2][9:].split(b'\0')
        chosen = rows[1 + sel - e.lists[2][3]]   # [0] — сообщение (пусто)
        names = {'01 intro': b'01INTR~1AVI', 'A very long video file name that exceeds the limit': b'AVERYL~1AVI',
                 'alpha video': b'ALPHAV~1AVI', 'BETA': b'BETA    AVI'}
        sfn = names.get(chosen.decode('cp866'))
        self.assertIsNotNone(sfn, chosen)
        cluster, size = files[sfn]
        other = next(d for ch, d, _ in m.objects if ch[0] == cluster)
        hdr_len = len(e.hdrs[1])
        self.assertEqual(bytes(e.hdrs[1]), other[:hdr_len])
        self.assertTrue(bytes(m.core.streams[-1]).startswith(other[:64]))
        self.check_exit(m)


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, default=test_ftview.OUT)
    args, rest = parser.parse_known_args()
    test_ftview.OUT = args.out
    unittest.main(argv=[sys.argv[0], *rest])
