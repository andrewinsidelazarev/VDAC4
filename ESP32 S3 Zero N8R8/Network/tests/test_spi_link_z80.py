"""Машинные тесты канала SPI платы VDAC4 со стороны Z80.

Сборки плагинов для VDAC4 (build\\vdac4, ключ -DZIFI_SPI) исполняются в
эмуляторе Z80. Вместо портов ZiFi у них порты #57/#77 TS-Config, за которыми
стоит модель общего буфера ESP32-S3 и её прошивки (tests/spi_link_model.py).
Правила обмена в модели — те же, что сверены с C++-кодом прошивки в
test_spi_link_host.py.

Проверяется:
- драйвер shared/z80/zifi_spi.asm: поиск прошивки, передача и приём кадров
  любой длины, повтор запроса, перечитывание испорченного ответа, занятая
  очередь ESP, молчащая ESP, сохранность регистров, дисциплина портов;
- все сценарии test_ftp_plugin_z80.py — на сборках FTP и SMB для VDAC4;
- плагин NTP и программа tools/esp_info_vdac4.sna целиком, от запуска до
  экрана, с «прошивкой», отвечающей кадрами протокола.
Нужны сборки: "FTP Server\\build.bat vdac4", "SMB Server\\build.bat vdac4",
"NTP Time Sync\\build.bat vdac4", tools\\build_esp_info.bat.
"""
import pathlib
import random
import re
import unittest

import z80

import test_ftp_plugin_z80 as base
from spi_link_model import (BULK_SIZE, CFG_ESP, CFG_HOST_MASTER, CFG_NONE, CHUNK, CMD_RD_DONE, CMD_RDBUF,
                            CMD_RDDMA, CMD_WR_DONE, CMD_WRBUF, CMD_WRDMA, REQUEST, RESPONSE,
                            STATE_ADDR, STATE_EMPTY, STATE_PENDING, EspSide, LinkCore, SlaveHd,
                            SpiPorts)

ROOT = base.ROOT
FTP_WMF = ROOT / "FTP Server" / "build" / "vdac4" / "VDAC4FTP.WMF"
FTP_SYM = ROOT / "FTP Server" / "build" / "vdac4" / "VDAC4FTP.sym"
SMB_WMF = ROOT / "SMB Server" / "build" / "vdac4" / "VDAC4SMB.WMF"
SMB_SYM = ROOT / "SMB Server" / "build" / "vdac4" / "VDAC4SMB.sym"
WEATHER_WMF = ROOT / "Weather screensaver" / "build" / "vdac4" / "VDAC4WTH.WMF"
WEATHER_SYM = ROOT / "Weather screensaver" / "build" / "vdac4" / "VDAC4WTH.sym"
WEATHER2_WMF = ROOT / "Weather screensaver VDAC2" / "build" / "vdac4" / "VDAC4WT2.WMF"
WEATHER2_SYM = ROOT / "Weather screensaver VDAC2" / "build" / "vdac4" / "VDAC4WT2.sym"
NTP_WMF = ROOT / "NTP Time Sync" / "build" / "vdac4" / "VDAC4NTP.WMF"
NTP_SYM = ROOT / "NTP Time Sync" / "build" / "vdac4" / "VDAC4NTP.sym"
INFO_SNA = ROOT / "tools" / "esp_info_vdac4.sna"
INFO_SYM = ROOT / "tools" / "esp_info_vdac4.sym"
UPDATE_SNA = ROOT / "tools" / "update_vdac4.sna"
UPDATE_SYM = ROOT / "tools" / "update_vdac4.sym"
PROBE_SNA = ROOT / "tools" / "dma_probe_vdac4.sna"
PROBE_SYM = ROOT / "tools" / "dma_probe_vdac4.sym"

CARRY, ZERO = 0x01, 0x40
CMD_SYS_INFO, CMD_WIFI_INI, CMD_PING, CMD_NET_NTP = 0x02, 0x03, 0x04, 0x22
CMD_UPDATE_START, CMD_UPDATE_STOP = 0x08, 0x09
RESP_SYS_INFO, RESP_WIFI_INI, RESP_NET_NTP, RESP_READY, RESP_ACK = 0x82, 0x83, 0xA2, 0xF0, 0xFE
RESP_UPDATE_START, RESP_UPDATE_STOP, RESP_ERROR = 0x88, 0x89, 0xEE
CMD_LINK_PROBE, RESP_LINK_PROBE = 0x3E, 0xBE
CMD_WEATHER_GET, RESP_WEATHER = 0x24, 0xA4
frame = base.frame
# Исходный стенд UART: имя base.Harness на время сценариев SPI подменяется.
BaseHarness = base.Harness


def need(*paths: pathlib.Path) -> None:
    missing = [path.name for path in paths if not path.exists()]
    if missing:
        raise unittest.SkipTest("нет сборки для VDAC4: " + ", ".join(missing))


def spi_runs(harness, start: int = 0) -> list[tuple[int, int]]:
    """Прогоны DMA с SPI (без переноса «ОЗУ → ОЗУ» из порции в буфер кадра)."""
    return [run for run in harness.ports.dma_runs[start:] if run[0] != 0x01]


def take_frames(stream: bytearray) -> list[tuple[int, bytes]]:
    """Забрать из потока целые кадры [5A][CMD][LEN_L][LEN_H][DATA][XOR]."""
    frames = []
    while len(stream) >= 5:
        if stream[0] == 0xA6:
            command = stream[1]
            length = int.from_bytes(stream[2:4], 'little')
            assert command in (0x31,0x6A) and 0 < length <= 8196
            if len(stream) < 4+length: break
            frames.append((command,bytes(stream[4:4+length])))
            del stream[:4+length]
            continue
        assert stream[0] == 0x5A, "кадр должен начинаться с SYNC"
        length = stream[2] | stream[3] << 8
        if len(stream) < 5 + length:
            break
        check = 0
        for byte in stream[1:4 + length]:
            check ^= byte
        assert stream[4 + length] == check, "контрольная сумма кадра"
        frames.append((stream[1], bytes(stream[4:4 + length])))
        del stream[:5 + length]
    return frames


class SpiLinkMixin:
    """Порты #57/#77 и «прошивка» за ними вместо портов ZiFi."""

    # Плагин WC: страницу ОЗУ окна #8000 он узнаёт у WC (ячейка #6002).
    WC_PAGE2, PLUGIN_PAGE = 0x6002, 0x21

    def attach_link(self, core: LinkCore | None = None) -> None:
        self.slave = SlaveHd()
        self.link = EspSide(self.slave, core)
        self.ports = SpiPorts(self.slave)
        self.ports.memory = self.memory
        if self.PLUGIN_PAGE is not None:
            self.memory[self.WC_PAGE2] = self.PLUGIN_PAGE
            self.ports.windows[self.PLUGIN_PAGE] = 0x8000
        # Очереди стенда — те же объекты, что у «приложения прошивки»: тесты
        # кладут кадры в rx и читают отправленное из tx, как на стенде UART.
        self.link.rx = self.rx
        self.link.tx = self.tx
        self.app = None                 # обработчик кадров: (команда, данные) -> ответные байты
        self._app_stream = bytearray()
        self.slave.on_read = self._read_started
        self.slave.on_written = self._written

    def revive(self) -> None:
        """ESP перезапустилась: буфер и очереди новые."""
        cfg_writes = self.ports.cfg_writes
        self.attach_link()
        self.ports.cfg_writes = cfg_writes

    def _run_app(self) -> None:
        if self.app is None:
            return
        self.link.pump()
        self._app_stream += self.tx
        self.tx.clear()
        for command, payload in take_frames(self._app_stream):
            self.rx += self.app(command, payload)
        self.link.pump()

    def _written(self) -> None:
        self.link._written()
        self._run_app()

    def _read_started(self, address: int) -> None:
        if address == STATE_ADDR:
            # Чтение байта состояния — тот же «опрос очереди», что чтение
            # счётчика приёма у UART: на нём стенд считает опросы и даёт слово ESP.
            self.rx_polls += 1
            if self.esp is not None:
                self.esp(self)
        self._run_app()
        self.link._read_started(address)

    def port_in(self, port: int) -> int:
        value = self.ports.read(port)
        if value is not None:
            return value
        if port & 0xFF == 0xEF:
            raise AssertionError(f"сборка для VDAC4 читает порт ZiFi #{port:04X}")
        return 0xFF

    def port_out(self, port: int, value: int) -> None:
        if not self.ports.write(port, value) and port & 0xFF == 0xEF:
            raise AssertionError(f"сборка для VDAC4 пишет в порт ZiFi #{port:04X}")


class SpiHarness(SpiLinkMixin, BaseHarness):
    """Стенд плагина Wild Commander на канале SPI."""

    def __init__(self, wmf: pathlib.Path = FTP_WMF, sym: pathlib.Path = FTP_SYM,
                 core: LinkCore | None = None) -> None:
        BaseHarness.__init__(self, wmf, sym)
        self.attach_link(core)
        # Кэш процессора: записанное DMA Z80 читает только после SpiLink_Sweep.
        for name, hook in (("SpiLink_Sweep", self._swept), ("Vfs_Dispatch", self._dispatch)):
            try:
                address = self.symbol(name)
            except AssertionError:
                continue
            self.machine.set_breakpoint(address)
            self.hooks[address] = hook

    def _swept(self) -> None:
        self.ports.dma_dirty.clear()

    def protobuf_clean(self) -> bool:
        buffer = self.symbol("ProtoBuf")
        return self.ports.clean(buffer, buffer + self.word(self.symbol("ProtoRxLen")))

    def _dispatch(self) -> None:
        if not self.protobuf_clean():
            raise AssertionError("кадр разбирается, а его данные DMA ещё могут быть в кэше прежними")

    def call(self, name: str, *, de: int = 0, **registers: int) -> None:
        self.machine.de = de
        self.run(self.symbol(name), **registers)

    @property
    def carry(self) -> bool:
        return bool(self.machine.f & CARRY)

    @property
    def zero(self) -> bool:
        return bool(self.machine.f & ZERO)

    def send(self, command: int, payload: bytes = b"") -> None:
        """Proto_Send: кадр из памяти Z80 в ESP."""
        self.memory[0xC000:0xC000 + len(payload)] = payload
        self.call("Proto_Send", a=command, hl=0xC000, bc=len(payload))

    def poll(self, limit: int = 64) -> tuple[int, bytes] | None:
        """Proto_Poll до готового кадра: (команда, данные) либо None."""
        for _ in range(limit):
            self.call("Proto_Poll")
            if self.carry:
                if not self.protobuf_clean():
                    raise AssertionError("кадр отдан, а его данные DMA ещё могут быть в кэше прежними")
                length = self.word(self.symbol("ProtoRxLen"))
                buffer = self.symbol("ProtoBuf")
                return self.machine.a, bytes(self.memory[buffer:buffer + length])
        return None


class SpiDriverTest(unittest.TestCase):
    """Драйвер zifi_spi.asm и proto.asm поверх него (в составе плагина FTP)."""

    @classmethod
    def setUpClass(cls) -> None:
        need(FTP_WMF, FTP_SYM)

    def tearDown(self) -> None:
        for harness in getattr(self, "harnesses", []):
            self.assertFalse(harness.slave.selected, "после процедуры ESP остался выбранным")
            self.assertEqual(harness.slave.overruns, 0, "обращение за пределы общего буфера")
            self.assertLessEqual(set(harness.ports.cfg_writes),
                                 {CFG_HOST_MASTER, CFG_NONE, CFG_ESP})

    def harness(self, **kwargs) -> SpiHarness:
        h = SpiHarness(**kwargs)
        self.harnesses = getattr(self, "harnesses", []) + [h]
        return h

    def test_init_finds_firmware_and_starts_a_clean_session(self) -> None:
        h = self.harness()
        h.link.core.write(b"left over from the previous program")
        h.link.pump()
        h.call("ZiFi_Init", hl=0x1234)
        self.assertFalse(h.carry, "прошивка на месте")
        self.assertEqual(h.machine.hl, 0x1234, "HL сохранён")
        self.assertEqual(h.ports.cfg_writes[0], CFG_HOST_MASTER, "шина SPI возвращена Evo")
        self.assertEqual(h.link.core.to_z80, b"", "остаток прежнего сеанса выброшен")
        self.assertEqual(h.slave.buffer[STATE_ADDR], STATE_EMPTY)
        # Дальше — обычный обмен.
        h.rx += frame(RESP_READY)
        self.assertEqual(h.poll(), (RESP_READY, b""))

    def test_init_reports_absent_firmware(self) -> None:
        for name, prepare in (
                ("буфер пуст: ESP в сбросе", lambda h: h.link.die()),
                ("линия висит в единицах", lambda h: (h.link.die(),
                                                      h.slave.buffer.__setitem__(
                                                          slice(None), b"\xFF" * 64))),
                ("байт состояния есть, а прошивка не отвечает",
                 lambda h: (h.link.die(), h.slave.buffer.__setitem__(STATE_ADDR, STATE_EMPTY)))):
            with self.subTest(name=name):
                h = self.harness()
                prepare(h)
                h.call("ZiFi_Init")
                self.assertTrue(h.carry)

    def test_frames_to_esp_arrive_whole(self) -> None:
        rng = random.Random(1)
        h = self.harness()
        h.call("ZiFi_Init")
        for length in (0, 1, 48, 49, 50, 54, 55, 103, 104, 500, 1023, 1024):
            with self.subTest(length=length):
                payload = rng.randbytes(length)
                h.tx.clear()
                before = h.link.core.turns
                h.send(0x11, payload)
                self.assertFalse(h.carry)
                self.assertEqual(h.frames(), [(0x11, payload)])
                # Данные (с чётного адреса) уходят блоками DMA по 512 прямо из
                # памяти, заголовок — в запросе первого блока, остальное —
                # кусками по 54. На блок — два обмена: «отдаю» и «записан».
                blocks = length // BULK_SIZE
                rest = length % BULK_SIZE + 1 if blocks else length + 5
                self.assertEqual(h.link.core.turns - before, 2 * blocks + -(-rest // CHUNK))
                self.assertEqual(h.memory[h.symbol("SpiTxLen")], 0, "накопитель пуст")

    def test_frames_from_esp_arrive_whole(self) -> None:
        rng = random.Random(2)
        for latency, split in ((0, False), (1, False), (3, True), (17, True)):
            h = self.harness()
            h.link.latency = latency
            h.link.split_publish = split
            h.call("ZiFi_Init")
            for length in (0, 1, 49, 50, 54, 55, 300, 1024):
                with self.subTest(latency=latency, split=split, length=length):
                    payload = rng.randbytes(length)
                    h.rx += frame(0x92, payload)
                    self.assertEqual(h.poll(), (0x92, payload))
            self.assertIsNone(h.poll(3), "лишних кадров нет")

    def test_several_frames_in_a_row_and_in_one_burst(self) -> None:
        h = self.harness()
        h.call("ZiFi_Init")
        # ACK и ответ лежат в одной порции — оба должны быть разобраны.
        h.rx += frame(RESP_ACK) + frame(RESP_SYS_INFO, b"FW:test") + frame(0x66, b"x" * 70)
        self.assertEqual(h.poll(), (RESP_ACK, b""))
        self.assertEqual(h.poll(), (RESP_SYS_INFO, b"FW:test"))
        self.assertEqual(h.poll(), (0x66, b"x" * 70))

    def test_pending_bytes_leave_with_the_next_receive(self) -> None:
        # Байт, оставшийся в накопителе, уходит и без ZiFi_Flush — вместе с
        # обменом, которым Z80 забирает данные.
        h = self.harness()
        h.call("ZiFi_Init")
        h.call("ZiFi_PutChar", a=0x77)
        self.assertEqual(bytes(h.tx), b"")
        h.call("ZiFi_ReadBurst", hl=h.symbol("ProtoBurstBuf"))
        self.assertEqual(bytes(h.tx), b"\x77")

    def test_missed_request_is_sent_again(self) -> None:
        h = self.harness()
        h.call("ZiFi_Init")
        h.link.ignore_requests = 1
        seen = h.link.requests_seen
        h.send(0x06, b"payload")
        self.assertFalse(h.carry)
        self.assertEqual(h.link.requests_seen - seen, 2, "запрос повторён один раз")
        self.assertEqual(h.frames(), [(0x06, b"payload")])

    def test_lost_reply_does_not_duplicate_data(self) -> None:
        # ESP запрос обработала, но ответа Z80 не увидела и повторила запрос.
        h = self.harness()
        h.call("ZiFi_Init")
        h.link.lose_replies = 1
        h.send(0x06, b"once")
        self.assertFalse(h.carry)
        self.assertEqual(h.link.core.repeats, 1)
        self.assertEqual(h.frames(), [(0x06, b"once")], "данные приняты ровно один раз")
        # То же на приёме: порция, ответ с которой потерялся, не пропадает.
        h.rx += frame(0x92, b"reply that was lost once")
        h.link.pump()
        h.link.lose_replies = 1
        self.assertEqual(h.poll(), (0x92, b"reply that was lost once"))
        self.assertEqual(h.link.core.repeats, 2)

    def test_damaged_reply_is_read_again(self) -> None:
        h = self.harness()
        h.call("ZiFi_Init")
        h.rx += frame(0x92, bytes(range(40)))
        h.link.pump()
        h.link.corrupt_reads = 2
        seen = h.link.requests_seen
        self.assertEqual(h.poll(), (0x92, bytes(range(40))))
        self.assertEqual(h.link.requests_seen - seen, 1, "запрос не повторялся: хватило перечитать")
        self.assertEqual(h.link.core.repeats, 0)

    def test_busy_esp_queue_keeps_bytes_in_order(self) -> None:
        rng = random.Random(3)
        h = self.harness(core=LinkCore(from_capacity=64))
        h.call("ZiFi_Init")
        # «Приложение» забирает принятое лишь на каждом седьмом запросе: очередь
        # ESP то и дело полна, и запрос берётся частично или не берётся вовсе.
        requests = 0

        def written() -> None:
            nonlocal requests
            requests += 1
            h.link.app_reads = requests % 7 == 0
            h.link._written()
        h.slave.on_written = written
        h.link.app_reads = False
        payload = rng.randbytes(700)
        h.send(0x57, payload)
        self.assertFalse(h.carry)
        h.link.app_reads = True
        h.link.pump()
        self.assertEqual(h.frames(), [(0x57, payload)])
        self.assertGreater(h.link.core.short_accepts, 0, "очередь действительно переполнялась")
        self.assertEqual(h.link.core.put_blocks, 0, "блоку в такой очереди нет места")

    def test_stuck_esp_queue_ends_with_an_error(self) -> None:
        h = self.harness(core=LinkCore(from_capacity=64))
        h.call("ZiFi_Init")
        h.link.app_reads = False
        h.send(0x57, bytes(300))
        self.assertTrue(h.carry, "ESP жива, но данные не берёт")
        self.assertEqual(h.memory[h.symbol("SpiTxLen")], 0, "обрывок кадра выброшен")
        h.link.app_reads = True
        h.link.pump()
        h.tx.clear()
        h.send(CMD_PING)
        self.assertFalse(h.carry)
        self.assertEqual(h.frames(), [(CMD_PING, b"")])

    def test_silent_esp_and_recovery(self) -> None:
        h = self.harness()
        h.call("ZiFi_Init")
        h.link.die()
        h.send(CMD_PING)
        self.assertTrue(h.carry, "ESP молчит")
        self.assertEqual(h.memory[h.symbol("SpiTxLen")], 0)
        polls = h.rx_polls
        self.assertIsNone(h.poll(5))
        self.assertEqual(h.rx_polls - polls, 5, "без ESP опрос — одно чтение состояния, без обменов")
        h.revive()
        h.call("ZiFi_Init")
        self.assertFalse(h.carry)
        h.send(CMD_PING)
        self.assertEqual(h.frames(), [(CMD_PING, b"")])

    def test_rx_ready_tells_without_taking(self) -> None:
        h = self.harness()
        h.call("ZiFi_Init")
        h.call("ZiFi_RxReady", de=0xDEDE, hl=0x4C4C, bc=0xBCBC)
        self.assertTrue(h.zero, "данных нет")
        self.assertEqual((h.machine.bc, h.machine.de, h.machine.hl), (0xBCBC, 0xDEDE, 0x4C4C))
        h.rx += frame(RESP_READY)
        h.call("ZiFi_RxReady", de=0xDEDE, hl=0x4C4C, bc=0xBCBC)
        self.assertFalse(h.zero, "данные есть")
        self.assertEqual((h.machine.bc, h.machine.de, h.machine.hl), (0xBCBC, 0xDEDE, 0x4C4C))
        self.assertEqual(len(h.link.core.to_z80), 5, "кадр ещё у ESP")
        self.assertEqual(h.poll(), (RESP_READY, b""))

    def test_registers_survive_the_calls(self) -> None:
        h = self.harness()
        h.call("ZiFi_Init")
        for count in range(60):                 # с обменом на 55-м байте
            h.call("ZiFi_PutChar", a=count, de=0x1122, hl=0x3344, bc=0x5566)
            self.assertFalse(h.carry)
            self.assertEqual((h.machine.a, h.machine.bc, h.machine.de, h.machine.hl),
                             (count, 0x5566, 0x1122, 0x3344))
        h.call("ZiFi_Flush", de=0x1122, hl=0x3344, bc=0x5566)
        self.assertFalse(h.carry)
        self.assertEqual((h.machine.bc, h.machine.de, h.machine.hl), (0x5566, 0x1122, 0x3344))
        self.assertEqual(bytes(h.tx), bytes(range(60)))
        h.rx += b"abc"
        buffer = h.symbol("ProtoBurstBuf")
        h.call("ZiFi_ReadBurst", de=0x1122, hl=buffer, bc=0x5566)
        self.assertEqual((h.machine.bc, h.machine.de), (3, 0x1122))
        self.assertFalse(h.zero)
        self.assertEqual(bytes(h.memory[buffer:buffer + 3]), b"abc")
        h.call("ZiFi_ReadBurst", de=0x1122, hl=buffer, bc=0x5566)
        self.assertEqual((h.machine.bc, h.machine.de), (0, 0x1122))
        self.assertTrue(h.zero)

    def test_every_read_starts_at_a_marker(self) -> None:
        # По первому байту драйвер отличает данные от того, что Evo отдаёт до
        # них: у признака половины байта взаимно обратны, у неподвижной линии —
        # нет.
        for marker in (REQUEST, RESPONSE, STATE_EMPTY, STATE_PENDING):
            self.assertEqual(marker >> 4 ^ marker & 0x0F, 0x0F, f"#{marker:02X}")
        for level in (0xFF, 0x00):
            self.assertNotEqual(level >> 4 ^ level & 0x0F, 0x0F)

    def test_data_start_is_found_whatever_comes_before_it(self) -> None:
        # Перед данными RDBUF Evo отдаёт два ничего не значащих байта либо один,
        # а линия MISO до данных стоит в любом уровне (с ранним выводом бита её
        # последний такт может достаться уже данным).
        rng = random.Random(4)
        for current in (False, True):
            for idle in (0xFF, 0x00, 0xFE, 0x01, 0x7F, 0x80):
                with self.subTest(current=current, idle=idle):
                    h = self.harness()
                    h.ports.current = current
                    h.slave.idle = idle
                    h.call("ZiFi_Init")
                    self.assertFalse(h.carry, "прошивка найдена")
                    payload = rng.randbytes(130)
                    h.send(0x11, payload)
                    self.assertFalse(h.carry)
                    self.assertEqual(h.frames(), [(0x11, payload)])
                    h.rx += frame(0x92, payload)
                    self.assertEqual(h.poll(), (0x92, payload))
                    h.call("ZiFi_RxReady")
                    self.assertTrue(h.zero, "данных больше нет")
                    # Чтение состояния — самое частое обращение: один байт
                    # данных, если Evo отдаёт его сразу, иначе следом
                    # выдвигается ненужный 61-й.
                    self.assertEqual(h.slave.transactions[-1],
                                     (CMD_RDBUF, STATE_ADDR, 1 if current else 2))

    def test_long_frames_come_in_dma_blocks(self) -> None:
        rng = random.Random(5)
        h = self.harness()
        h.call("ZiFi_Init")
        buffer = h.symbol("ProtoBurstBuf")
        self.assertEqual(buffer & 1, 0, "приёмник DMA — с чётного адреса")
        self.assertGreaterEqual(buffer, 0x8200, "чтения для кэша идут из #8000..#81FF")
        for length in (507, 508, 1019, 1024):
            with self.subTest(length=length):
                payload = rng.randbytes(length)
                blocks, runs = h.link.core.bulk_blocks, len(h.ports.dma_runs)
                turns = h.link.core.turns
                # Три кадра подряд: очередь длиннее блока даже для короткого кадра.
                for _ in range(3):
                    h.rx += frame(0x92, payload)
                for _ in range(3):
                    self.assertEqual(h.poll(), (0x92, payload))
                total = 3 * (length + 5)
                self.assertEqual(h.link.core.bulk_blocks - blocks, total // BULK_SIZE,
                                 "всё, что набирается на блок, идёт блоками")
                # Каждый блок принят и отослан обратно.
                self.assertEqual(spi_runs(h, runs),
                                 [(0x02, BULK_SIZE), (0x82, BULK_SIZE)] * (total // BULK_SIZE))
                # Данные кадра из порции в буфер кадра переносит DMA.
                self.assertIn(0x01, {mode for mode, _ in h.ports.dma_runs[runs:]})
                # Обмен на блок (итог сравнения и следующий блок — одним ответом),
                # остаток меньше блока — обычными обменами по 54 байта.
                tail = -(-(total % BULK_SIZE) // CHUNK)
                self.assertLessEqual(h.link.core.turns - turns, total // BULK_SIZE + tail + 4)
        self.assertIsNone(h.poll(3), "лишних кадров нет")
        self.assertFalse(h.link.bulk_loaded or h.link.echo_wanted or h.link.core.block_out,
                         "каждый блок прочитан, возвращён и подтверждён")
        self.assertTrue(h.link.echo_results and all(h.link.echo_results))
        self.assertEqual(h.link.core.bulk_bad, 0)

    def test_block_read_with_an_error_is_read_again(self) -> None:
        rng = random.Random(8)
        h = self.harness()
        h.call("ZiFi_Init")
        payload = rng.randbytes(3000)
        h.rx += frame(0x92, payload[:1000]) + frame(0x93, payload[1000:2000])
        h.rx += frame(0x94, payload[2000:])
        h.link.pump()
        h.link.corrupt_blocks = 2       # два блока подряд Z80 прочитает неверно
        self.assertEqual(h.poll(), (0x92, payload[:1000]))
        self.assertEqual(h.poll(), (0x93, payload[1000:2000]))
        self.assertEqual(h.poll(), (0x94, payload[2000:]))
        core = h.link.core
        self.assertEqual((core.bulk_bad, core.bulk_gave_ups), (2, 0))
        self.assertEqual(h.link.echo_results.count(False), 2)
        self.assertEqual(len(spi_runs(h)), 2 * (core.bulk_blocks + 2))

    def test_dma_that_keeps_failing_is_given_up(self) -> None:
        # Блок раз за разом читается неверно: ESP отказывается от DMA, и те же
        # байты приходят обычными ответами. Ни один байт не потерян и не удвоен.
        rng = random.Random(9)
        h = self.harness()
        h.call("ZiFi_Init")
        payload = rng.randbytes(2600)
        h.rx += frame(0x92, payload[:1000]) + frame(0x93, payload[1000:2000])
        h.rx += frame(0x94, payload[2000:])
        h.link.pump()
        h.link.corrupt_blocks = 1000
        self.assertEqual(h.poll(), (0x92, payload[:1000]))
        self.assertEqual(h.poll(), (0x93, payload[1000:2000]))
        self.assertEqual(h.poll(), (0x94, payload[2000:]))
        core = h.link.core
        self.assertEqual((core.bulk_blocks, core.bulk_gave_ups), (1, 1))
        self.assertEqual(len(spi_runs(h)), 6, "три попытки чтения с обратной посылкой")
        # Новый сеанс пробует DMA заново.
        h.link.corrupt_blocks = 0
        h.call("ZiFi_Init")
        h.rx += frame(0x95, payload[:1000])
        self.assertEqual(h.poll(), (0x95, payload[:1000]))
        self.assertEqual(core.bulk_blocks, 2)

    def test_block_waits_while_the_program_sends(self) -> None:
        # Ответ, подтвердивший блок, сообщил и о следующем. Программа, получив
        # кадр, сперва шлёт свой: выставленный блок от этого не теряется.
        rng = random.Random(10)
        h = self.harness()
        h.call("ZiFi_Init")
        first, second = rng.randbytes(507), rng.randbytes(700)
        h.rx += frame(0x92, first) + frame(0x93, second)
        h.link.pump()
        self.assertEqual(h.poll(), (0x92, first))
        self.assertTrue(h.link.bulk_loaded, "следующий блок уже выставлен")
        h.send(0x21, b"reply")
        self.assertEqual(h.poll(), (0x93, second))
        self.assertEqual(bytes(h.link.tx), frame(0x21, b"reply"))
        self.assertEqual(h.link.core.bulk_bad, 0)

    def test_firmware_without_dma_blocks_still_works(self) -> None:
        # Прошивка старше блоков флага не знает и отвечает данными, как раньше.
        h = self.harness()
        h.link.bulk = False
        h.call("ZiFi_Init")
        payload = bytes(range(256)) * 4
        h.rx += frame(0x92, payload)
        self.assertEqual(h.poll(), (0x92, payload))
        self.assertEqual(h.ports.dma_runs, [])

    def test_block_left_by_the_previous_program_is_dropped(self) -> None:
        # Прежнюю программу сняли, когда блок уже был выставлен, но не прочитан.
        h = self.harness()
        h.call("ZiFi_Init")
        h.slave.dma_tx = bytes([0xEE]) * (BULK_SIZE + 4)
        h.link.bulk_loaded = True
        h.call("ZiFi_Init")             # новый сеанс: очередь ESP пуста
        payload = bytes(index * 5 & 0xFF for index in range(1024))
        h.rx += frame(0x92, payload)
        self.assertEqual(h.poll(), (0x92, payload), "чужой блок в кадр не попал")
        self.assertEqual(spi_runs(h)[0], (0x02, BULK_SIZE))
        self.assertEqual(spi_runs(h)[1:3], [(0x02, BULK_SIZE), (0x82, BULK_SIZE)],
                         "чужой блок прочитан и выброшен без обратной посылки")
        self.assertEqual(h.link.core.bulk_bad, 0)

    def test_interrupts_stay_as_they_were(self) -> None:
        # На время чтений для кэша стек чужой и прерывания закрыты; после —
        # как было у вызывающего.
        h = self.harness()
        h.call("ZiFi_Init")
        for enabled in (True, False):
            with self.subTest(enabled=enabled):
                runs = len(h.ports.dma_runs)
                h.rx += frame(0x92, bytes(1000))
                h.link.pump()
                h.machine.iff1 = h.machine.iff2 = enabled
                self.assertIsNotNone(h.poll())
                self.assertGreater(len(h.ports.dma_runs), runs)
                self.assertEqual(bool(h.machine.iff1), enabled)

    def test_long_frames_go_to_the_esp_in_dma_blocks(self) -> None:
        # Блоки DMA читают данные прямо там, где их оставил вызывающий, — в
        # окне #C000 (send кладёт их туда) или #8000; заголовок кадра уходит в
        # запросе первого блока, раньше него.
        rng = random.Random(21)
        h = self.harness()
        h.call("ZiFi_Init")
        for length in (507, 508, 511, 512, 1019, 1020, 1024, 1500):
            with self.subTest(length=length):
                payload = rng.randbytes(length)
                runs, puts = len(h.ports.dma_runs), h.link.core.put_blocks
                h.link.tx.clear()
                h.send(0x31, payload)
                h.link.pump()
                self.assertEqual(bytes(h.link.tx), frame(0x31, payload))
                blocks = length // BULK_SIZE
                self.assertEqual(h.link.core.put_blocks - puts, blocks)
                self.assertEqual(h.ports.dma_runs[runs:], [(0x82, BULK_SIZE)] * blocks)
        self.assertEqual(h.link.core.put_bad, 0)

    def test_blocks_go_from_either_window_and_odd_addresses(self) -> None:
        # Окно #8000 — своя страница плагина (WC_PAGE2), #C000 — WC_PAGE3. С
        # нечётного адреса один байт уходит обычным обменом, блок — со
        # следующего, чётного.
        rng = random.Random(23)
        h = self.harness()
        h.call("ZiFi_Init")
        h.memory[WC_PAGE3] = 0x30
        h.ports.windows = {page: base for page, base in h.ports.windows.items() if base != 0xC000}
        h.ports.windows[0x30] = 0xC000
        free = h.symbol("ProtoBuf")             # 1025 байтов буфера приёма свободны
        for address, length, blocks in ((0xC000, 1024, 2), (0xC001, 1025, 2),
                                        (0xC001, 1024, 1), (0xC001, 512, 0),
                                        (free, 1024, 2), (free + 1, 600, 1)):
            with self.subTest(address=hex(address), length=length):
                payload = rng.randbytes(length)
                h.memory[address:address + length] = payload
                puts = h.link.core.put_blocks
                h.link.tx.clear()
                h.call("Proto_Send", a=0x31, hl=address, bc=length)
                self.assertFalse(h.carry)
                h.link.pump()
                self.assertEqual(bytes(h.link.tx), frame(0x31, payload))
                self.assertEqual(h.link.core.put_blocks - puts, blocks)
        self.assertEqual(h.link.core.put_bad, 0)

    def test_frame_in_two_parts_and_held_checksum(self) -> None:
        # Proto_Send2: заголовок из одного места, данные из другого; с
        # ProtoHold сумма кадра остаётся в накопителе и уходит со следующим.
        rng = random.Random(24)
        h = self.harness()
        h.call("ZiFi_Init")
        head, data = rng.randbytes(9), rng.randbytes(1024)
        h.memory[0xC000:0xC000 + 1024] = data
        head_at = h.symbol("ProtoBuf")
        h.memory[head_at:head_at + 9] = head
        tail = h.symbol("ProtoTail")
        h.memory[tail:tail + 2] = (0xC000).to_bytes(2, "little")
        h.memory[tail + 2:tail + 4] = (1024).to_bytes(2, "little")
        h.memory[h.symbol("ProtoHold")] = 1
        h.link.tx.clear()
        turns = h.link.core.turns
        h.call("Proto_Send2", a=0x58, hl=head_at, bc=9)
        self.assertFalse(h.carry)
        h.link.pump()
        whole = frame(0x58, head + data)
        self.assertEqual(bytes(h.link.tx), whole[:-1], "сумма кадра ещё у Z80")
        self.assertEqual(h.memory[h.symbol("SpiTxLen")], 1)
        self.assertEqual(h.link.core.turns - turns, 4, "два блока, заголовок — в запросе первого")
        self.assertEqual(h.link.core.put_blocks, 2)
        h.send(CMD_PING)                        # обычный Proto_Send досылает
        h.link.pump()
        self.assertEqual(bytes(h.link.tx), whole + frame(CMD_PING))

    def test_block_that_arrives_damaged_is_sent_again(self) -> None:
        h = self.harness()
        h.call("ZiFi_Init")
        payload = bytes(index * 7 & 0xFF for index in range(1024))
        h.link.corrupt_puts = 2
        h.send(0x31, payload)
        h.link.pump()
        self.assertEqual(bytes(h.link.tx), frame(0x31, payload))
        self.assertEqual(h.link.core.put_bad, 2)
        self.assertEqual(h.link.core.put_blocks, 2)

    def test_block_that_never_matches_goes_by_the_mailbox(self) -> None:
        h = self.harness()
        h.call("ZiFi_Init")
        payload = bytes(index * 3 & 0xFF for index in range(1000))
        h.link.corrupt_puts = 1000
        h.send(0x31, payload)
        h.link.pump()
        self.assertEqual(bytes(h.link.tx), frame(0x31, payload))
        # Блок — три попытки, потом обычные обмены; остаток меньше блока.
        self.assertEqual((h.link.core.put_blocks, h.link.core.put_bad), (0, 3))

    def test_firmware_without_dma_receive_gets_the_mailbox(self) -> None:
        # Прошивка до 0.1.18 не ставит «умею»: байт 4 она приняла бы за место.
        h = self.harness()
        h.link.put = False
        h.call("ZiFi_Init")
        payload = bytes(range(256)) * 6
        h.send(0x31, payload)
        h.link.pump()
        self.assertEqual(bytes(h.link.tx), frame(0x31, payload))
        self.assertEqual([run for run in h.ports.dma_runs if run[0] == 0x82], [])

    def test_send_and_receive_blocks_interleave(self) -> None:
        # Приём и отправка через DMA делят один приёмник ESP: блок к Z80
        # (обратная посылка) и блок от Z80 не путаются.
        rng = random.Random(22)
        h = self.harness()
        h.call("ZiFi_Init")
        for _ in range(3):
            incoming, outgoing = rng.randbytes(1000), rng.randbytes(1024)
            h.rx += frame(0x92, incoming)
            self.assertEqual(h.poll(), (0x92, incoming))
            h.link.tx.clear()
            h.send(0x31, outgoing)
            h.link.pump()
            self.assertEqual(bytes(h.link.tx), frame(0x31, outgoing))
        self.assertEqual((h.link.core.bulk_bad, h.link.core.put_bad), (0, 0))

    def test_port_behaviour_may_change_on_the_fly(self) -> None:
        # Скорость Z80 переключили, пока программа работает: чтение #57 стало
        # отдавать другой байт. Начало данных ищется при каждом чтении.
        h = self.harness()
        h.call("ZiFi_Init")
        for step in range(6):
            h.ports.current = step % 2 == 0
            payload = bytes([step]) * (20 + step)
            h.send(0x11, payload)
            self.assertFalse(h.carry)
            h.rx += frame(0x92, payload)
            self.assertEqual(h.poll(), (0x92, payload))
        self.assertEqual(h.frames(), [(0x11, bytes([step]) * (20 + step)) for step in range(6)])

    def test_only_documented_bus_accesses(self) -> None:
        h = self.harness()
        h.call("ZiFi_Init")
        h.send(0x11, bytes(200))
        h.rx += frame(0x92, bytes(200))
        h.poll()
        commands = {(command, address) for command, address, _ in h.slave.transactions}
        self.assertEqual(commands, {(CMD_WRBUF, 0), (CMD_RDBUF, 0), (CMD_RDBUF, STATE_ADDR)})
        # Кадр длиннее блока: к тем же обращениям добавляются чтение блока
        # через DMA, его обратная посылка и их завершения; адрес у них не значим.
        h.rx += frame(0x92, bytes(1000))
        h.poll()
        commands = {(command, address) for command, address, _ in h.slave.transactions}
        self.assertEqual(commands, {(CMD_WRBUF, 0), (CMD_RDBUF, 0), (CMD_RDBUF, STATE_ADDR),
                                    (CMD_RDDMA, 0xFF), (CMD_RD_DONE, 0xFF),
                                    (CMD_WRDMA, 0xFF), (CMD_WR_DONE, 0xFF)})
        self.assertEqual(max(count for command, _, count in h.slave.transactions
                             if command == CMD_WRBUF), 60, "запрос не длиннее области обмена")
        self.assertEqual(h.ports.cfg_writes.count(CFG_HOST_MASTER), 1)


def spi_variant(case: type, plugins: tuple = ()) -> type:
    """Тот же набор сценариев на сборках для VDAC4 и стенде SPI."""

    class Variant(case):
        @classmethod
        def setUpClass(cls) -> None:
            need(FTP_WMF, FTP_SYM, SMB_WMF, SMB_SYM)
            names = ("Harness", "WMF", "SYM", "SMB_WMF", "SMB_SYM")
            cls._saved = {name: getattr(base, name) for name in names}
            base.Harness, base.WMF, base.SYM = SpiHarness, FTP_WMF, FTP_SYM
            base.SMB_WMF, base.SMB_SYM = SMB_WMF, SMB_SYM
            try:
                super().setUpClass()
            except BaseException:
                cls._restore()
                raise

        @classmethod
        def _restore(cls) -> None:
            for name, value in cls._saved.items():
                setattr(base, name, value)

        @classmethod
        def tearDownClass(cls) -> None:
            super().tearDownClass()
            cls._restore()

    if plugins:
        Variant.PLUGINS = plugins
    Variant.__name__ = Variant.__qualname__ = "Spi" + case.__name__
    Variant.__doc__ = (case.__doc__ or "") + "\n\n    Сборка для VDAC4, канал SPI."
    return Variant


class SpiFtpExitRefreshTest(spi_variant(base.FtpExitRefreshTest)):
    def test_header_version(self) -> None:
        data = FTP_WMF.read_bytes()
        self.assertEqual(data[165:197].rstrip(b" "), b"VDAC4 FTP Server v0.15")
        self.assertEqual(data[197], 3, "плагин меню F10")


SpiFtpCrcTest = spi_variant(base.FtpCrcTest)
SpiFtpDatesTest = spi_variant(base.FtpDatesTest)
SpiFtpWindowTest = spi_variant(base.FtpWindowTest)
SpiSmbPluginDatesTest = spi_variant(base.SmbPluginDatesTest)
SpiWifiSignalColorTest = spi_variant(
    base.WifiSignalColorTest, ((FTP_WMF, FTP_SYM, "FTP"), (SMB_WMF, SMB_SYM, "SMB")))
SpiSmbPluginWaitTest = spi_variant(base.SmbPluginWaitTest)
SpiSmbPluginDirBatchTest = spi_variant(base.SmbPluginDirBatchTest)
SpiFtpPluginWaitTest = spi_variant(base.FtpPluginWaitTest)
SpiFtpPluginDirBatchTest = spi_variant(base.FtpPluginDirBatchTest)


class SmbLayoutTest(unittest.TestCase):
    """Плагин SMB занимает страницу почти целиком."""

    @classmethod
    def setUpClass(cls) -> None:
        need(SMB_WMF, SMB_SYM)

    def test_plugin_fits_its_page(self) -> None:
        symbol = base.Symbols(SMB_SYM)
        self.assertLessEqual(symbol("mainEnd"), 0xC000)
        # CRC16 в сборке для платы нет вовсе: таблиц и их выравнивания тоже.
        with self.assertRaises(AssertionError):
            symbol("VfsCrcHi")
        # Блоки к ESP уходят прямо из памяти: своего буфера у них нет.
        with self.assertRaises(AssertionError):
            symbol("SpiPutBuf")
        self.assertEqual(symbol("ProtoBuf") & 1, 0, "в буфер кадра переносит DMA")
        # Буфера старого пути BLOCK в сборке платы нет.
        with self.assertRaises(AssertionError):
            symbol("VfsCompBuf")
        buffer = symbol("ProtoBurstBuf")
        self.assertEqual(buffer & 1, 0, "приёмник DMA — с чётного адреса")
        self.assertGreaterEqual(buffer, 0x8200, "чтения для кэша идут из #8000..#81FF")
        self.assertLessEqual(buffer + BULK_SIZE, 0xC000)


VFS_WRITE_WINDOW, VFS_READ_WINDOW = 0x57, 0x58
WINDOW_START, WINDOW_END = 0x01, 0x02
FN_MNGC_PL, FN_LOAD512, FN_APPEND, FN_MNG8_PL = 0x00, 0x30, 0x4C, 0x4F
WC_PAGE3 = 0x6003
WINDOW = 0x4000                         # окно 16 КиБ: page1 плагина


def padded(data: bytes) -> bytes:
    """Кадр прошивки 0.1.19 и новее: дополнен нулём до чётной длины."""
    return data + bytes(len(data) & 1)


class WindowHarness(SpiHarness):
    """Плагин с окнами чтения и записи: страницы WC и файл за LOAD512/APPEND.

    Окна #8000 и #C000 переключаются по-настоящему: содержимое уходящей
    страницы сохраняется, приходящей — ставится на место, ячейки WC #6002 и
    #6003 и окна DMA следуют за ними. Одна страница может стоять в обоих окнах
    (трамплин LOAD512/APPEND исполняется из копии страницы кода в #C000)."""

    def __init__(self, wmf: pathlib.Path = FTP_WMF, sym: pathlib.Path = FTP_SYM,
                 core: LinkCore | None = None) -> None:
        super().__init__(wmf, sym, core)
        self.pages: dict[int, bytes] = {}
        self.mapped = {0x8000: self.PLUGIN_PAGE, 0xC000: None}
        self.file = b""                 # что отдаёт LOAD512
        self.file_pos = 0
        self.appended = bytearray()     # что записали APPEND
        self.map_page(0xC000, self.PLUGIN_PAGE + 1)

    def map_page(self, base: int, page: int) -> None:
        other = 0xC000 if base == 0x8000 else 0x8000
        old = self.mapped[base]
        if old is not None:
            self.pages[old] = bytes(self.memory[base:base + WINDOW])
        if self.mapped[other] == page:
            self.pages[page] = bytes(self.memory[other:other + WINDOW])
        self.memory[base:base + WINDOW] = self.pages.get(page, bytes(WINDOW))
        self.mapped[base] = page
        self.memory[self.WC_PAGE2 if base == 0x8000 else WC_PAGE3] = page
        self.ports.windows = {}
        for window in (0xC000, 0x8000):     # одна страница в двух окнах — DMA видит #8000
            if self.mapped[window] is not None:
                self.ports.windows[self.mapped[window]] = window

    def handle_api(self) -> None:
        m = self.machine
        api = m.a
        if api in (FN_MNGC_PL, FN_MNG8_PL):
            self.calls.append(api)
            index = self.memory[base.ALT_A_CELL]
            self.map_page(0xC000 if api == FN_MNGC_PL else 0x8000, self.PLUGIN_PAGE + index)
        elif api == FN_LOAD512:
            self.calls.append(api)
            count = m.b * 512
            chunk = self.file[self.file_pos:self.file_pos + count]
            self.file_pos += count
            self.memory[m.hl:m.hl + count] = chunk.ljust(count, b"\0")
            m.a = 0
            self.flags()
        elif api == FN_APPEND:
            self.calls.append(api)
            if not self.ports.clean(m.hl, m.hl + m.bc):
                raise AssertionError("WC пишет в файл то, что DMA оставил мимо кэша")
            self.appended += self.memory[m.hl:m.hl + m.bc]
            self.flags(zero=True)
        else:
            super().handle_api()
            return
        self.ret()

    def set_word(self, name: str, value: int, size: int = 2) -> None:
        address = self.symbol(name)
        self.memory[address:address + size] = value.to_bytes(size, "little")


class WindowTest(unittest.TestCase):
    """Окна чтения и записи FTP и SMB целиком: страница данных, кадры, файл.

    Заодно — такты Z80 на байт (WC и карта в стенде ничего не стоят)."""

    PLUGINS = ((FTP_WMF, FTP_SYM, "FTP"), (SMB_WMF, SMB_SYM, "SMB"))
    SIZE = 2 * WINDOW + 700             # два полных окна и неполное
    WRITE_PART = 1016                   # кусок окна записи у прошивки (kWriteWindowData)
    ticks: dict[str, float] = {}

    @classmethod
    def setUpClass(cls) -> None:
        need(FTP_WMF, FTP_SYM, SMB_WMF, SMB_SYM)

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.ticks:
            print("\nтактов Z80 на байт: " + ", ".join(
                f"{name} {value:.1f}" for name, value in sorted(cls.ticks.items())))

    def read_file(self, h: WindowHarness, data: bytes) -> tuple[bytes, int]:
        h.file, h.file_pos = data, 0
        h.set_word("FileRemaining", len(data), 4)
        h.memory[h.symbol("VfsMode")] = 0
        seq = h.memory[h.symbol("VfsReadSeq")]
        got, ticks, offset = bytearray(), 0, 0
        while offset < len(data):
            count = min(WINDOW, len(data) - offset)
            h.set_request(bytes([seq]) + count.to_bytes(2, "little"))
            h.tx.clear()
            before = h.ticks
            h.call("Vfs_ReadWindow")
            ticks += h.ticks - before
            h.link.pump()
            part_offset = 0
            for command, payload in h.frames():
                self.assertEqual(command, VFS_READ_WINDOW)
                self.assertEqual(payload[0], 0, "статус")
                flags = payload[1]
                part = payload[9:]
                self.assertEqual(payload[2], seq)
                self.assertEqual(int.from_bytes(payload[3:5], "little"), part_offset)
                self.assertEqual(int.from_bytes(payload[5:7], "little"), count)
                self.assertEqual(bool(flags & WINDOW_START), part_offset == 0)
                self.assertEqual(bool(flags & WINDOW_END), part_offset + len(part) == count)
                got += part
                part_offset += len(part)
            self.assertEqual(part_offset, count, "окно отдано целиком")
            seq = (seq + 1) & 0xFF
            offset += count
        return bytes(got), ticks

    def write_file(self, h: WindowHarness, data: bytes, pad: bool) -> int:
        for name in ("VfsWriteFailed", "VfsExpectedSeq", "VfsWindowActive"):
            h.memory[h.symbol(name)] = 0
        h.memory[h.symbol("VfsMode")] = 1
        h.set_word("VfsWriteCount", 0)
        ticks, offset, seq = 0, 0, 0
        while offset < len(data):
            total = min(WINDOW, len(data) - offset)
            window = data[offset:offset + total]
            crc = base.crc16_ccitt_false(window).to_bytes(2, "little")
            for start in range(0, total, self.WRITE_PART):
                part = window[start:start + self.WRITE_PART]
                flags = ((WINDOW_START if start == 0 else 0) |
                         (WINDOW_END if start + len(part) == total else 0))
                payload = (bytes([flags, seq]) + start.to_bytes(2, "little") +
                           total.to_bytes(2, "little") + crc + part)
                packet = frame(VFS_WRITE_WINDOW, payload)
                h.rx += padded(packet) if pad else packet
            h.link.pump()
            h.tx.clear()
            before = h.ticks
            h.call("Vfs_Serve")
            ticks += h.ticks - before
            h.link.pump()
            self.assertEqual(h.frames(), [(VFS_WRITE_WINDOW, bytes([0, seq]) +
                                           total.to_bytes(2, "little"))], "окно принято")
            seq += 1
            offset += total
        h.call("Vfs_WriteFlush")            # остаток неполного окна, как при закрытии
        self.assertEqual(bytes(h.appended), data)
        return ticks

    def test_read_windows_carry_the_file(self) -> None:
        for wmf, sym, name in self.PLUGINS:
            with self.subTest(plugin=name):
                h = WindowHarness(wmf, sym)
                h.call("ZiFi_Init")
                data = random.Random(30).randbytes(self.SIZE)
                got, ticks = self.read_file(h, data)
                self.assertEqual(got, data)
                self.assertEqual(h.link.core.put_bad, 0)
                self.ticks[f"чтение {name}"] = ticks / len(data)

    def test_write_windows_land_in_the_file(self) -> None:
        for wmf, sym, name in self.PLUGINS:
            for pad in (False, True):
                with self.subTest(plugin=name, padded=pad):
                    h = WindowHarness(wmf, sym)
                    h.call("ZiFi_Init")
                    data = random.Random(31).randbytes(self.SIZE)
                    ticks = self.write_file(h, data, pad)
                    self.assertEqual(h.link.core.bulk_bad, 0)
                    self.ticks[f"запись {name}{' (кадры чётные)' if pad else ''}"] = ticks / len(data)


class EspApp:
    """Прикладная часть прошивки для сквозных сценариев."""

    SYS_INFO = (b"RAM:201532B PSRAM:8386308/8388608B Flash:8388608B Sketch:1510320B RST:1 "
                b"CORE:SPI+VFS1/NET+EVT0 NETSTK:13120B CTRL:PSRAM VFSBUF:PSRAM:65536+65536 "
                b"BOARD:VDAC4-A3.1 IDF:v6.0.2 LINK:12/0/0 AUDIO:idle PROXY:OFF FW:vdac4-0.1.0")

    def __init__(self, ntp: bytes = b"20261001203045") -> None:
        self.commands: list[int] = []
        self.ini = b""
        self.ntp = ntp

    def __call__(self, command: int, payload: bytes) -> bytes:
        self.commands.append(command)
        if command == CMD_PING:
            return frame(RESP_READY)
        if command == CMD_SYS_INFO:
            return frame(RESP_ACK) + frame(RESP_SYS_INFO, self.SYS_INFO)
        if command == CMD_WIFI_INI:
            self.ini = payload
            return frame(RESP_ACK) + frame(RESP_WIFI_INI, bytes([1, 192, 168, 1, 77]))
        if command == CMD_NET_NTP:
            return frame(RESP_ACK) + frame(RESP_NET_NTP, self.ntp)
        return frame(0xEE, b"unsupported")


class InfoMachine(SpiLinkMixin):
    """Снимок 48K (по умолчанию tools/esp_info_vdac4.sna) на «голом» Spectrum:
    ПЗУ заменено ловушками. stop — метка, на которой программа ждёт клавишу."""

    ROM_PRINT, ROM_CHAN_OPEN, ROM_CLS = 0x0010, 0x1601, 0x0DAF
    PLUGIN_PAGE = None                  # программа без WC: страниц она не знает

    def __init__(self, sna: pathlib.Path = INFO_SNA, sym: pathlib.Path = INFO_SYM,
                 stop: str = "WaitRefresh") -> None:
        data = sna.read_bytes()
        assert len(data) == 27 + 48 * 1024, "ожидался снимок 48K"
        text = sym.read_text(encoding="utf-8", errors="replace")
        self.symbols = {name: int(value, 16) for name, value in
                        re.findall(r"^([\w.]+):\s+EQU\s+0x([0-9A-Fa-f]+)", text, re.M)}
        self.machine = z80.Z80Machine()
        self.memory = self.machine.memory
        self.machine.set_memory_block(0x4000, data[27:])
        self.machine.set_input_callback(self.port_in)
        self.machine.set_output_callback(self.port_out)
        self.stop = self.symbols[stop]
        for address in (self.ROM_PRINT, self.ROM_CHAN_OPEN, self.ROM_CLS, self.stop):
            self.machine.set_breakpoint(address)
        self.rx = bytearray()
        self.tx = bytearray()
        self.rx_polls = 0
        self.esp = None
        self.lines: list[str] = [""]
        self._skip = 0
        self.attach_link()

    def _ret(self) -> None:
        m = self.machine
        m.pc = self.memory[m.sp] | self.memory[(m.sp + 1) & 0xFFFF] << 8
        m.sp = (m.sp + 2) & 0xFFFF

    def _print(self, code: int) -> None:
        if self._skip:
            self._skip -= 1             # параметр управляющего кода
        elif code == 13:
            self.lines.append("")
        elif 16 <= code <= 21:
            self._skip = 1              # INK, PAPER, FLASH, BRIGHT, INVERSE, OVER
        else:
            self.lines[-1] += chr(code)

    def run(self) -> list[str]:
        """Выполнить программу до ожидания клавиши; вернуть строки экрана."""
        m = self.machine
        m.pc = self.symbols["Start"]
        for _ in range(200_000):
            m.ticks_to_stop = 5_000_000
            event = m.run()
            if not event & base.BREAKPOINT_HIT:
                continue
            if m.pc == self.stop:
                return self.lines
            if m.pc == self.ROM_PRINT:
                self._print(m.a)
            elif m.pc == self.ROM_CLS:
                self.lines = [""]
            self._ret()
        raise AssertionError("программа не дошла до ожидания клавиши")


class EspInfoTest(unittest.TestCase):
    """esp_info для VDAC4 целиком: поиск ESP, PING, SYS_INFO, экран."""

    @classmethod
    def setUpClass(cls) -> None:
        need(INFO_SNA, INFO_SYM)

    def test_screen_shows_every_field(self) -> None:
        machine = InfoMachine()
        app = EspApp()
        machine.app = app
        lines = machine.run()
        self.assertEqual(app.commands, [CMD_PING, CMD_SYS_INFO])
        screen = [line for line in lines if line]
        self.assertEqual(screen, [
            "VDAC4 ESP32-S3 information",
            "--------------------------",
            "ESP link over SPI: READY",
            "Firmware: vdac4-0.1.0",
            "Board: VDAC4-A3.1",
            "ESP-IDF: v6.0.2",
            "Free RAM: 201532B",
            "PSRAM: 8386308/8388608B",
            "Flash: 8388608B",
            "Application: 1510320B",
            "Reset reason: 1",
            "Cores: SPI+VFS1/NET+EVT0",
            "Network stack: 13120B",
            "Control buffer: PSRAM",
            "VFS buffers: PSRAM:65536+65536",
            "SPI turns: 12/0/0",
            "Audio: idle",
            "HTTP proxy: OFF",
            "ANY KEY - refresh",
        ])
        self.assertLessEqual(len(lines), 22, "верхняя часть экрана — 22 строки, без прокрутки")
        self.assertTrue(all(len(line) <= 32 for line in lines), "строка не длиннее экрана")
        self.assertEqual(machine.slave.overruns, 0)
        self.assertFalse(machine.slave.selected)

    def test_slow_firmware_changes_nothing(self) -> None:
        machine = InfoMachine()
        machine.link.latency = 9
        machine.link.split_publish = True
        machine.app = EspApp()
        self.assertIn("Firmware: vdac4-0.1.0", machine.run())

    @staticmethod
    def probe(word: bytes, idle: int = 0xFF) -> str:
        """Строка экрана: пять байтов после команды чтения с адреса 60."""
        skipped = 1 if SpiPorts.default_current else 2
        raw = bytes([idle]) * skipped + word[:5 - skipped]
        return "Read " + " ".join(f"{byte:02X}" for byte in raw)

    def test_no_firmware(self) -> None:
        machine = InfoMachine()
        machine.link.die()
        lines = machine.run()
        self.assertIn("VDAC4 firmware not found", lines)
        # Для наладки платы программа показывает, что читается на месте слова
        # состояния: по этим байтам видно, молчит линия или сбоит чтение.
        self.assertIn(self.probe(bytes(4)), lines)
        self.assertIn("Need 5A D7 01 from byte 2 or 3", lines)
        self.assertEqual(machine.ports.cfg_writes[0], CFG_HOST_MASTER)
        self.assertEqual(machine.slave.overruns, 0)
        self.assertFalse(machine.slave.selected)

        silent = InfoMachine()
        silent.link.die()
        silent.slave.buffer[:] = bytes([0xFF]) * 64
        self.assertIn("Read FF FF FF FF FF", silent.run())

    def test_probe_shows_where_the_state_word_is(self) -> None:
        # Слово состояния на месте, а прошивка на запрос не отвечает: по строке
        # видно и само слово, и сколько байтов Evo отдала до него.
        machine = InfoMachine()
        machine.link.die()
        word = bytes([STATE_EMPTY, 0xD7, 0x01, 0x4E])
        machine.slave.buffer[STATE_ADDR:] = word
        lines = machine.run()
        self.assertIn("VDAC4 firmware not found", lines)
        self.assertIn(self.probe(word), lines)
        self.assertEqual(machine.slave.overruns, 0)

    def test_blind_requests_reach_the_esp_when_miso_is_dead(self) -> None:
        # Линия MISO оборвана: Z80 читает одни #FF. Программа всё равно шлёт
        # запросы — ESP их получает, и по светодиоду модуля это видно.
        machine = InfoMachine()
        machine.ports.miso_stuck = 0xFF
        lines = machine.run()
        self.assertIn("VDAC4 firmware not found", lines)
        self.assertIn("Read FF FF FF FF FF", lines)
        self.assertIn("Need 5A D7 01 from byte 2 or 3", lines)
        self.assertIn("Sending requests blind: a blue", lines)
        self.assertIn("LED on the module: ESP got them", lines)
        self.assertTrue(all(len(line) <= 32 for line in lines), "строка не длиннее экрана")
        core = machine.link.core
        # Два запроса по 16 попыток. Запрос начала сеанса повтором не считается,
        # поэтому каждая попытка для ESP — отдельный верный обмен.
        self.assertEqual((core.turns, core.repeats), (2 * 16, 0))
        self.assertEqual(core.bad_requests, 0)
        self.assertEqual(machine.slave.overruns, 0)
        self.assertFalse(machine.slave.selected)

    def test_firmware_without_application_reply(self) -> None:
        # Канал жив, а на PING никто не отвечает: ровно 30 попыток и отказ.
        machine = InfoMachine()
        sent: list[int] = []
        machine.app = lambda command, payload: (sent.append(command), b"")[1]
        lines = machine.run()
        self.assertIn("ESP firmware does not reply", lines)
        self.assertEqual(sent, [CMD_PING] * 30)


class UpdateModeTest(unittest.TestCase):
    """tools/update_vdac4.sna: открыть порт обновления прошивки и показать адрес."""

    @classmethod
    def setUpClass(cls) -> None:
        need(UPDATE_SNA, UPDATE_SYM)

    def machine(self, stop: str) -> InfoMachine:
        return InfoMachine(UPDATE_SNA, UPDATE_SYM, stop)

    def test_shows_the_address_of_the_update_port(self) -> None:
        machine = self.machine("WaitKey")
        commands: list[int] = []

        def app(command: int, payload: bytes) -> bytes:
            commands.append(command)
            if command == CMD_PING:
                return frame(RESP_READY)
            if command == CMD_UPDATE_START:
                return frame(RESP_ACK) + frame(
                    RESP_UPDATE_START, bytes([1, 192, 168, 1, 114]) + (8267).to_bytes(2, "little"))
            return b""
        machine.app = app
        lines = machine.run()
        self.assertEqual(commands, [CMD_PING, CMD_UPDATE_START])
        self.assertIn("VDAC4 firmware update", lines)
        self.assertIn("Server: 192.168.1.114:8267", lines)
        self.assertIn("python tools\\esp_tool.py", lines)
        self.assertIn(" --net 192.168.1.114", lines)
        self.assertTrue(all(len(line) <= 32 for line in lines), "строка не длиннее экрана")
        self.assertLessEqual(len(lines), 22)
        self.assertEqual(machine.slave.overruns, 0)
        self.assertFalse(machine.slave.selected)

    def test_reports_a_refusal_with_the_text_from_the_esp(self) -> None:
        machine = self.machine("WaitAgain")

        def app(command: int, payload: bytes) -> bytes:
            if command == CMD_PING:
                return frame(RESP_READY)
            return (frame(RESP_ACK) + frame(RESP_ERROR, b"ota:no wifi config") +
                    frame(RESP_UPDATE_START, bytes(7)))
        machine.app = app
        lines = machine.run()
        self.assertIn("ESP did not open the port", lines)
        self.assertIn("ESP error: ota:no wifi config", lines)

    def test_no_firmware(self) -> None:
        machine = self.machine("WaitAgain")
        machine.link.die()
        self.assertIn("VDAC4 firmware not found", machine.run())
        self.assertFalse(machine.slave.selected)


AUDIO_SNA = ROOT / "tools" / "audio_test_vdac4.sna"
AUDIO_SYM = ROOT / "tools" / "audio_test_vdac4.sym"
CMD_AUDIO_STATUS, CMD_AUDIO_TONE, CMD_AUDIO_VOLUME, CMD_AUDIO_PROBE = 0x33, 0x34, 0x35, 0x37


class AudioFirmware:
    """Звук прошивки для audio_test_vdac4.sna: состояние, тон, громкость, линии."""

    def __init__(self, state: int = 0, volume: int = 160) -> None:
        self.state = state
        self.volume = volume
        self.commands: list[tuple[int, bytes]] = []
        self.probe = bytes([1]) + bytes(range(150, 174))   # 3,0..3,46 В

    def __call__(self, command: int, payload: bytes) -> bytes:
        self.commands.append((command, payload))
        if command == CMD_PING:
            return frame(RESP_READY)
        if command == CMD_AUDIO_STATUS:
            record = bytearray(24)
            record[0] = self.state
            record[23] = self.volume
            return frame(0xB3, bytes(record))
        if command == CMD_AUDIO_TONE:
            return frame(0xB4, b"\x01")
        if command == CMD_AUDIO_VOLUME:
            self.volume = payload[0]
            return frame(0xB5, b"\x01")
        if command == CMD_AUDIO_PROBE:
            return frame(0xB7, self.probe)
        return frame(0xEE, b"unsupported")


class AudioTestMachine(InfoMachine):
    """audio_test_vdac4.sna с клавишами по списку: GetKey отдаёт их по одной;
    когда список кончился, run() возвращает экран."""

    def __init__(self, keys: str = "") -> None:
        super().__init__(AUDIO_SNA, AUDIO_SYM, "GetKey")
        self.keys = list(keys)

    def run(self) -> list[str]:
        m = self.machine
        m.pc = self.symbols["Start"]
        for _ in range(200_000):
            m.ticks_to_stop = 5_000_000
            event = m.run()
            if not event & base.BREAKPOINT_HIT:
                continue
            if m.pc == self.stop:
                if not self.keys:
                    return self.lines
                m.a = ord(self.keys.pop(0))
            elif m.pc == self.ROM_PRINT:
                self._print(m.a)
            elif m.pc == self.ROM_CLS:
                self.lines = [""]
            self._ret()
        raise AssertionError("программа не дошла до ожидания клавиши")


class AudioTestProgramTest(unittest.TestCase):
    """tools/audio_test_vdac4.sna: тоны в каналы, громкость, тест линий ЦАП."""

    @classmethod
    def setUpClass(cls) -> None:
        need(AUDIO_SNA, AUDIO_SYM)

    def machine(self, keys: str = "", **firmware) -> tuple[AudioTestMachine, AudioFirmware]:
        machine = AudioTestMachine(keys)
        app = AudioFirmware(**firmware)
        machine.app = app
        return machine, app

    def check_screen(self, machine: AudioTestMachine, lines: list[str]) -> None:
        self.assertTrue(all(len(line) <= 32 for line in lines), "строка не длиннее экрана")
        self.assertLessEqual(len(lines), 22, "верхняя часть экрана — 22 строки")
        self.assertEqual(machine.slave.overruns, 0)
        self.assertFalse(machine.slave.selected)

    def test_menu_shows_volume_and_state(self) -> None:
        machine, app = self.machine()
        lines = machine.run()
        self.assertEqual([command for command, _ in app.commands], [CMD_PING, CMD_AUDIO_STATUS])
        self.assertIn("VDAC4 audio test", lines)
        self.assertIn("L - tone 1 kHz, LEFT channel", lines)
        self.assertIn("Volume: 160", lines)
        self.assertIn("State:  ready", lines)
        self.check_screen(machine, lines)

    def test_tones_go_to_the_chosen_channel(self) -> None:
        for key, channels, text in (("l", 1, "LEFT"), ("R", 2, "RIGHT"), ("b", 3, "both")):
            with self.subTest(key=key):
                machine, app = self.machine(key)
                lines = machine.run()
                command, payload = app.commands[-1]
                self.assertEqual(command, CMD_AUDIO_TONE)
                # [каналы][1000 Гц LE16][700 мс LE16][уровень]
                self.assertEqual(payload, bytes([channels, 0xE8, 0x03, 0xBC, 0x02, 160]))
                self.assertTrue(any(text in line for line in lines if line.startswith("State:")))
                self.check_screen(machine, lines)

    def test_volume_steps_and_limits(self) -> None:
        machine, app = self.machine("qqa", volume=240)
        lines = machine.run()
        volumes = [payload[0] for command, payload in app.commands if command == CMD_AUDIO_VOLUME]
        self.assertEqual(volumes, [255, 255, 239])
        self.assertIn("Volume: 239", lines)
        machine, app = self.machine("a", volume=10)
        self.assertIn("Volume: 0", machine.run())

    def test_lines_table(self) -> None:
        machine, app = self.machine("t")
        lines = machine.run()
        self.assertEqual(app.commands[-1][0], CMD_AUDIO_PROBE)
        self.assertIn("others at:  00  01  10  11", lines)
        self.assertIn("BCK  drive 3.0 3.0 3.0 3.0", lines)
        self.assertIn("DIN  pull  3.4 3.4 3.4 3.4", lines)
        self.assertIn("T - again, other key - menu", lines)
        self.check_screen(machine, lines)
        # Звук играет — ESP не меряет.
        machine, app = self.machine("t")
        app.probe = bytes(25)
        self.assertIn("no measurement", machine.run())

    def test_sound_off_is_reported(self) -> None:
        machine, _ = self.machine(state=4)
        lines = machine.run()
        self.assertIn("Sound is off: I2S not started", lines)
        self.check_screen(machine, lines)


def probe_byte(index: int, seed: int) -> int:
    """Узор проверки DMA — как SpiLink::dmaProbeByte в прошивке."""
    return (index + 3 * (index >> 8) + seed) & 0xFF


class ProbeFirmware:
    """Команда LINK_PROBE прошивки поверх модели DMA ведомого SPI."""

    def __init__(self, slave: SlaveHd) -> None:
        self.slave = slave
        self.action = 0
        self.length = 0
        self.seed = 0
        self.spoil_send = lambda data: data      # что на самом деле выставить для чтения
        self.refuse = False

    def __call__(self, command: int, payload: bytes) -> bytes:
        if command == CMD_PING:
            return frame(RESP_READY)
        if command != CMD_LINK_PROBE:
            return b""
        action, length, seed = payload[0], payload[1] | payload[2] << 8, payload[3]
        status, got, errors, first = 1, length, 0, 0
        if self.refuse:
            status = 0
        elif action == 1:
            self.action, self.length, self.seed = action, length, seed
            pattern = bytes(probe_byte(i, seed) for i in range(length + 4))
            self.slave.dma_tx = self.spoil_send(pattern)
            self.slave.dma_done.clear()
        elif action == 2:
            self.action, self.length, self.seed = action, length, seed
            self.slave.dma_rx = bytearray()
            self.slave.dma_done.clear()
        elif action == 3:
            wanted = CMD_RD_DONE if self.action == 1 else CMD_WR_DONE
            got = self.length
            if wanted not in self.slave.dma_done:
                status = 2
            elif self.action == 2:
                data = bytes(self.slave.dma_rx)
                got = len(data)
                bad = [i for i in range(min(got, self.length))
                       if data[i] != probe_byte(i, self.seed)]
                errors, first = len(bad), bad[0] if bad else 0
        return frame(RESP_LINK_PROBE,
                     bytes([status, action]) + got.to_bytes(2, "little") +
                     errors.to_bytes(2, "little") + first.to_bytes(2, "little"))


class DmaProbeTest(unittest.TestCase):
    """tools/dma_probe_vdac4.sna: обмен с ESP через DMA TS-Config в обе стороны."""

    @classmethod
    def setUpClass(cls) -> None:
        need(PROBE_SNA, PROBE_SYM)

    def run_probe(self, prepare=None) -> tuple[InfoMachine, ProbeFirmware, list[str]]:
        machine = InfoMachine(PROBE_SNA, PROBE_SYM, "WaitAgain")
        firmware = ProbeFirmware(machine.slave)
        if prepare is not None:
            prepare(machine, firmware)
        machine.app = firmware
        lines = machine.run()
        self.assertTrue(all(len(line) <= 32 for line in lines), "строка не длиннее экрана")
        self.assertFalse(machine.slave.selected)
        return machine, firmware, lines

    def test_both_directions_pass(self) -> None:
        machine, firmware, lines = self.run_probe()
        screen = [line for line in lines if line]
        self.assertEqual(screen, [
            "VDAC4 DMA probe",
            "---------------",
            "ESP -> Evo, 1024 bytes:",
            " OK",
            " A7 A8 A9 AA AB AC AD AE",
            "Evo -> ESP, 1024 bytes:",
            " ESP got 1024",
            " OK",
            "ANY KEY - again",
        ])
        # Чтение — блоками по 512 в окно #C000, запись — оттуда же.
        self.assertEqual(machine.ports.dma_runs, [(0x02, 1024), (0x82, 1024)])
        self.assertEqual(machine.ports.page3, 0)
        self.assertEqual(bytes(machine.slave.dma_rx),
                         bytes(probe_byte(i, 0x3D) for i in range(1024)))
        commands = [command for command, _, _ in machine.slave.transactions
                    if command in (CMD_RDDMA, CMD_WRDMA, CMD_RD_DONE, CMD_WR_DONE)]
        self.assertEqual(commands, [CMD_RDDMA, CMD_RD_DONE, CMD_WRDMA, CMD_WR_DONE])
        # После последнего нужного байта DMA Evo начинает ещё один обмен.
        self.assertEqual(machine.slave.dma_tx_read, 1025)

    def test_reports_where_the_data_went_wrong(self) -> None:
        # Данные пришли со сдвигом на байт: видно, сколько не совпало и с какого места.
        def prepare(machine, firmware) -> None:
            firmware.spoil_send = lambda data: b"\x00" + data
        _, _, lines = self.run_probe(prepare)
        self.assertIn(" 1024 bad bytes, first at 0", lines)
        self.assertIn(" 00 A7 A8 A9 AA AB AC AD", lines)
        # Вторая половина проверки от первой не зависит.
        self.assertIn(" ESP got 1024", lines)

    def test_refusal(self) -> None:
        def prepare(machine, firmware) -> None:
            firmware.refuse = True
        _, _, lines = self.run_probe(prepare)
        self.assertIn("ESP refused the probe", lines)


class NtpHarness(SpiHarness):
    """Плагин NTP: запуск целиком, с файлом zifi.ini «на карте» и часами."""

    FN_GFILE, FN_GDIR, FN_LOAD512 = 0x3E, 0x3F, 0x30
    INI = b"SSID: home\r\npassword: secret\r\ntime: 3\r\n"

    def __init__(self) -> None:
        super().__init__(NTP_WMF, NTP_SYM)
        self.files = {b"zifi": (0, True), b"zifi.ini": (len(self.INI), False)}
        self.clock: dict[int, int] = {}
        self.clock_register = 0

    def handle_api(self) -> None:
        m = self.machine
        if m.a in (self.FN_GFILE, self.FN_GDIR):
            self.calls.append(m.a)
            self.flags(zero=True)
            self.ret()
        elif m.a == self.FN_LOAD512:
            self.calls.append(m.a)
            self.memory[m.hl:m.hl + 512] = self.INI.ljust(512, b"\0")
            m.hl = (m.hl + 512) & 0xFFFF
            self.ret()
        else:
            super().handle_api()

    def port_in(self, port: int) -> int:
        if port & 0xFF == 0xF7:             # часы Mr.Gluk: #BFF7 — данные
            return self.clock.get(self.clock_register, 0)
        return super().port_in(port)

    def port_out(self, port: int, value: int) -> None:
        if port & 0xFF == 0xF7:
            if port >> 8 == 0xDF:           # #DFF7 — номер регистра
                self.clock_register = value
            elif port >> 8 == 0xBF:
                self.clock[self.clock_register] = value
            return
        super().port_out(port, value)


class WeatherHarness(NtpHarness):
    """Заставка погоды: обмен с ESP (Weather_Fetch) без рисования экрана."""

    FN_ANYK = 45

    def __init__(self, wmf: pathlib.Path, sym: pathlib.Path) -> None:
        SpiHarness.__init__(self, wmf, sym)
        self.files = {b"zifi": (0, True), b"zifi.ini": (len(self.INI), False)}
        self.clock = {}
        self.clock_register = 0
        # Часы стоят на нуле, и экран рисовать незачем: «показанное» время —
        # тоже ноль.
        for name in ("ShownMin", "ShownSec"):
            self.memory[self.symbol(name)] = 0
        # WC вызывает заставку с разрешёнными прерываниями: ожидание идёт по HALT.
        self.machine.iff1 = self.machine.iff2 = 1

    def handle_api(self) -> None:
        if self.machine.a == self.FN_ANYK:
            self.calls.append(self.machine.a)
            self.flags(zero=True)
            self.ret()
        else:
            super().handle_api()


class WeatherApp(EspApp):
    """ESP с записью погоды для заставки."""

    def __init__(self, record: bytes) -> None:
        super().__init__()
        self.record = record

    def __call__(self, command: int, payload: bytes) -> bytes:
        if command == CMD_WEATHER_GET:
            self.commands.append(command)
            return frame(RESP_ACK) + frame(RESP_WEATHER, self.record)
        return super().__call__(command, payload)


class WeatherSaverTest(unittest.TestCase):
    """VDAC4WTH.WMF и VDAC4WT2.WMF: погода с ESP по SPI."""

    SAVERS = ((WEATHER_WMF, WEATHER_SYM, b"VDAC4 Weather Saver v0.1"),
              (WEATHER2_WMF, WEATHER2_SYM, b"VDAC4 Weather 2 (VDAC2) v0.1"))

    @classmethod
    def setUpClass(cls) -> None:
        need(WEATHER_WMF, WEATHER_SYM)
        # Заставку для VDAC2 собрать удаётся не везде: её картинки рисуются
        # шрифтами Windows и на иных версиях шрифтов не влезают в страницы.
        cls.SAVERS = tuple(saver for saver in cls.SAVERS
                           if saver[0].exists() and saver[1].exists())

    def test_header_names_the_board(self) -> None:
        for wmf, _, name in self.SAVERS:
            with self.subTest(wmf=wmf.name):
                data = wmf.read_bytes()
                self.assertEqual(data[165:197].rstrip(b" "), name)
                self.assertEqual(data[197], 2, "экранная заставка")

    def test_weather_comes_over_spi(self) -> None:
        for wmf, sym, _ in self.SAVERS:
            with self.subTest(wmf=wmf.name):
                h = WeatherHarness(wmf, sym)
                size = h.symbol("REC_SIZE")
                record = bytes([1, 1]) + b"Moscow".ljust(24, b"\0") + bytes(range(size - 26))
                app = WeatherApp(record)
                h.app = app
                # Остаток прошлого запуска заставки в счётчике порции.
                h.memory[h.symbol("ProtoBurstLeft") + 1] = 0x7F
                h.call("Weather_Fetch")
                self.assertEqual(app.commands, [CMD_PING, CMD_WIFI_INI, CMD_WEATHER_GET])
                self.assertEqual(app.ini, h.INI, "zifi.ini ушёл в ESP целиком")
                self.assertEqual(h.memory[h.symbol("FetchOk")], 1)
                stored = h.symbol("WeatherRecord")
                self.assertEqual(bytes(h.memory[stored:stored + size]), record)
                self.assertFalse(h.slave.selected)
                self.assertEqual(h.slave.overruns, 0)

    def test_no_firmware_means_no_zifi(self) -> None:
        for wmf, sym, _ in self.SAVERS:
            with self.subTest(wmf=wmf.name):
                h = WeatherHarness(wmf, sym)
                h.link.die()
                h.call("Weather_Fetch")
                self.assertTrue(h.carry)
                self.assertEqual(h.memory[h.symbol("FetchOk")], 0)


class NtpPluginTest(unittest.TestCase):
    """VDAC4NTP.WMF: от запуска до записи часов."""

    @classmethod
    def setUpClass(cls) -> None:
        need(NTP_WMF, NTP_SYM)

    def test_header_names_the_board(self) -> None:
        data = NTP_WMF.read_bytes()
        self.assertEqual(data[165:197].rstrip(b" "), b"VDAC4 NTP Sync v0.1")

    def test_clock_is_set_from_the_reply(self) -> None:
        h = NtpHarness()
        app = EspApp(ntp=b"20261001203045")
        h.app = app
        h.run(base.CODE, a=6)
        self.assertEqual(app.commands, [CMD_PING, CMD_WIFI_INI, CMD_NET_NTP])
        self.assertEqual(app.ini, h.INI, "zifi.ini ушёл в ESP целиком")
        time = h.symbol("NtpTime")
        self.assertEqual(bytes(h.memory[time:time + 6]),
                         bytes([0x26, 0x10, 0x01, 0x20, 0x30, 0x45]), "время разобрано в BCD")
        # Регистры часов: год, месяц, число, часы, минуты, секунды; #0B — режим.
        self.assertEqual(h.clock, {0x09: 0x26, 0x08: 0x10, 0x07: 0x01, 0x04: 0x20,
                                   0x02: 0x30, 0x00: 0x45, 0x0B: 0x02})
        self.assertFalse(h.slave.selected)
        self.assertEqual(h.slave.overruns, 0)

    def test_silent_when_there_is_no_firmware(self) -> None:
        h = NtpHarness()
        h.link.die()
        h.run(base.CODE, a=6)
        self.assertEqual(h.calls, [], "без ESP плагин выходит, не трогая ни карту, ни часы")


def current_byte_variant(case: type) -> type:
    """Те же сценарии, когда чтение #57 отдаёт байт обмена, начатого им самим."""

    class Variant(case):
        def setUp(self) -> None:
            SpiPorts.default_current = True
            self.addCleanup(setattr, SpiPorts, "default_current", False)
            super().setUp()

    Variant.__name__ = Variant.__qualname__ = case.__name__.replace("Test", "CurrentByteTest")
    Variant.__doc__ = (case.__doc__ or "") + "\n\n    Чтение #57 отдаёт байт уже начатого обмена."
    return Variant


SpiDriverCurrentByteTest = current_byte_variant(SpiDriverTest)
EspInfoCurrentByteTest = current_byte_variant(EspInfoTest)
UpdateModeCurrentByteTest = current_byte_variant(UpdateModeTest)
DmaProbeCurrentByteTest = current_byte_variant(DmaProbeTest)
NtpPluginCurrentByteTest = current_byte_variant(NtpPluginTest)


if __name__ == "__main__":
    unittest.main()
