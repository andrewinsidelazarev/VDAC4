"""Модель канала SPI платы VDAC4 для машинных тестов.

Три слоя, как на железе:

- LinkCore — эталон правил обмена (src/spi_link_core.cpp). Его сверяет с
  настоящим C++-кодом tests/test_spi_link_host.py;
- SlaveHd — общий буфер ESP32-S3 в режиме SPI Slave HD на уровне байтов SPI:
  команда, адрес, пустой байт, данные;
- EspSide — «прошивка»: замечает записанный запрос, с задержкой отвечает на
  него и ведёт байт состояния. Умеет портить обмен: не заметить запрос,
  ответить не сразу, показать испорченный ответ.

Порты #57/#77 TS-Config поверх этого моделирует SpiPorts.
"""
from __future__ import annotations

REQUEST, RESPONSE = 0xC3, 0x3C
HEADER, CHUNK, TURN_SIZE, STATE_ADDR, BUFFER_SIZE = 5, 54, 60, 60, 64
STATE_EMPTY, STATE_PENDING, MAGIC, VERSION = 0x5A, 0xA5, 0xD7, 2
FLAG_DROP, FLAG_SESSION, STATUS_MORE = 0x01, 0x80, 0x01
# Блок через DMA: флаг запроса, статусы ответа, размер, предел неудач подряд;
# состояния DMA: нет, свободен (верной обратной посылки нет), блок не прочитан,
# свободен (блок вернулся без искажений).
FLAG_BULK, STATUS_BULK, STATUS_BULK_STALE, STATUS_ECHO_BAD = 0x02, 0x02, 0x04, 0x08
BULK_SIZE, BULK_ATTEMPTS = 512, 3
# Блок от Z80: флаги запроса и статусы ответа.
FLAG_PUT, FLAG_PUT_DONE = 0x04, 0x08
FLAG_MEDIA = 0x10
STATUS_PUT_READY, STATUS_PUT_OK, STATUS_PUT_BAD, STATUS_PUT_CAPABLE = 0x10, 0x20, 0x40, 0x80
BULK_OFF, BULK_FREE, BULK_LOADED, BULK_ECHOED = 0, 1, 2, 3

CMD_WRBUF, CMD_RDBUF = 0x01, 0x02
# Передача блоков через DMA ведомого: ведущий пишет, читает, завершает запись и чтение.
CMD_WRDMA, CMD_RDDMA, CMD_WR_DONE, CMD_RD_DONE = 0x03, 0x04, 0x07, 0x08
PORT_DATA, PORT_CFG = 0x57, 0x77
CFG_HOST_MASTER, CFG_NONE, CFG_ESP = 0x02, 0x03, 0x13


def checksum(data: bytes) -> int:
    """Дополнение суммы байтов до #FF."""
    return ~sum(data) & 0xFF


def request(tid: int, payload: bytes = b"", *, room: int = 0, flags: int = 0) -> bytes:
    """Запрос Z80, дополненный до области обмена."""
    body = bytes([REQUEST, tid, flags, len(payload), room]) + payload
    return (body + bytes([checksum(body)])).ljust(TURN_SIZE, b"\0")


class LinkCore:
    """Эталон SpiLinkCore: очереди и ответ на запрос."""

    def __init__(self, from_capacity: int = 4096, to_capacity: int = 4096) -> None:
        self.from_capacity = from_capacity
        self.to_capacity = to_capacity
        self.from_z80 = bytearray()
        self.to_z80 = bytearray()
        self.last_tid: int | None = None
        self.last_response = b""
        self.turns = self.repeats = self.bad_requests = self.short_accepts = 0
        self.bulk_blocks = self.bulk_bad = self.bulk_gave_ups = 0
        self.bulk_data = b""            # блок, положенный для DMA последним
        self.block_out = False          # блок отдан и ещё не подтверждён
        self.block_tries = 0
        self.block_inline = 0           # сколько его байтов осталось отдать ответами
        self.gave_up = False            # DMA выключен до нового сеанса
        self.put_armed = False
        self.put_xor = 0
        self.put_size = BULK_SIZE
        self.put_media = False
        self.put_command = 0x31
        self.put_prefix = b""
        self.put_blocks = self.put_bad = 0

    def handle_request(self, data: bytes, bulk: int = BULK_OFF, *, put_available: bool = False,
                       put_received: bytes | None = None) -> bytes | None:
        if data[0] != REQUEST:
            return None
        tid, flags, length, room = data[1], data[2], data[3], data[4]
        if length > CHUNK or checksum(data[:HEADER + length]) != data[HEADER + length]:
            self.bad_requests += 1
            return None
        room = min(room, CHUNK)
        if not flags & FLAG_SESSION and self.last_tid == tid:
            self.repeats += 1
            return self.last_response
        if flags & FLAG_SESSION:
            self.gave_up = False
            self.put_armed = False
        if flags & FLAG_DROP:
            self.to_z80.clear()
            self.block_out = False
            self.block_inline = 0
        put_status = STATUS_PUT_CAPABLE if put_available else 0
        if flags & FLAG_PUT_DONE:
            good = False
            if (self.put_armed and put_received is not None and len(put_received) >= (self.put_size+511)//512*512
                    and self.from_capacity - len(self.from_z80) >= self.put_size + (4+len(self.put_prefix) if self.put_media else 0)):
                xor = 0
                if not self.put_media:
                    for value in put_received[:self.put_size]:
                        xor ^= value
                if self.put_media or xor == self.put_xor:
                    if self.put_media:
                        self.from_z80 += bytes([0xA6,self.put_command]) + (self.put_size+len(self.put_prefix)).to_bytes(2,'little') + self.put_prefix
                    self.from_z80 += put_received[:self.put_size]
                    good = True
            self.put_armed = False
            if good:
                put_status |= STATUS_PUT_OK
                self.put_blocks += 1
            else:
                put_status |= STATUS_PUT_BAD
                self.put_bad += 1
        media = bool(flags & FLAG_MEDIA)
        accepted = length if media else min(length, self.from_capacity - len(self.from_z80))
        if media:
            room = 0
            command = data[HEADER] if length else 0
            resource = command == 0x6A and length == 7
            valid = resource or (command == 0x31 and length == 3)
            size = int.from_bytes(data[HEADER+1:HEADER+3],'little') if valid else 0
            prefix = data[HEADER+3:HEADER+7] if resource else b''
            if put_available and 0 < size <= 8192 and self.from_capacity-len(self.from_z80) >= size+len(prefix)+4:
                self.put_armed = True
                self.put_media = True
                self.put_size = size
                self.put_command = command
                self.put_prefix = prefix
                put_status |= STATUS_PUT_READY
        else:
            self.from_z80 += data[HEADER:HEADER + accepted]
        if not media and flags & FLAG_PUT:
            room = 0
            if put_available and self.from_capacity - len(self.from_z80) >= BULK_SIZE:
                self.put_armed = True
                self.put_xor = data[4]
                self.put_media = False
                self.put_size = BULK_SIZE
                put_status |= STATUS_PUT_READY
        wants = bool(flags & FLAG_BULK)
        bulk_status = 0
        if self.block_out and not self.block_inline:
            if bulk == BULK_ECHOED:
                self.block_out = False
            elif wants:
                if bulk == BULK_LOADED:
                    bulk_status = STATUS_BULK
                else:
                    self.bulk_bad += 1
                    self.block_tries += 1
                    if bulk == BULK_FREE and self.block_tries < BULK_ATTEMPTS:
                        bulk_status = STATUS_BULK | STATUS_ECHO_BAD
                    else:
                        self.gave_up = True
                        self.bulk_gave_ups += 1
                        self.block_inline = BULK_SIZE
                        bulk_status = STATUS_ECHO_BAD
        if not bulk_status and wants and not self.block_out and not self.gave_up:
            if bulk == BULK_LOADED:
                bulk_status = STATUS_BULK | STATUS_BULK_STALE
            elif bulk != BULK_OFF and len(self.to_z80) >= BULK_SIZE:
                self.bulk_data = bytes(self.to_z80[:BULK_SIZE])
                del self.to_z80[:BULK_SIZE]
                self.block_out = True
                self.block_tries = 0
                self.bulk_blocks += 1
                bulk_status = STATUS_BULK
        sent = b""
        if room and not bulk_status & STATUS_BULK:
            if self.block_inline:
                start = BULK_SIZE - self.block_inline
                sent = self.bulk_data[start:start + room]
                self.block_inline -= len(sent)
                if not self.block_inline:
                    self.block_out = False
            elif not self.block_out:
                sent = bytes(self.to_z80[:room])
                del self.to_z80[:room]
        more = STATUS_MORE if self.to_z80 or self.block_out else 0
        body = bytes([RESPONSE, tid, more | bulk_status | put_status, len(sent), accepted]) + sent
        response = body + bytes([checksum(body)])
        self.last_tid, self.last_response = tid, response
        self.turns += 1
        if accepted != length:
            self.short_accepts += 1
        return response

    def state_word(self, nonce: int = 0) -> bytes:
        pending = self.to_z80 or self.block_out
        return bytes([STATE_PENDING if pending else STATE_EMPTY, MAGIC, getattr(self,"version",VERSION), nonce])

    def write(self, data: bytes) -> int:
        count = min(len(data), self.to_capacity - len(self.to_z80))
        self.to_z80 += data[:count]
        return count

    def read(self, limit: int | None = None) -> bytes:
        count = len(self.from_z80) if limit is None else min(limit, len(self.from_z80))
        data = bytes(self.from_z80[:count])
        del self.from_z80[:count]
        return data


class SlaveHd:
    """Общий буфер SPI Slave HD: что видит ведущий на линии байт за байтом."""

    def __init__(self) -> None:
        self.buffer = bytearray(BUFFER_SIZE)
        # Что ведущий принимает, пока ведомый данных не выдаёт (команда, адрес,
        # пустой байт, запись): уровень неподвижной линии MISO. На плате она
        # подтянута к питанию.
        self.idle = 0xFF
        self.selected = False
        self.phase = 0
        self.command = 0
        self.address = 0
        self.start = 0                  # адрес, с которого началась транзакция
        self.overruns = 0               # обращения за пределы 64 байтов
        self.transactions: list[tuple[int, int, int]] = []   # (команда, адрес, байтов данных)
        self.on_written = None          # WRBUF закончен: ведущий снял выбор
        self.on_read = None             # принят адрес RDBUF, данные ещё не читались
        # DMA: что ведомый выставил для чтения, что принял, чем ведущий закончил.
        self.dma_tx = b""
        self.dma_tx_read = 0            # сколько байтов ведущий прочитал
        self.dma_rx = bytearray()
        self.dma_done: list[int] = []   # принятые команды завершения (#07, #08)

    def select(self, active: bool) -> None:
        if active and not self.selected:
            self.phase = 0
        if not active and self.selected and self.phase:
            data_bytes = max(self.phase - 3, 0)
            self.transactions.append((self.command, self.start, data_bytes))
            if self.command in (CMD_WR_DONE, CMD_RD_DONE):
                self.dma_done.append(self.command)
            if self.command == CMD_WRBUF and data_bytes and self.on_written:
                self.on_written()
        self.selected = active

    def exchange(self, mosi: int) -> int:
        """Один байт SPI: mosi от ведущего, возврат — байт на MISO."""
        if not self.selected:
            return 0xFF
        phase = self.phase
        self.phase += 1
        if phase == 0:
            self.command = mosi
            self.start = 0
            return self.idle            # в фазах команды, адреса и пустого байта MISO не значим
        if phase == 1:
            self.address = self.start = mosi
            if self.command == CMD_RDBUF and self.on_read:
                self.on_read(mosi)
            return self.idle
        if phase == 2:
            return self.idle
        if self.command == CMD_RDDMA:
            index = phase - 3
            self.dma_tx_read = index + 1
            return self.dma_tx[index] if index < len(self.dma_tx) else 0x00
        if self.command == CMD_WRDMA:
            self.dma_rx.append(mosi)
            return self.idle
        address = self.address
        self.address += 1
        if address >= BUFFER_SIZE:
            self.overruns += 1
            return 0x00
        if self.command == CMD_WRBUF:
            self.buffer[address] = mosi
            return self.idle
        if self.command == CMD_RDBUF:
            return self.buffer[address]
        return self.idle


class EspSide:
    """Прошивка у общего буфера: задача обмена и приложение."""

    def __init__(self, slave: SlaveHd, core: LinkCore | None = None) -> None:
        self.slave = slave
        self.core = core or LinkCore()
        self.alive = True
        self.latency = 0                # сколько чтений буфера Z80 сделает до ответа
        self.ignore_requests = 0        # столько запросов «прошивка» не заметит
        self.corrupt_reads = 0          # столько раз ответ будет показан испорченным
        self.lose_replies = 0           # столько готовых ответов Z80 не увидит
        self.split_publish = False      # тело ответа и его признак — в разные чтения
        self.app_reads = True           # приложение забирает принятое из очереди
        self.bulk = True                # прошивка умеет отдавать блоки через DMA
        self.bulk_loaded = False        # блок выставлен и ещё не прочитан
        self.echo_wanted = False        # приём обратной посылки открыт
        self.echo_good = False          # последний блок вернулся без искажений
        self.echo_results: list[bool] = []
        self.corrupt_blocks = 0         # столько блоков Z80 прочитает с ошибкой
        # Приём через DMA один на две цели: обратную посылку и блок от Z80.
        self.receive_is_put = False
        self.put = True                 # прошивка принимает блоки от Z80
        self.put_data: bytes | None = None   # пришедший блок от Z80
        self.corrupt_puts = 0           # столько блоков от Z80 придут с ошибкой
        self.rx = bytearray()           # приложение -> Z80 (ещё не в очереди)
        self.tx = bytearray()           # Z80 -> приложение
        self.requests_seen = 0
        self._pending = 0               # чтений до ответа; 0 — запроса нет
        self._header: bytes | None = None
        self._restore: tuple[int, int] | None = None
        slave.on_written = self._written
        slave.on_read = self._read_started
        self.publish_state()

    # --- приложение -----------------------------------------------------------
    def pump(self) -> None:
        """Сдвинуть данные между приложением и очередями канала."""
        if not self.alive:
            return
        if self.rx:
            count = self.core.write(bytes(self.rx))
            del self.rx[:count]
        if self.app_reads:
            self.tx += self.core.read()
        self.publish_state()

    def publish_state(self) -> None:
        self.slave.buffer[STATE_ADDR:BUFFER_SIZE] = self.core.state_word()

    def die(self) -> None:
        """ESP сброшена: буфер обнулён, отвечать некому."""
        self.alive = False
        self._pending = 0
        self._header = None
        self.slave.buffer[:] = bytes(BUFFER_SIZE)

    # --- задача обмена --------------------------------------------------------
    def _written(self) -> None:
        if not self.alive:
            return
        self.requests_seen += 1
        if self.ignore_requests:
            self.ignore_requests -= 1
            return
        self._pending = self.latency + 1
        if self.latency == 0:
            self._advance()

    def _read_started(self, address: int) -> None:
        if not self.alive:
            return
        if self._restore is not None:
            index, value = self._restore
            self.slave.buffer[index] = value
            self._restore = None
        self.pump()
        self._advance()
        if address == 0 and self.corrupt_reads and self.slave.buffer[0] == RESPONSE:
            # Ответ опубликован, но этот раз Z80 прочитает его с ошибкой.
            self.corrupt_reads -= 1
            index = HEADER + self.slave.buffer[3]       # контрольная сумма
            self._restore = (index, self.slave.buffer[index])
            self.slave.buffer[index] ^= 0x40

    def _advance(self) -> None:
        if self._header is not None:
            self.slave.buffer[0:4] = self._header
            self._header = None
            self.pump()
            return
        if not self._pending:
            return
        self._pending -= 1
        if self._pending:
            return
        self.pump()
        state = BULK_OFF
        if self.bulk:
            # Чтение блока Z80 завершает командой #08, обратную посылку — #07.
            if self.bulk_loaded and CMD_RD_DONE in self.slave.dma_done:
                self.slave.dma_done.remove(CMD_RD_DONE)
                self.bulk_loaded = False
            if self.echo_wanted and CMD_WR_DONE in self.slave.dma_done:
                self.slave.dma_done.remove(CMD_WR_DONE)
                self.echo_wanted = False
                received = bytes(self.slave.dma_rx)
                if self.receive_is_put:
                    if self.corrupt_puts:
                        self.corrupt_puts -= 1
                        broken = bytearray(received)
                        broken[100] ^= 0x04
                        received = bytes(broken)
                    self.put_data = received
                else:
                    self.echo_good = received == self.core.bulk_data
                    self.echo_results.append(self.echo_good)
            if self.bulk_loaded:
                state = BULK_LOADED
            else:
                state = BULK_ECHOED if self.echo_good else BULK_FREE
        response = self.core.handle_request(bytes(self.slave.buffer[:TURN_SIZE]), state,
                                            put_available=self.bulk and self.put,
                                            put_received=self.put_data)
        if response is None:
            return
        if response[2] & (STATUS_PUT_OK | STATUS_PUT_BAD):
            self.put_data = None
        if response[2] & STATUS_PUT_READY:
            self.receive_is_put = True
            self.put_data = None
            if not self.echo_wanted:
                self.slave.dma_rx.clear()
                self.echo_wanted = True
        if response[2] & STATUS_BULK and not response[2] & STATUS_BULK_STALE:
            self.echo_good = False
            self.receive_is_put = False
            if not self.bulk_loaded:
                block = bytearray(self.core.bulk_data)
                if self.corrupt_blocks:
                    self.corrupt_blocks -= 1
                    block[77] ^= 0x10
                self.slave.dma_tx = bytes(block) + bytes(4)
                self.bulk_loaded = True
            if not self.echo_wanted:
                self.slave.dma_rx.clear()
                self.echo_wanted = True
        # Как в прошивке: сначала байты с четвёртого, признак ответа — последним.
        self.slave.buffer[4:len(response)] = response[4:]
        if self.lose_replies:
            # Запрос обработан, но ответа Z80 не увидит: ей придётся повторить.
            self.lose_replies -= 1
            self.slave.buffer[0:4] = bytes(4)
        elif self.split_publish:
            self._header = response[:4]
        else:
            self.slave.buffer[0:4] = response[:4]
        self.pump()


class SpiPorts:
    """Порты #57/#77 TS-Config: ведущий SPI ZX Evolution."""

    # Что отдаёт чтение #57. Обмен, который оно начинает, на 14 МГц к концу
    # цикла чтения не успевает: Z80 получает байт прошлого обмена (False). На
    # 28 МГц при Z80 на 3,5 МГц он успевает, и Z80 получает его байт (True).
    # Значение для вновь создаваемых портов; у готовых меняется поле current.
    default_current = False

    def __init__(self, slave: SlaveHd) -> None:
        self.slave = slave
        self.current = SpiPorts.default_current
        self.received = 0xFF            # байт, принятый в последнем обмене
        self.cfg_writes: list[int] = []
        # DMA TS-Config: память Z80 (её задаёт стенд), страница окна #C000 и регистры.
        self.memory = None
        self.page3 = 0
        # Какая страница ОЗУ в каком окне: номер -> адрес окна. Окно #C000
        # задаёт запись в #13AF, окно #8000 у плагина WC называет стенд.
        self.windows: dict[int, int] = {0: 0xC000}
        self.dma: dict[int, int] = {}
        self.dma_runs: list[tuple[int, int]] = []       # (режим, байтов)
        # Что DMA записал в память мимо кэша процессора: (страница, начало,
        # конец в ней). На железе Z80 может прочитать там прежние байты из кэша,
        # пока программа не вытеснит его (SpiLink_Sweep — тогда стенд очищает
        # список). Страница, а не адрес: окно у неё к тому времени может быть другим.
        self.dma_dirty: list[tuple[int, int, int]] = []
        # Неисправная линия MISO: ведущий читает только это значение, хотя
        # ведомый по-прежнему слышит всё, что ему передают.
        self.miso_stuck: int | None = None

    def _miso(self, value: int) -> int:
        return value if self.miso_stuck is None else self.miso_stuck

    def write(self, port: int, value: int) -> bool:
        low = port & 0xFF
        if low == PORT_CFG:
            if not value & 0x01 and value & 0x80:
                raise AssertionError("#77 с D0=0 и D7=1 отдаёт шину SPI ESP: на VDAC4 нельзя")
            if not value & 0x02:
                raise AssertionError("обмен с ESP не должен выбирать SD-карту")
            self.cfg_writes.append(value)
            self.slave.select(bool(value & 0x10))
            return True
        if low == PORT_DATA:
            self.received = self._miso(self.slave.exchange(value))
            return True
        if low == 0xAF:
            return self._ts_write(port >> 8, value)
        return False

    # Регистры TS-Config: #13AF — страница окна #C000; #1A..#1CAF и #1D..#1FAF —
    # адреса источника и приёмника DMA; #26AF — слов в блоке минус один;
    # #28AF — блоков минус один; #27AF — запуск.
    def _ts_write(self, register: int, value: int) -> bool:
        if register == 0x13:
            self.windows = {page: base for page, base in self.windows.items()
                            if base != 0xC000}
            self.windows[value] = 0xC000
            self.page3 = value
        elif register in (0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x26, 0x28):
            self.dma[register] = value
        elif register == 0x27:
            self._dma_launch(value)
        else:
            return False
        return True

    def _dma_address(self, base: int, count: int) -> int:
        """Адрес в памяти Z80 по трём регистрам DMA с номера base."""
        window = self.windows.get(self.dma.get(base + 2))
        if window is None:
            raise AssertionError(f"DMA: страница #{self.dma.get(base + 2)} не стоит ни в одном окне")
        if self.dma[base] & 1:
            raise AssertionError("DMA: нечётный адрес — младший разряд теряется")
        offset = (self.dma[base + 1] & 0x3F) << 8 | self.dma[base]
        if offset + count > 0x4000:
            raise AssertionError("DMA выходит за окно")
        return window + offset

    def _written(self, address: int, count: int) -> None:
        self.dma_dirty.append((self.dma[0x1F], address & 0x3FFF, (address & 0x3FFF) + count))

    def clean(self, start: int, end: int) -> bool:
        """Можно ли Z80 читать [start, end): DMA туда после SpiLink_Sweep не писал."""
        window = start & 0xC000
        pages = [page for page, base in self.windows.items() if base == window]
        if not pages:
            return True
        low, high = start - window, end - window
        return all(page != pages[0] or high <= first or last <= low
                   for page, first, last in self.dma_dirty)

    def _dma_launch(self, mode: int) -> None:
        if mode & 0x78:
            raise AssertionError(f"DMA: неожиданные разряды режима #{mode:02X}")
        count = 2 * (self.dma[0x26] + 1) * (self.dma[0x28] + 1)
        if mode == 0x01:                # ОЗУ -> ОЗУ: словами вперёд
            source = self._dma_address(0x1A, count)
            target = self._dma_address(0x1D, count)
            if source < target < source + count:
                raise AssertionError("DMA: приёмник внутри источника")
            self.memory[target:target + count] = bytes(self.memory[source:source + count])
            self._written(target, count)
            self.dma_runs.append((mode, count))
            return
        if mode & 0x07 != 0x02:
            raise AssertionError(f"DMA: устройство {mode & 7}, а не SPI")
        to_device = bool(mode & 0x80)
        address = self._dma_address(0x1A if to_device else 0x1D, count)
        if not to_device:
            self._written(address, count)
        for offset in range(count):
            if to_device:
                self.received = self._miso(self.slave.exchange(self.memory[address + offset]))
            else:
                # Как при чтении #57 в опубликованном HDL: в память уходит байт
                # прошлого обмена, и тут же начинается следующий.
                self.memory[address + offset] = self.received
                self.received = self._miso(self.slave.exchange(0xFF))
        self.dma_runs.append((mode, count))

    def read(self, port: int) -> int | None:
        low = port & 0xFF
        if low == PORT_DATA:
            value = self.received
            self.received = self._miso(self.slave.exchange(0xFF))
            return self.received if self.current else value
        if low == PORT_CFG:
            return 0x00
        if port == 0x27AF:
            return 0x00                 # DMA в модели заканчивается мгновенно
        return None
