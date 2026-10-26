"""Машинные тесты медиапроигрывателя платы VDAC4 (MP3 Player/build/VDAC4MP3.WMF).

Плагин исполняется в эмуляторе Z80 целиком, от входа до возврата в Wild
Commander. Карту отдают заглушки файловых функций WC с потоками API 57: у
потока панели и у каждого из двух потоков плагина свой каталог и своя позиция
в открытом файле (LOAD512 читает секторы, LOADNONE пропускает их, FENTRY читает
каталог тем же потоком и позицию сбивает). Канал до ESP — модель SPI из
spi_link_model.py, а за ней «прошивка», которая отвечает на команды звука и
модулей и запоминает всё, что ей прислали. Кадровое прерывание WC приходит
каждые FRAME_TICKS тактов, и без HALT тоже.
Проверяется:
- MP3 любой длины доходит до ESP целиком и по порядку, блоками не длиннее
  кадра, каждый сектор карты читается или пропускается ровно раз; блок уходит,
  только когда в очереди ESP для него есть место; длинная метка ID3 не
  читается (LOADNONE), ESP получает звук с её конца;
- модуль (MOD, XM, S3M, IT) и MIDI (MID, RMI, KAR) уходят целиком до звука:
  AUDIO_OPEN с длиной (у MIDI — флаг #40), файл, ожидание разбора; играют до
  конца песни;
- свой файл: поиск по имени из BC (и длинному), TENTRY, затем заново по имени
  8.3 и с начала (GFILE); не нашёлся — поток на начало файла плагина
  (GIPAGPL), без перемотки и обложки;
- перемотка: модуль и MIDI — AUDIO_SEEK; MP3 — AUDIO_FLUSH, свой файл заново,
  LOADNONE до сектора нового места, его хвост — первым блоком;
- после конца файла плагин ждёт, пока ESP доиграет, и сам выходит — с кодом
  перехода на следующий файл панели, если он есть; Esc обрывает звук сразу;
- PgDn и PgUp уводят на соседний файл, пробел ставит паузу и снимает её;
- отказы: ESP нет, прошивка без звука, I2S не запустился, файл не взят;
- окно (макет 2026-10-04): название, исполнитель, альбом и год (AUDIO_TAGS,
  у старой прошивки — название из AUDIO_INFO, иначе имя файла), формат, номер
  файла, время с полосой (пока файл целиком принимается — полоса загрузки),
  состояние, столбики спектра (MP3, MIDI) или уровней каналов (AUDIO_VIEW),
  громкость (когда ESP её назвала) и «H Help»; у модуля — сэмплы и эффекты, у
  MIDI — инструменты и дорожки; H — справка, I — подробности окнами поверх,
  Esc сначала закрывает их;
- обложка: пока очередь полна, метка досылается потоком 1 кусками до конца
  сектора с того места, что просит ESP (AUDIO_COVER; вперёд — LOADNONE),
  звук в потоке 0 идёт подряд; готовая миниатюра (AUDIO_THUMB) ложится в
  графику спрайта, описатели — через окно FMADDR, слой спрайтов включается и
  при выходе выключается; картинки нет в метке (или метки нет) —
  cover.jpg/folder.jpg/front.jpg из папки трека кадрами AUDIO_PICTURE;
- модули ESP: PLAYER.ELF и декодер (MP3.ELF, TRACKER.ELF, MIDI.ELF) из
  /WC/ESP/ уходят в кэш ESP, только если их там нет или они изменились
  (MOD_QUERY с длиной и временем записи, MOD_PUT по сектору с полосой
  загрузки, MOD_COMMIT), банк MIDI.SF2 — ресурсом (флаг 1), потом APP_START;
  Esc во время передачи — выход. Нет каталога, модуля, места или прошивка без
  модулей — MP3 и модули играет встроенный плеер, а у MIDI — ошибка с именем
  того, что не вышло.
Нужна сборка: "MP3 Player\\build.bat".
"""
import random
import unittest

import sound_paths
import test_ftp_plugin_z80 as base
from test_spi_link_z80 import SpiHarness, frame, need

ROOT = sound_paths.ROOT
WMF = ROOT / "MP3 Player" / "build" / "VDAC4MP3.WMF"
SYM = ROOT / "MP3 Player" / "build" / "VDAC4MP3.sym"

CALL_EXTENSION, CALL_MENU, CALL_VIEWER = 0, 3, 4
FN_SPACE, FN_UP, FN_DOWN, FN_LEFT, FN_RIGHT, FN_LOAD512 = 0x10, 0x11, 0x12, 0x13, 0x14, 0x30
FN_PGUP, FN_PGDN, FN_WAITUP, FN_ENTER, FN_LOADNONE = 0x19, 0x1A, 0x2E, 0x16, 0x3D
EXIT_NEXT, EXIT_PREV = 2, 4
CMD_PING, RESP_READY, RESP_ERROR = 0x04, 0xF0, 0xEE
TANG_READ = 0x45                            # регистры Tang (прошивка 0.1.72+)
SCREEN_F_SHOWN, SCREEN_F_TANG = 1, 2        # флаги экрана Tang (PLAYER 1.26)
AUDIO_VIEW = 0x2F
AUDIO_OPEN, AUDIO_DATA, AUDIO_CLOSE, AUDIO_STATUS, AUDIO_TONE, AUDIO_VOLUME = range(0x30, 0x36)
AUDIO_PAUSE, AUDIO_PROBE, AUDIO_SEEK, AUDIO_FLUSH, AUDIO_INFO = range(0x36, 0x3B)
AUDIO_COVER, AUDIO_THUMB, AUDIO_PICTURE, LINK_PROBE, AUDIO_TAGS = range(0x3B, 0x40)
MOD_QUERY, MOD_PUT, MOD_COMMIT, APP_START = 0x69, 0x6A, 0x6B, 0x6C
FN_PRWOW, FN_MNGCVPL, FN_GVSGP = 0x01, 65, 71
FN_GIPAGPL, FN_TENTRY, FN_STREAM, FN_FENTRY, FN_GFILE = 0x32, 0x33, 0x39, 0x3B, 0x3E
FN_GDIR, STREAM_WC, STREAM_CLONE, STREAM_ROOT = 0x3F, 0xFD, 0xFE, 0xFF
FN_ADIR, FN_FINDNEXT = 0x38, 0x3A
TS_CONFIG, TS_FMADDR, TS_PAGE3 = 0x06AF, 0x15AF, 0x13AF
# Окно #C000 (main.asm, Page1_Call): физические страницы — как их считает WC.
FN_MNGC_PL, WC_PAGE3 = 0x00, 0x6003
PLUGIN_BASE = 0x61                          # страница 0 плагина (#61 + база пула)
VIDEO_BASE = 0x20                           # видеостраницы API 65
TEXT_PAGE = 0x00                            # текстовый экран: TXTPR ставит его в #C000
POISON = 0xC7                               # RST 0: код страницы 1 без неё в #C000 — в #0000
SFILE = 0xB200                              # окно FMADDR: описатели спрайтов
STATE_IDLE, STATE_PLAYING, STATE_DRAINING, STATE_TONE, STATE_OFFLINE, STATE_LOADING = range(6)
FORMAT_MP3, FORMAT_MOD, FORMAT_XM, FORMAT_S3M, FORMAT_IT, FORMAT_MIDI = 1, 2, 3, 4, 5, 6
FORMAT_OPL = 7
OPEN_MODULE, OPEN_MIDI, OPEN_RELEASE = 0x80, 0x40, 0x20


def wmf_extensions(data: bytes) -> list[tuple[str, int]]:
    """Список расширений WMF #11: записи [длина][код обработчика][имя] с отступа 512."""
    assert data[32] == 0x11 and data[54] == 1, "заголовок формата #11, схема записи 1"
    size = int.from_bytes(data[48:50], "little")
    count = int.from_bytes(data[50:52], "little")
    entries, pos = [], 512
    for _ in range(count):
        length, handler = data[pos], data[pos + 1]
        entries.append((data[pos + 2:pos + 2 + length].decode("ascii"), handler))
        pos += 2 + length
    assert pos == 512 + size, "длина списка в заголовке совпадает с записями"
    return entries


def wmf_code_offset(data: bytes) -> int:
    """Где в файле начинается код: у #11 — после секторов списка расширений."""
    if data[32] != 0x11:
        return 512
    return 512 + (int.from_bytes(data[48:50], "little") + 511) // 512 * 512
NAME_ADDRESS = 0x7000
# Окно проигрывателя (ui.asm): строки внутри рамки.
TITLE_ROW, SECOND_ROW, THIRD_ROW, FORMAT_ROW, TRACK_ROW = 2, 3, 4, 6, 7
TIME_ROW, STATUS_ROW, BARS_ROW, NUMBERS_ROW, VOLUME_ROW = 9, 10, 11, 17, 19
# Окно подробностей (клавиша I).
INFO_FILE, INFO_STATE, INFO_FORMAT, INFO_SENT, INFO_QUEUE, INFO_FAULTS, INFO_VOLUME = range(2, 9)
PLAYED, LEFT, FULL, LOWER, UPPER, SHADE = "═", "─", "█", "▄", "▀", "░"
TEXT_MP3, TEXT_MODULE = 20, 2               # столбец текста: у MP3 слева обложка
FN_KBSCN = 0x2A
HINT_PLAY, HINT_LOST, HINT_MODULE = "Seek: on", "No seek: file not found", "Seek: positions"
BYTES_PER_SECOND = 16000                    # 128 кбит/с
FRAME_TICKS = 280_000                       # кадр 20 мс при 14 МГц
MAX_FRAMES = 60_000                         # 20 минут: дольше — плагин завис
SFN_CHARS = set(b"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")

_symbols = None


def ui(name: str) -> int:
    """Константа плагина из .sym (ширины полос, столбец «H Help»)."""
    global _symbols
    if _symbols is None:
        _symbols = base.Symbols(SYM)
    return _symbols(name)


def short_name(name: bytes) -> bytes:
    """Имя 8.3, как его записала бы FAT для длинного имени: «SONG.MP3» или
    «01FORT~1.MP3»."""
    stem, dot, ext = name.upper().rpartition(b".")
    if not dot:
        stem, ext = name.upper(), b""
    clean_stem = bytes(c for c in stem if c in SFN_CHARS)
    clean_ext = bytes(c for c in ext if c in SFN_CHARS)[:3]
    if clean_stem != stem or len(stem) > 8 or clean_ext != ext:
        clean_stem = clean_stem[:6] + b"~1"
    return clean_stem + (b"." + clean_ext if clean_ext else b"")


def entry_of(short: bytes, size: int, time: int = 0, date: int = 0) -> bytes:
    """32 байта каталожной записи DOS: имя 8.3, время и дата FAT, длина."""
    stem, _, ext = short.partition(b".")
    return (stem.ljust(8) + ext.ljust(3) + bytes(11) + time.to_bytes(2, "little") +
            date.to_bytes(2, "little") + bytes(2) + size.to_bytes(4, "little"))


UI_F_OPEN, UI_F_LEAVING = 1, 2


def parse_ui_block(payload: bytes) -> dict | None:
    """Тело AUDIO_STATUS: [версия 1][флаги][окно поверх][номер LE16][из LE16][отдано LE32]
    [имя, 0][строка состояния, 0]; пусто или другая версия — None."""
    if len(payload) < 11 or payload[0] != 1:
        return None
    texts = payload[11:].split(b"\0")
    return {'flags': payload[1], 'page': payload[2],
            'track': int.from_bytes(payload[3:5], 'little'),
            'tracks': int.from_bytes(payload[5:7], 'little'),
            'sent': int.from_bytes(payload[7:11], 'little'),
            'name': texts[0].decode('cp866'), 'status': texts[1].decode('cp866') if len(texts) > 1 else '',
            'size': len(payload)}


class AudioEsp:
    """Звуковая часть прошивки: очередь MP3, буфер файла целиком и ответы AUDIO_*."""

    def __init__(self, capacity: int = 128 * 1024, played_per_status: int = 4096) -> None:
        self.capacity = capacity
        self.played_per_status = played_per_status
        self.queue = 0                      # байтов в очереди
        self.received = bytearray()         # всё, что прислал плагин
        self.blocks: list[int] = []
        self.rejected = 0                   # блоки, которым не хватило места
        self.state = STATE_IDLE
        self.finished = False
        self.volume = 200
        self.temperature = 0                # кристалл ESP, °C (0 — старый PLAYER: нет)
        self.rate = 44100                   # больше 65535 — младшие 16 бит и флаг 128 (PLAYER 1.19)
        self.volumes: list[int] = []
        self.commands: list[int] = []
        self.closes: list[int] = []
        self.opens: list[bytes] = []
        self.supported = True
        self.online = True
        self.pause_known = True             # прошивка знает команду паузы
        self.paused = False
        self.pauses: list[int] = []
        self.buffering = False
        self.status_queue = 0               # очередь в последнем ответе о состоянии
        # Тела AUDIO_STATUS (плагин 1.19: экран оверлеем Tang): разобранные поля по порядку.
        self.ui_blocks: list[dict] = []
        self.ui_raw: list[bytes] = []       # они же как пришли (test_player_host: вся цепочка)
        self.decode_errors = 0
        self.underruns = 0
        self.i2s_faults = 0
        # Перемотка MP3: с какого времени и с какого байта после AUDIO_FLUSH.
        self.flushes: list[int] = []
        self.flush_offsets: list[int | None] = []   # место в файле (плагин 0.9)
        self.base_seconds = 0
        self.since_flush = 0
        self.flush_while_draining = True    # PLAYER 1.7: перемотка снова открывает закрытый поток
        # Файл целиком: модуль или MIDI.
        self.module_size = 0
        self.module_format = FORMAT_XM
        self.module_ok = True               # разберёт ли ESP файл
        self.module_take = True             # возьмёт ли ESP файл (память)
        self.module_refuse_at = None        # столько байтов файла — и ESP его отвергает (ответ 2)
        self.skip_polls = 0                 # пропуск: столько запросов состояния «играет»
        self.loading_polls = 2              # запросов состояния до конца разбора
        self.module_opens = 0
        self.needs_rom = False              # файл для OPL4: декодер просит ПЗУ (флаг 16)
        self.releases = 0                   # AUDIO_OPEN [#20]: ресурсы освобождены
        self.length = 6
        self.position = 0
        self.row = 0
        self.seeks: list[int] = []
        self.title = b"Test Module Title"
        # Обложка: где в файле тело APIC (None — картинки нет).
        self.cover_known = True             # прошивка знает AUDIO_COVER
        self.cover_apic: tuple[int, int] | None = None
        self.cover_want = 0
        self.cover_state = 0                # 0 — шли, 1 — собрана, 2 — нет
        self.cover_data = bytearray()
        self.cover_chunks: list[tuple[int, int]] = []
        # Обложку нашёл декодер (Ogg): на этом опросе AUDIO_STATUS — флаг 64,
        # миниатюра сводится; AUDIO_THUMB флаг снимает.
        self.decoder_cover_at: int | None = None
        self.decoder_cover = False
        self.thumb_delay = 2                # «сводится» столько вопросов
        self.thumb_fail = False
        self.thumb_polls = 0
        self.thumb = bytes((index * 7 + (index >> 5)) & 0xFF for index in range(2048))
        # Картинка файлом из папки трека (AUDIO_PICTURE).
        self.picture_known = True
        self.picture = bytearray()
        self.picture_total = 0
        self.picture_chunks: list[tuple[int, int]] = []
        # Модули ESP: кэш во flash (имя -> длина и время записи), ресурсы в
        # PSRAM (имя -> длина) и приложение.
        self.modules_known = True           # прошивка с загрузчиком модулей
        self.cache: dict[bytes, tuple[int, int]] = {}
        self.cache_room = 1 << 20
        self.resources: dict[bytes, int] = {}
        self.resource_room = 7 << 20
        self.queries: list[tuple[bytes, int, int, int]] = []   # имя, длина, время, флаги
        self.upload: tuple[bytes, int, int, int] | None = None
        self.uploaded = bytearray()
        self.puts: list[tuple[int, int]] = []
        self.stored: dict[bytes, bytes] = {}    # что дошло целиком
        self.started: list[bytes] = []
        self.start_result = 0
        # Tang: VERSION (None — Tang нет / прошивка без TANG_READ) и флаги экрана PLAYER 1.26
        # в ответах AUDIO_OPEN и AUDIO_STATUS (None — прежний PLAYER: байта нет).
        self.tang_version: int | None = None
        self.screen: int | None = None
        # Столбики (AUDIO_VIEW) и метки (AUDIO_TAGS) — только у PLAYER.ELF:
        # встроенный плеер прошивки их не знает.
        self.view_known = False
        self.view_kind = 1
        self.view_values: list[int] | object = [0] * 32   # или функция номера опроса
        self.view_duration = 0
        self.view_polls = 0
        self.view_text: bytes | None = None    # вид 3 (радио): список станций текстом TXTPR
        self.tags_known = False
        self.tags = b"\0\0\0\0\0"
        # Радио: на этом запросе состояния (счёт с 1) — флаг 32 «метки сменились»;
        # AUDIO_TAGS его снимает и отдаёт tags_next (новая песня).
        self.tags_flag_at: int | None = None
        self.tags_flag = False
        self.tags_next: bytes | None = None
        self.tags_reads = 0
        self.status_polls = 0

    def module_command(self, command: int, payload: bytes) -> bytes:
        """MOD_QUERY, MOD_PUT, MOD_COMMIT, APP_START — как в module_app.cpp."""
        if command == MOD_QUERY:
            flags = payload[0]
            size = int.from_bytes(payload[1:5], "little")
            stamp = int.from_bytes(payload[5:9], "little")
            name = payload[9:].split(bytes(1), 1)[0]
            self.queries.append((name, size, stamp, flags))
            resource = bool(flags & 1)
            if resource and self.resources.get(name) == size:
                state = 0
            elif not resource and self.cache.get(name) == (size, stamp):
                state = 0
            elif size > ((self.resource_room - sum(length for other, length in self.resources.items()
                                                   if other != name))
                         if resource else self.cache_room):
                state = 2
            else:
                state = 1
                self.upload, self.uploaded = (name, size, stamp, flags), bytearray()
            return frame(0xE9, bytes([state]) + self.cache_room.to_bytes(4, "little"))
        if command == MOD_PUT:
            offset = int.from_bytes(payload[:4], "little")
            data = payload[4:]
            self.puts.append((offset, len(data)))
            ok = (self.upload is not None and offset == len(self.uploaded) and
                  offset + len(data) <= self.upload[1])
            if ok:
                self.uploaded += data
            return frame(0xEA, bytes([1 if ok else 0]) + len(self.uploaded).to_bytes(4, "little"))
        if command == MOD_COMMIT:
            ok = False
            if self.upload is not None and len(self.uploaded) == self.upload[1]:
                name, size, stamp, flags = self.upload
                if flags & 1:
                    ok = True
                    self.resources[name] = size
                elif self.uploaded[:4] == b"\x7fELF":
                    ok = True
                    self.cache[name] = (size, stamp)
                if ok:
                    self.stored[name] = bytes(self.uploaded)
            self.upload = None
            return frame(0xEB, bytes([1 if ok else 0]))
        name = payload.split(bytes(1), 1)[0]
        self.started.append(name)
        result = self.start_result if name in self.cache else -3     # ZM_NOT_FOUND
        return frame(0xEC, bytes([result & 0xFF]) + name + b"\x000.7\x00")

    def cover(self, payload: bytes) -> bytes:
        offset = int.from_bytes(payload[:4], "little")
        data = payload[4:]
        self.cover_chunks.append((offset, len(data)))
        want = self.cover_want
        if self.cover_state == 0 and offset <= want < offset + len(data):
            if self.cover_apic is None:
                self.cover_state = 2
            elif want < self.cover_apic[0]:
                self.cover_want = self.cover_apic[0]          # к телу APIC
            else:
                part = data[want - offset:want - offset + self.cover_apic[1] - want]
                self.cover_data += part
                self.cover_want += len(part)
                if self.cover_want >= self.cover_apic[1]:
                    self.cover_state = 1
        elif self.cover_state == 0 and offset == want and not data:
            self.cover_state = 2                              # файл кончился
        return frame(0xBB, bytes([self.cover_state]) + self.cover_want.to_bytes(4, "little") +
                     self.free())

    def thumb_part(self, part: int) -> bytes:
        self.thumb_polls += 1
        if self.cover_state != 1 or self.thumb_fail:
            return frame(0xBC, bytes([2, part]))
        if self.thumb_polls <= self.thumb_delay:
            return frame(0xBC, bytes([0, part]))
        return frame(0xBC, bytes([1, part]) + self.thumb[part * 512:(part + 1) * 512])

    def view(self) -> bytes:
        values = (self.view_values(self.view_polls) if callable(self.view_values)
                  else self.view_values)
        self.view_polls += 1
        seconds = self.base_seconds + self.since_flush // BYTES_PER_SECOND
        if self.module_size:
            seconds = self.position * 3
        if self.view_text is not None:
            # Радио: [3][0][длительность][позиция][строки TXTPR, 0].
            return frame(0xAF, bytes([self.view_kind, 0]) + self.view_duration.to_bytes(2, "little") +
                         seconds.to_bytes(2, "little") + self.view_text + bytes(1))
        return frame(0xAF, bytes([self.view_kind, len(values)]) +
                     self.view_duration.to_bytes(2, "little") +
                     seconds.to_bytes(2, "little") + bytes(values))

    def free(self) -> bytes:
        if self.module_size:
            return (self.module_size - len(self.received)).to_bytes(4, "little")
        return (self.capacity - self.queue).to_bytes(4, "little")

    def status(self) -> bytes:
        module = bool(self.module_size)
        seconds = self.base_seconds + self.since_flush // BYTES_PER_SECOND
        if module:
            seconds = self.position * 3
        decoded = (bool(self.received) and (not module or self.state != STATE_LOADING) and
                   self.module_refuse_at is None)
        flags = ((1 if self.finished else 0) | (2 if decoded else 0) |
                 (4 if self.buffering else 0) | (8 if self.paused else 0) |
                 (32 if self.tags_flag else 0) | (64 if self.decoder_cover else 0) |
                 (128 if decoded and self.rate > 0xFFFF else 0))
        if (module and self.needs_rom and self.state == STATE_PLAYING and
                b"YRW801.ROM" not in self.resources):
            flags |= 16
        record = (bytes([self.state, flags]) +
                  self.free() + (0 if module else self.queue).to_bytes(4, "little") +
                  ((self.rate & 0xFFFF) if decoded else 0).to_bytes(2, "little") +
                  bytes([2 if decoded else 0]) +
                  (0 if module or not decoded else 128).to_bytes(2, "little") +
                  seconds.to_bytes(2, "little") + self.i2s_faults.to_bytes(2, "little") +
                  self.decode_errors.to_bytes(2, "little") +
                  self.underruns.to_bytes(2, "little") +
                  bytes([self.volume]))
        fmt = (self.module_format if module and decoded else (FORMAT_MP3 if decoded else 0))
        record += (bytes([fmt, self.row]) + self.position.to_bytes(2, "little") +
                   (self.length if module else 0).to_bytes(2, "little") +
                   bytes([8 if module else 0, self.temperature]))
        assert len(record) == 32
        return record + (bytes([self.screen]) if self.screen is not None else b"")

    def opened(self, body: bytes) -> bytes:
        return frame(0xB0, body + (bytes([self.screen]) if self.screen is not None else b""))

    def __call__(self, command: int, payload: bytes) -> bytes:
        self.commands.append(command)
        if command == CMD_PING:
            return frame(RESP_READY)
        if command == TANG_READ and self.tang_version is not None:
            assert payload == bytes([0x04, 0x00, 1]), 'плагин читает только VERSION'
            return frame(0xC5, bytes([1]) + self.tang_version.to_bytes(4, "little"))
        if not self.supported:
            return frame(RESP_ERROR, b"unsupported:%02X" % command)
        if command in (MOD_QUERY, MOD_PUT, MOD_COMMIT, APP_START) and self.modules_known:
            return self.module_command(command, payload)
        if command == AUDIO_OPEN:
            self.opens.append(payload)
            if not self.online:
                return self.opened(bytes([0]) + bytes(4))
            if payload == bytes([OPEN_RELEASE]):
                self.releases += 1
                for name in (b"MIDI.SF2", b"YRW801.ROM"):
                    self.resources.pop(name, None)
                self.state = STATE_IDLE
                return self.opened(bytes([1]) + self.free())
            if payload and payload[0] & OPEN_MODULE:
                if not self.module_take:
                    return self.opened(bytes([0]) + bytes(4))
                self.module_opens += 1
                if self.module_opens > 1:
                    self.received, self.loading_polls = bytearray(), 2    # файл заново
                    self.position = self.row = 0
                self.module_size = int.from_bytes(payload[1:5], "little")
                self.state = STATE_LOADING
                return self.opened(bytes([1]) + self.free())
            self.queue, self.state, self.finished = 0, STATE_PLAYING, False
            return self.opened(bytes([1]) + self.free())
        if command == AUDIO_DATA:
            if self.module_size:
                if (self.module_refuse_at is not None and self.state == STATE_LOADING and
                        len(self.received) >= self.module_refuse_at):
                    # PLAYER 1.2: декодер отверг IT посреди приёма — пропуск сразу.
                    self.state, self.skip_polls = STATE_PLAYING, 3
                if self.state != STATE_LOADING or \
                        len(payload) > self.module_size - len(self.received):
                    return frame(0xB1, bytes([2]) + self.free())
                self.received += payload
                self.blocks.append(len(payload))
                return frame(0xB1, bytes([1]) + self.free())
            if self.state != STATE_PLAYING:
                return frame(0xB1, bytes([2]) + self.free())
            if len(payload) > self.capacity - self.queue:
                self.rejected += 1
                return frame(0xB1, bytes([0]) + self.free())
            self.queue += len(payload)
            self.received += payload
            self.since_flush += len(payload)
            self.blocks.append(len(payload))
            return frame(0xB1, bytes([1]) + self.free())
        if command == AUDIO_CLOSE:
            self.closes.append(payload[0])
            if payload[0] and self.state == STATE_PLAYING:
                self.state = STATE_DRAINING
            else:
                self.queue, self.state = 0, STATE_IDLE
            return frame(0xB2, bytes([1]))
        if command == AUDIO_PAUSE and self.pause_known:
            self.paused = bool(payload[0])
            self.pauses.append(payload[0])
            return frame(0xB6, bytes([1]))
        if command == AUDIO_SEEK:
            delta = payload[0] - 256 if payload[0] >= 128 else payload[0]
            self.seeks.append(delta)
            self.position = max(0, min(self.length - 1, self.position + delta))
            self.row = 0
            return frame(0xB8, bytes([1]) + self.position.to_bytes(2, "little"))
        if command == AUDIO_FLUSH:
            ms = int.from_bytes(payload[:4], "little")
            self.flushes.append(ms)
            self.flush_offsets.append(int.from_bytes(payload[4:8], "little") if len(payload) >= 8 else None)
            if self.state == STATE_DRAINING:
                if not self.flush_while_draining:
                    return frame(0xB9, bytes([0]) + self.free())   # старый PLAYER: поток закрыт
                self.state = STATE_PLAYING
            self.queue = 0
            self.base_seconds, self.since_flush = ms // 1000, 0
            self.received += b"<FLUSH>"
            return frame(0xB9, bytes([1]) + self.free())
        if command == AUDIO_COVER and self.cover_known:
            return self.cover(payload)
        if command == AUDIO_THUMB and self.cover_known:
            self.decoder_cover = False
            return self.thumb_part(payload[0])
        if command == AUDIO_PICTURE and self.picture_known:
            total = int.from_bytes(payload[:4], "little")
            offset = int.from_bytes(payload[4:8], "little")
            data = payload[8:]
            self.picture_chunks.append((offset, len(data)))
            if offset == 0:
                self.picture, self.picture_total = bytearray(), total
            if offset == len(self.picture) and total == self.picture_total:
                self.picture += data
            state = 0
            if len(self.picture) == self.picture_total:
                state = 1
                self.cover_state, self.thumb_polls = 1, 0      # миниатюра сводится
            return frame(0xBD, bytes([state]) + len(self.picture).to_bytes(4, "little") +
                         self.free())
        if command == AUDIO_VIEW and self.view_known:
            return self.view()
        if command == AUDIO_TAGS and self.tags_known:
            self.tags_reads += 1
            if self.tags_flag:
                self.tags_flag = False
                if self.tags_next is not None:
                    self.tags = self.tags_next
            return frame(0xBF, self.tags)
        if command == AUDIO_INFO:
            return frame(0xBA, bytes([self.module_format, 8]) +
                         self.length.to_bytes(2, "little") + self.title + bytes(1))
        if command == AUDIO_STATUS:
            block = parse_ui_block(payload)
            self.ui_blocks.append(block)
            self.ui_raw.append(bytes(payload))
            if block and block['flags'] & UI_F_LEAVING:
                # Прощальный запрос: ответ — как есть, без хода «проигрывания»
                return frame(0xB3, self.status())
            self.status_polls += 1
            if self.tags_flag_at == self.status_polls:
                self.tags_flag = True
            if self.decoder_cover_at == self.status_polls:
                self.decoder_cover = True
                self.cover_state, self.thumb_polls = 1, 0
            if self.module_size and self.skip_polls:
                self.skip_polls -= 1
                if self.skip_polls == 0:
                    self.state, self.finished = STATE_IDLE, True
            elif self.module_size:
                if self.state == STATE_LOADING and len(self.received) == self.module_size:
                    self.loading_polls -= 1
                    if self.loading_polls <= 0:
                        self.state = STATE_PLAYING if self.module_ok else STATE_IDLE
                elif self.state == STATE_PLAYING and not self.paused:
                    # «Проигрывание»: позиция растёт с каждым запросом.
                    self.row += 16
                    if self.row >= 64:
                        self.row = 0
                        self.position += 1
                    if self.position >= self.length:
                        self.state, self.finished = STATE_IDLE, True
            else:
                # «Проигрывание»: между запросами состояния очередь убывает.
                if not self.paused:
                    self.queue = max(0, self.queue - self.played_per_status)
                if self.state == STATE_DRAINING and self.queue == 0:
                    self.state, self.finished = STATE_IDLE, True
            if not self.online:
                self.state = STATE_OFFLINE
            self.status_queue = self.queue
            return frame(0xB3, self.status())
        if command == AUDIO_VOLUME:
            self.volume = payload[0]
            self.volumes.append(payload[0])
            return frame(0xB5, bytes([1]))
        return frame(RESP_ERROR, b"unsupported:%02X" % command)


class Stream:
    """Поток WC (API 57): каталог и открытый файл с позицией."""

    def __init__(self, cwd, content: bytes | None = None, ok: bool = False) -> None:
        self.cwd = cwd
        self.content = content              # что открыто (GFILE, GIPAGPL)
        self.position = 0
        self.ok = ok                        # позиция верна: каталог этим потоком не читался
        self.found: tuple | None = None     # последний объект FENTRY
        self.walk = 0                       # API 58: следующая запись каталога

    def copy(self) -> "Stream":
        clone = Stream(self.cwd, self.content, self.ok)
        clone.position = self.position
        return clone


class Mp3Harness(SpiHarness):
    """Плагин проигрывателя: файл на «карте», клавиши и кадровые прерывания."""

    def __init__(self, content: bytes = b"", name: bytes = b"MUSIC.MP3",
                 esp: AudioEsp | None = None) -> None:
        super().__init__(WMF, SYM)
        self.content = content
        self.name = name
        self.short = short_name(name)
        self.link.core.version = 1
        self.initial_clock = 0
        self.clock = self.initial_clock
        self.turbo_requests = []
        # Каталог панели: имя 8.3 -> (длина, каталог) у своего файла или
        # (длина, каталог, содержимое) у соседних; длинные имена -> 8.3.
        self.files: dict[bytes, tuple] = {self.short: (len(content), False)}
        # Каталог панели для API 58: «..» (подкаталог) и каталоги до файлов —
        # как в записях каталога на носителе («.» и «..» первыми).
        self.parent = False
        self.panel_dirs: list[bytes] = []
        self.long_names = {name.upper(): self.short}
        # Остальная карта: корень тома, имя -> каталог (dict) или файл
        # (содержимое, время FAT, дата FAT).
        self.root: dict = {}
        # Том, откуда загружен WC (STREAM #FD); None — тот же, что у панели.
        self.home: dict | None = None
        self.wc_stream = True               # WC знает STREAM #FD
        # Поток панели: WC открыл файл плагину по кластеру (GIPAG), FENTRY не было.
        self.panel = Stream("panel", content, ok=True)
        # Потоки 0 и 1 и служебный (2) — STREAM #FD.
        self.streams: list[Stream | None] = [None, None, None]
        self.current: int | None = None     # None — поток панели (до первого API 57)
        # TENTRY до своего FENTRY отдаёт чужую запись: что DOS искал последним.
        self.entry = entry_of(b"OTHER.TXT", 1234)
        self.sectors: list[int] = []        # LOAD512: секторов за вызов, все потоки
        self.reads: list[tuple] = []        # LOAD512: (поток, файл, позиция, секторов)
        self.skips: list[tuple] = []        # LOADNONE: (поток, позиция, секторов)
        self.ops: list[tuple] = []          # то и другое по порядку: (поток, позиция, секторов)
        self.keys: dict[int, set[int]] = {}     # функция клавиши -> кадры, когда она нажата
        self.fs_calls: list[int] = []
        self.found_names: list[bytes] = []  # что искал FENTRY, по порядку
        self.vpages: list[int] = []             # MNGCVPL: какие видеостраницы в #C000
        self.sgpages: list[int] = []            # GVsgp: страница графики спрайтов
        self.ts_writes: list[tuple[int, int]] = []
        self.sfile: list[bytes] = []
        self.audio = esp or AudioEsp()
        self.app = self.audio
        # Буквы KBSCN: буква -> кадры, когда она нажата.
        self.key_chars: dict[str, set[int]] = {}
        # Автоповтор WC (PS2P.ASM): счётчик ARR (#76CB) и последняя нажатая
        # клавиша ARK (#76CC) — общие для всех функций клавиш и KBSCN.
        self.repeat_key = None
        self.repeat_counter = 0
        self.key_frame = -1                     # по какой кадр состояние посчитано
        self.key_started: dict = {}             # нажатая клавиша -> кадр нажатия
        # Экраны окон поверх (справка, подробности): адрес дескриптора -> строки.
        self.dialog_screens: dict[int, dict[int, bytearray]] = {}
        self.main_window = self.symbol("Mp3Window")
        self.memory[NAME_ADDRESS:NAME_ADDRESS + len(name) + 1] = name + bytes(1)
        # Страницы плагина из заголовка WMF (+34 — сколько, +36 — пары
        # [страница, секторов]: блоки файла по порядку). Общий стенд кладёт в
        # #8000 только страницу 0; остальные — здесь, по физическим номерам.
        # В #C000 сначала «чужая» страница WC, заполненная RST 0: код страницы 1
        # работает, только если плагин сам подключил её туда (API 0).
        data = WMF.read_bytes()
        self.plugin_pages = data[34]
        self.pages: dict[int, bytearray] = {}   # физ. страница -> 16 КБ (кроме той, что в #C000)
        offset = wmf_code_offset(data)
        for index in range(6):
            page, sectors = data[36 + 2 * index], data[37 + 2 * index]
            if not sectors:
                break
            if page >= self.plugin_pages:
                raise AssertionError("блок файла — в страницу, которой плагин не заказал")
            block = data[offset:offset + 512 * sectors]
            offset += 512 * sectors
            if page:
                self.pages[PLUGIN_BASE + page] = bytearray(block.ljust(0x4000, bytes(1)))
        self.c000 = TEXT_PAGE
        self.memory[0xC000:0x10000] = bytes([POISON]) * 0x4000
        self.memory[WC_PAGE3] = TEXT_PAGE
        self.machine.set_breakpoint(0)
        self.hooks[0] = self.lost_in_c000

    # --- окно #C000 -------------------------------------------------------------------

    def map_c000(self, page: int) -> None:
        """Физическую страницу — в окно #C000, тень #6003 — тоже (как L6011_114 WC)."""
        if page != self.c000:
            self.pages[self.c000] = bytearray(self.memory[0xC000:0x10000])
            content = self.pages.get(page)
            if content is None:
                content = bytes([POISON] if page == TEXT_PAGE else [0]) * 0x4000
            self.memory[0xC000:0x10000] = content
            self.c000 = page
        self.memory[WC_PAGE3] = page
        self.ports.write(0x13AF, page)  # WC API updates both the register and its shadow

    def page_bytes(self, page: int) -> bytes:
        """Содержимое физической страницы, где бы она ни была."""
        if page == self.c000:
            return bytes(self.memory[0xC000:0x10000])
        return bytes(self.pages.get(page, bytes(0x4000)))

    def lost_in_c000(self) -> None:
        raise AssertionError("Z80 в #0000: исполнялась чужая страница в окне #C000 "
                             "(код страницы 1 без Page1_Call или API WC мимо Wc_Api)")

    def ret(self) -> None:
        super().ret()
        pc = self.machine.pc
        if pc >= 0xC000 and not PLUGIN_BASE < self.c000 < PLUGIN_BASE + self.plugin_pages:
            raise AssertionError(f"возврат из API WC в #{pc:04X}, а в окне #C000 — не страница "
                                 f"плагина (#{self.c000:02X}): вызов мимо Wc_Api")

    # --- потоки -------------------------------------------------------------------

    def stream(self) -> Stream:
        return self.panel if self.current is None else self.streams[self.current]

    def stream_reads(self, number: int | None) -> list[tuple[int, int]]:
        """(позиция, секторов) чтений LOAD512 потока (None — потока панели)."""
        return [(position, count) for stream, _, position, count in self.reads
                if stream == number]

    def stream_ops(self, number: int | None) -> list[tuple[int, int]]:
        """(позиция, секторов) чтений и пропусков потока по порядку."""
        return [(position, count) for stream, position, count in self.ops if stream == number]

    def own_sectors(self) -> int:
        """Секторов своего файла, прочитанных или пропущенных потоком звука."""
        read = sum(count for stream, content, _, count in self.reads
                   if stream in (0, None) and content is self.content)
        skipped = sum(count for stream, _, count in self.skips if stream in (0, None))
        return read + skipped

    def port_out(self, port: int, value: int) -> None:
        if port in (TS_CONFIG, TS_FMADDR, TS_PAGE3):
            self.ts_writes.append((port, value))
        if port == TS_PAGE3:
            self.map_c000(value)
        if port == TS_FMADDR and value == 0:
            # Окно закрыто: что легло в SFILE (в стенде — в ОЗУ под окном).
            self.sfile.append(bytes(self.memory[SFILE:SFILE + 18]))
        super().port_out(port, value)

    # --- клавиатура WC (PS2P.ASM) ---------------------------------------------------

    def held_keys(self, frame: int) -> set:
        """Что нажато в кадре: функции клавиш (self.keys) и буквы (self.key_chars)."""
        held = {key for key, frames in self.keys.items() if frame in frames}
        held |= {("char", char.upper()) for char, frames in self.key_chars.items()
                 if frame in frames}
        return held

    def advance_keys(self) -> None:
        """Прерывания WC по текущий кадр: идущая задержка автоповтора — на 1 меньше,
        затем make-коды новых нажатий: ARK — эта клавиша, ARR — #FF."""
        for frame in range(self.key_frame + 1, self.interrupts + 1):
            if self.repeat_counter not in (0, 0xFF):
                self.repeat_counter -= 1
            held = self.held_keys(frame)
            for key in sorted(held - self.held_keys(frame - 1), key=str):
                self.key_started[key] = frame
                self.repeat_key = key
                self.repeat_counter = 0xFF
            for key in [key for key in self.key_started if key not in held]:
                del self.key_started[key]
        self.key_frame = max(self.key_frame, self.interrupts)

    def wc_key_fires(self, key) -> bool:
        """Функция клавиши WC (L7143_89): NZ, только если клавиша нажата и она —
        последняя нажатая (ARK), а ARR — #FF (новое нажатие: ставится задержка
        25 кадров) или 0 (автоповтор). Событие нажатия одно на всех: функция
        клавиши или KBSCN — кто спросил первым, тот его и получил."""
        self.advance_keys()
        if key not in self.held_keys(self.interrupts) or key != self.repeat_key:
            return False
        if self.repeat_counter == 0xFF:
            self.repeat_counter = 0x19
            return True
        return self.repeat_counter == 0

    def handle_api(self) -> None:
        m = self.machine
        # Окно #C000: API 0 и 65 ставят туда страницу плагина и видеостраницу;
        # любой другой вызов, как TXTPR и файловые функции WC, может оставить там
        # свою — в стенде всегда оставляет (Wc_Api плагина возвращает страницу 1).
        if m.a == 0x0E:
            self.turbo_requests.append(m.bc)
            self.clock = self.initial_clock if m.b == 255 else m.c & 3
            self.ret()
            return
        if m.a == FN_MNGC_PL:
            self.calls.append(m.a)
            page = self.memory[base.ALT_A_CELL]
            if page >= self.plugin_pages:
                raise AssertionError(f"MNGC_PL: страницы {page} у плагина нет")
            self.map_c000(PLUGIN_BASE + page)
            self.ret()
            return
        # LOAD512 writes into the caller's mapped buffer (WC restores PAGE3
        # around its internal FAT/driver work). Leave an unrelated page mapped
        # after it to catch a missing remap before SPI or a retry.
        read_page = self.c000
        self.map_c000(VIDEO_BASE + m.alt_a if m.a == FN_MNGCVPL else TEXT_PAGE)
        if m.a in (FN_TENTRY, FN_STREAM, FN_FENTRY, FN_GFILE, FN_GIPAGPL, FN_GDIR):
            self.calls.append(m.a)
            self.fs_calls.append(m.a)
            stream = self.stream()
            if m.a == FN_TENTRY:
                self.memory[m.de:m.de + 32] = self.entry
            elif m.a == FN_STREAM:
                self.flags(zero=self.select_stream(m.de >> 8, m.bc))
            elif m.a == FN_FENTRY:
                stream.ok = False                   # каталог читался этим потоком
                self.find_entry(m, stream)
            elif m.a == FN_GFILE:
                if stream.found is None or stream.found[0] != "file":
                    raise AssertionError("GFILE без найденного файла")
                stream.content, stream.position, stream.ok = stream.found[1], 0, True
            elif m.a == FN_GDIR:
                if stream.found is None or stream.found[0] != "dir":
                    raise AssertionError("GDIR без найденного каталога")
                stream.cwd = stream.found[1]
            else:                                   # GIPAGPL — файл плагина с начала
                stream.content, stream.position, stream.ok = self.content, 0, True
            self.ret()
        elif m.a in (FN_LOAD512, FN_LOADNONE):
            self.calls.append(m.a)
            stream = self.stream()
            if not stream.ok:
                raise AssertionError("чтение после FENTRY без GFILE: поток сбит")
            count = m.bc >> 8
            if m.a == FN_LOAD512:
                self.sectors.append(count)
                self.reads.append((self.current, stream.content, stream.position, count))
                data = stream.content[stream.position:stream.position + 512 * count]
                block = data.ljust(512 * count, b"\xE5")
                if m.hl >= 0xC000:
                    self.map_c000(read_page)
                    self.memory[m.hl:m.hl + len(block)] = block
                    self.map_c000(TEXT_PAGE)
                else:
                    self.memory[m.hl:m.hl + len(block)] = block
                m.hl = (m.hl + 512 * count) & 0xFFFF
            else:
                if not 0 < count <= 255:
                    raise AssertionError("LOADNONE: B вне 1..255")
                self.skips.append((self.current, stream.position, count))
            self.ops.append((self.current, stream.position, count))
            stream.position += 512 * count
            m.a = 0
            self.flags(zero=True)
            self.ret()
        elif m.a == FN_PRWOW:
            # Как GRESB в WCIF.ASM: X/Y=#FF — середина экрана 80×30, и WC
            # записывает вычисленное в дескриптор.
            self.calls.append(m.a)
            ix = m.ix
            self.prwow_windows.append(bytes(self.memory[ix:ix + 16]))
            if self.memory[ix + 2] == 0xFF:
                self.memory[ix + 2] = (80 - self.memory[ix + 4]) // 2
            if self.memory[ix + 3] == 0xFF:
                self.memory[ix + 3] = (30 - self.memory[ix + 5]) // 2
            self.flags(zero=True)
            self.ret()
        elif m.a == FN_MNGCVPL:
            self.calls.append(m.a)
            self.vpages.append(m.alt_a)
            self.ret()
        elif m.a == base.FN_TXTPR and m.ix != self.main_window:
            # Окно поверх: свой экран, основное окно под ним не трогать.
            self.calls.append(m.a)
            saved = self.screen, self.colors
            self.screen = self.dialog_screens.setdefault(m.ix, {})
            self.colors = {}
            self.txtpr(m)
            self.screen, self.colors = saved
            self.ret()
        elif m.a == FN_KBSCN:
            self.calls.append(m.a)
            if self.memory[base.ALT_A_CELL] != 1:
                raise AssertionError("буквы читать по сырой таблице (любая раскладка)")
            # KBSCN режима 1 (L7143_83): первая нажатая из «символьных» клавиш
            # (пробел, Enter, буквы). Если она — последняя нажатая (ARK), её
            # событие забирается, как функцией клавиши, — даже у Enter, символа
            # которого в таблице TAI0 нет.
            self.advance_keys()
            ordinary = [key for key in self.held_keys(self.interrupts)
                        if key in (FN_SPACE, FN_ENTER) or isinstance(key, tuple)]
            ordinary.sort(key=lambda key: (self.key_started.get(key, 0), str(key)))
            char = 0
            if ordinary:
                first = ordinary[0]
                if first != self.repeat_key or self.wc_key_fires(first):
                    if first == FN_SPACE:
                        char = 0x20
                    elif isinstance(first, tuple):
                        char = ord(first[1])
            if char:
                m.a = char
                self.flags()
            else:
                m.a = 0
                self.flags(zero=True)
            self.ret()
        elif m.a == FN_GVSGP:
            self.calls.append(m.a)
            self.sgpages.append(m.c)
            self.ret()
        elif m.a == base.FN_FILEX:
            raise AssertionError("FILEX плагину больше не нужен: свой файл — потоками")
        elif m.a in (FN_SPACE, FN_UP, FN_DOWN, FN_LEFT, FN_RIGHT, FN_PGUP, FN_PGDN, FN_ENTER):
            self.flags(zero=not self.wc_key_fires(m.a))
            self.ret()
        elif m.a == FN_WAITUP:
            # WC ждёт отпускания: клавиша, нажатая сейчас, до конца этого
            # нажатия больше не срабатывает.
            self.calls.append(m.a)
            for frames in list(self.keys.values()) + list(self.key_chars.values()):
                frame_number = self.interrupts
                while frame_number in frames:
                    frames.discard(frame_number)
                    frame_number += 1
            self.ret()
        elif m.a == FN_ADIR:
            self.calls.append(m.a)
            self.stream().walk = 0
            self.flags(zero=True)
            self.ret()
        elif m.a == FN_FINDNEXT:
            self.calls.append(m.a)
            self.find_next(m, self.stream())
            self.ret()
        else:
            super().handle_api()

    def find_next(self, m, stream: Stream) -> None:
        """API 58 в каталоге панели: DE -> [атрибут][имя][0] (полей маска не
        просит), NZ; каталог кончился — Z. Обход сбивает позицию потока."""
        if stream.cwd != "panel" or m.alt_a & 0x7F:
            raise AssertionError("API 58 плагину — только по каталогу панели, без полей")
        listing = ([(b".", True), (b"..", True)] if self.parent else [])
        listing += [(name, True) for name in self.panel_dirs]
        listing += [(short, value[1]) for short, value in self.files.items()]
        stream.ok = False
        if stream.walk >= len(listing):
            self.flags(zero=True)
            return
        name, is_dir = listing[stream.walk]
        stream.walk += 1
        # Короткое имя — строчными, как его отдаёт ядро (Acs): так и в панели.
        out = bytes([0x10 if is_dir else 0x20]) + name.lower() + bytes(1)
        self.memory[m.de:m.de + len(out)] = out
        m.de = (m.de + len(out)) & 0xFFFF
        self.flags()

    def select_stream(self, d: int, bc: int) -> bool:
        """API 57: D=#FE — поток панели копией в 0 и 1, выбран 0; D=#FF —
        корень в текущем потоке; D=#FD — служебный поток в каталоге WC тома WC;
        D=0/1 при BC=#FFFF — выбрать поток. Возврат — флаг Z."""
        if d == STREAM_WC:
            home = self.root if self.home is None else self.home
            folder = next((v for k, v in home.items() if k.upper() == b"WC"), None)
            if not self.wc_stream or not isinstance(folder, dict):
                self.machine.a = 1 if not self.wc_stream else 2
                return False
            self.streams[2] = Stream(folder)
            self.current = 2
            return True
        if d == STREAM_CLONE:
            self.streams[0:2] = [self.panel.copy(), self.panel.copy()]
            self.current = 0
        elif d == STREAM_ROOT:
            if self.current is None:
                raise AssertionError("корень — только в клоне потока")
            stream = self.stream()
            stream.cwd, stream.ok = self.root, False
        elif bc == 0xFFFF and d in (0, 1):
            if self.streams[d] is None:
                raise AssertionError("поток выбран до клонирования")
            self.current = d
        return True

    def find_entry(self, m, stream: Stream) -> None:
        """API 59 в каталоге потока: HL -> [тип][имя,0]; NZ и длина в DE:HL,
        если объект есть. Найденный файл — новая ENTRY DOS (её отдаёт TENTRY)."""
        kind = self.memory[m.hl]
        name = bytes(self.memory[m.hl + 1:m.hl + 257]).split(bytes(1), 1)[0]
        self.found_names.append(name)
        folder = stream.cwd
        if folder == "panel":
            key = self.long_names.get(name.upper(), name.upper())
            found = next((value for short, value in self.files.items() if short.upper() == key),
                         None)
            if found is None or found[1] != (kind == 0x10):
                self.flags(zero=True)
                return
            content = found[2] if len(found) > 2 else self.content
            stream.found = ("file", content)
            size = found[0]
            short = next(short for short in self.files if short.upper() == key)
            self.entry = entry_of(short.upper(), size)
        else:
            key = next((k for k in folder if k.upper() == name.upper()), None)
            item = folder.get(key)
            if item is None or isinstance(item, dict) != (kind == 0x10):
                self.flags(zero=True)
                return
            if isinstance(item, dict):
                stream.found, size = ("dir", item), 0
            else:
                content, time, date = item
                stream.found, size = ("file", content), len(content)
                self.entry = entry_of(key.upper(), size, time, date)
        m.hl = size & 0xFFFF
        m.de = size >> 16
        self.flags()

    def opl_card(self, rom: bytes | None = None) -> None:
        """/WC/ESP с приложением и декодером OPL (и ПЗУ OPL4, если дано)."""
        esp = {b"PLAYER.ELF": (b"\x7fELF" + bytes(700), 0, 0),
               b"OPL.ELF": (b"\x7fELF" + bytes(800), 0, 0)}
        if rom is not None:
            esp[b"YRW801.ROM"] = (rom, 0, 0)
        self.root = {b"WC": {b"ESP": esp}}

    def radio_card(self) -> None:
        """/WC/ESP с приложением и декодером MP3 (поток станции); OPL.ELF нет."""
        self.root = {b"WC": {b"ESP": {b"PLAYER.ELF": (b"\x7fELF" + bytes(700), 0, 0),
                                       b"MP3.ELF": (b"\x7fELF" + bytes(600), 0, 0)}}}

    def midi_card(self) -> None:
        """/WC/ESP с приложением, декодером MIDI и банком (без них MIDI не играет)."""
        self.root = {b"WC": {b"ESP": {b"PLAYER.ELF": (b"\x7fELF" + bytes(700), 0, 0),
                                       b"MIDI.ELF": (b"\x7fELF" + bytes(900), 0, 0),
                                       b"MIDI.SF2": (b"sfbk" + bytes(3000), 0, 0)}}}

    def start(self, call: int = CALL_EXTENSION, index: int = 1, count: int = 1,
              size: int | None = None, extension: int = 0) -> None:
        """Вызов плагина, как это делает Wild Commander: index — номер файла
        среди count объектов панели."""
        m = self.machine
        size = len(self.content) if size is None else size
        m.de = size >> 16
        m.alt_hl, m.alt_de = index, count
        m.alt_a = extension
        self.run(base.CODE, a=call, hl=size & 0xFFFF, bc=NAME_ADDRESS)

    def run(self, pc: int, **registers) -> None:
        # Тот же цикл, что у общего стенда, но кадровое прерывание WC
        # приходит каждые FRAME_TICKS тактов (на HALT — сразу) и двигает его
        # кадровый таймер: по нему плагин опрашивает клавиши.
        m = self.machine
        sp = base.STACK - 2
        self.memory[sp] = base.SENTINEL & 0xFF
        self.memory[sp + 1] = base.SENTINEL >> 8
        m.sp, m.pc = sp, pc
        m.a, m.hl, m.bc = registers.get("a", 0), registers.get("hl", 0), registers.get("bc", 0)
        m.ix = 0x7E00
        busy = 0
        for _ in range(20_000_000):
            slice_ticks = FRAME_TICKS - busy
            m.ticks_to_stop = slice_ticks
            event = m.run()
            busy += slice_ticks - m.ticks_to_stop
            if event & base.BREAKPOINT_HIT and m.pc == base.SENTINEL:
                return
            if event & base.BREAKPOINT_HIT and m.pc == base.API_BREAK:
                self.handle_api()
                continue
            if event & base.BREAKPOINT_HIT and m.pc in self.hooks:
                self.hooks[m.pc]()
                m.step_over_breakpoint()
                continue
            if m.halted:
                self.interrupts += 1
                self.next_frame()
                m.on_handle_active_int()
                busy = 0
            elif busy >= FRAME_TICKS:
                self.interrupts += 1
                self.next_frame()
                busy = 0
            if self.interrupts > MAX_FRAMES:
                break
        raise AssertionError(f"плагин не вернулся в Wild Commander за {MAX_FRAMES} кадров")

    def physical(self, number: int) -> int:
        """Строка экрана для строки макета: у модуля и MIDI альбома нет, и
        строки от формата (кроме громкости) — на одну выше (UiShift)."""
        if number >= FORMAT_ROW and number != VOLUME_ROW and self.memory[self.symbol("UiShift")]:
            return number - 1
        return number

    def line(self, number: int) -> str:
        return bytes(self.screen.get(self.physical(number), b"")).decode("cp866").rstrip()

    def text(self, number: int) -> str:
        """Строка основного окна без отступов."""
        return self.line(number).strip()

    def color(self, number: int, column: int) -> int:
        """Атрибут знака основного окна (столбец с единицы)."""
        return self.colors.get(self.physical(number), [0] * 80)[column - 1]

    def dialog(self, symbol: str, number: int) -> str:
        screen = self.dialog_screens.get(self.symbol(symbol), {})
        return bytes(screen.get(number, b"")).decode("cp866").strip()

    def hint(self) -> str:
        """Подсказка плагина (UiHint): в окне её нет, по ней видно, возможна
        ли перемотка."""
        address = self.word(self.symbol("UiHint"))
        if not address:
            return ""
        return bytes(self.memory[address:address + 80]).split(bytes(1), 1)[0].decode("cp866")

    def at(self, number: int, column: int) -> str:
        """Текст строки основного окна с данного столбца (с единицы)."""
        return self.line(number)[column - 1:]


def volume_row(volume: int, temperature: int = 0) -> str:
    """«Vol ████████░░░░ 78%», температура кристалла ESP («   ESP 47'C», если
    известна) и «H Help» с столбца UI_HELP_COL (текст — со 2-го)."""
    cells = 16 * volume // 255
    text = "Vol " + FULL * cells + SHADE * (16 - cells) + " %d%%" % (100 * volume // 255)
    if temperature:
        text += "   ESP %d'C" % temperature
    return text + " " * (ui("UI_HELP_COL") - 2 - len(text)) + "H Help"


class Mp3PluginTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        need(WMF, SYM)

    def played(self, content: bytes, **esp) -> Mp3Harness:
        h = Mp3Harness(content, esp=AudioEsp(**esp))
        h.start()
        self.assertEqual(h.machine.a, 0, "код возврата 0 — панели не перечитывать")
        self.assertEqual(h.machine.ix, 0x7E00, "IX панели восстановлен")
        self.assertEqual(h.calls[-2:], [base.FN_RRESB, base.FN_GEDPL], "окно закрыто")
        self.assertFalse(h.slave.selected)
        self.assertEqual(h.slave.overruns, 0)
        return h

    def test_header(self) -> None:
        data = WMF.read_bytes()
        self.assertEqual(data[16:32], b"WildCommanderMDL")
        self.assertEqual(data[32], 0x11, "формат #11: список расширений переменной длины")
        self.assertEqual(data[52:54] + data[55:63], bytes(10), "резерв описания списка — нули")
        self.assertEqual(data[63], 0, "и Enter, и F3")
        self.assertEqual(data[64:161], bytes(97), "старое поле 32 расширений у #11 пустое")
        self.assertEqual(data[161:165], b"\xFF" * 4, "размер файла не ограничен")
        self.assertEqual(data[165:197].rstrip(b" "), b"VDAC4 Media Player v1.22")
        self.assertEqual((data[34], data[35]), (2, 0), "две страницы, запуск — страница 0")
        self.assertEqual(data[38], 1, "второй блок файла — в страницу 1")
        self.assertEqual(data[197], 4, "меню выбора просмотрщика, в F10 плеера нет")
        self.assertEqual(wmf_extensions(data), [
            ("MP3", 0), ("WAV", 0), ("AAC", 0), ("OGG", 0), ("OGA", 0), ("FLAC", 0), ("FLA", 0),
            ("MOD", 1), ("XM", 1), ("S3M", 1), ("IT", 1), ("MO", 1),
            ("MID", 2), ("MIDI", 2), ("RMI", 2), ("KAR", 2),
            ("VGM", 3), ("VGZ", 3), ("DRO", 3), ("IMF", 3), ("WLF", 3), ("MWM", 3), ("MFM", 3),
            ("MWK", 4), ("M3U", 5), ("SAA", 6), ("COP", 6),
            ("AS0", 7), ("ASC", 7), ("FTC", 7), ("GTR", 7), ("PSC", 7), ("PSM", 7), ("PT1", 7),
            ("PT2", 7), ("PT3", 7), ("SQT", 7), ("ST3", 7), ("STC", 7), ("STP", 7), ("TS", 7),
            ("TFC", 7), ("TFD", 7), ("MTC", 7)],
            "весь список одного плагина: потоки, модули, MIDI, OPL, набор MWM (чтобы его "
            "пропустить), радио, SAA, AY/TurboSound/TFC; код обработчика — вид файла")
        names = [name for name, _ in wmf_extensions(data)]
        self.assertEqual(len(names), len(set(names)), "дубликатов нет: WC их не принимает")
        self.assertTrue(all(33 <= ord(c) < 127 and c not in "./\\*?" for n in names for c in n),
                        "только допустимые знаки, записи из пробелов нет")
        self.assertEqual(data[204:228].rstrip(b" "), b"Play on VDAC4")

    def test_whole_file_reaches_the_esp(self) -> None:
        rng = random.Random(7)
        for size in (0, 1, 511, 512, 513, 1023, 1024, 1025, 4000, 70000):
            with self.subTest(size=size):
                content = rng.randbytes(size)
                h = self.played(content)
                esp = h.audio
                self.assertEqual(bytes(esp.received), content)
                self.assertTrue(all(0 < block <= 1024 for block in esp.blocks))
                self.assertEqual(h.own_sectors(), -(-size // 512), "с карты прочитано ровно нужное")
                # Плагин 1.21: после PING — VERSION Tang (есть ли экран на HDMI)
                self.assertEqual(esp.commands[:3], [CMD_PING, TANG_READ, AUDIO_OPEN])
                self.assertEqual(esp.opens, [bytes([0]) + size.to_bytes(4, "little") + bytes([3]) +
                                             b"MP3"],
                                 "поток MP3 открывается с длиной (по ней ESP считает время); "
                                 "флаги: перемотка есть, свой файл найден, метки нет; "
                                 "расширение — по нему PLAYER выбирает декодер")
                self.assertEqual(esp.closes, [1], "файл отдан: доиграть накопленное")
                self.assertEqual((esp.state, esp.finished), (STATE_IDLE, True),
                                 "плагин дождался конца звука")
                self.assertEqual(esp.rejected, 0)

    @staticmethod
    def id3(size: int, footer: bool = False) -> bytes:
        """Метка ID3v2.4 длиной size байтов после заголовка."""
        digits = bytes([(size >> 21) & 0x7F, (size >> 14) & 0x7F, (size >> 7) & 0x7F,
                        size & 0x7F])
        body = bytes(index * 13 & 0xFF for index in range(size))
        return (b"ID3\x04\x00" + bytes([0x10 if footer else 0]) + digits + body +
                (b"3DI\x04\x00\x10" + digits if footer else b""))

    def test_sd_read_ahead_is_independent_of_esp_packet_size(self) -> None:
        data=random.Random(1988).randbytes(8*8192+513)
        h=self.played(data,capacity=3072,played_per_status=700)
        self.assertEqual(bytes(h.audio.received),data)
        reads=[r for r in h.reads if r[0] in (0,None) and r[1] is h.content]
        self.assertTrue(any(r[3]==16 for r in reads),'SD reads up to eight KiB per request')
        self.assertLessEqual(len(reads),11,'one probe plus batched reads, not one per ESP packet')
        self.assertTrue(all(0<n<=1024 for n in h.audio.blocks))
        self.assertEqual(h.own_sectors(),-(-len(data)//512))
        self.assertEqual(h.audio.rejected,0)

    def test_big_id3_tag_is_skipped_on_the_card(self) -> None:
        # Обложка в метке: в ESP уходит только звук; секторы метки, кроме
        # первого и последнего, не читаются вовсе (LOADNONE).
        rng = random.Random(31)
        audio = b"\xFF\xFB\x50\x00" + rng.randbytes(20000)
        for size, footer in ((502, False), (1014, False), (3000, False), (70000, True),
                             (300000, False)):
            with self.subTest(size=size, footer=footer):
                tag = self.id3(size, footer)
                content = tag + audio
                h = self.played(content)
                self.assertEqual(bytes(h.audio.received), audio)
                self.assertEqual(h.own_sectors(), -(-len(content) // 512),
                                 "каждый сектор прочитан или пропущен один раз")
                skipped = sum(count for stream, _, count in h.skips if stream == 0)
                self.assertEqual(skipped, max(0, len(tag) // 512 - 1),
                                 "целые секторы метки после первого — без чтения")
                self.assertEqual(h.audio.opens, [bytes([0]) + len(audio).to_bytes(4, "little") +
                                                 bytes([7]) + b"MP3"], "длина потока — без метки")

    def test_small_or_odd_tags_go_to_the_esp(self) -> None:
        # Метку в пределах сектора и всё, что меткой только похоже, пропускает
        # (или разбирает) ESP: плагин отдаёт файл как есть.
        rng = random.Random(32)
        cases = (self.id3(400) + rng.randbytes(5000),
                 b"ID3\x04\x00\x00\x00\x80\x00\x00" + rng.randbytes(5000),   # не 7-битный
                 b"ID3\x04\x00\x00\x00\x01\x00\x00" + rng.randbytes(3000),   # длиннее файла
                 b"ID3" + bytes(5))
        for content in cases:
            with self.subTest(head=content[:10].hex()):
                h = self.played(content)
                self.assertEqual(bytes(h.audio.received), content)
                self.assertEqual(h.skips, [])

    def test_block_is_sent_only_when_it_fits(self) -> None:
        # Очередь всего на три блока и убывает медленно: плагин обязан ждать.
        content = random.Random(8).randbytes(20000)
        h = self.played(content, capacity=3072, played_per_status=700)
        self.assertEqual(bytes(h.audio.received), content)
        self.assertEqual(h.audio.rejected, 0, "ни один блок не отправлен в занятую очередь")
        self.assertGreater(h.interrupts, 0, "ожидание места идёт через HALT, а не впустую")

    def test_wav_is_a_stream_with_its_own_name(self) -> None:
        content = b"RIFF" + bytes(40000)
        h = Mp3Harness(content, name=b"Take 1.wav", esp=AudioEsp())
        h.start()
        [opened] = h.audio.opens
        self.assertEqual(len(opened), 9, "поток, как MP3: [0][длина][флаги][расширение]")
        self.assertEqual(opened[6:], b"WAV")
        self.assertEqual(bytes(h.audio.received), content)
        self.assertEqual(h.at(FORMAT_ROW, TEXT_MP3), "WAV  128 kbps  44.1 kHz  stereo")

    def test_window_shows_file_and_progress(self) -> None:
        # Встроенный плеер: ни меток, ни AUDIO_VIEW — название по имени файла,
        # время из записи состояния, полоса — по отданному в декодер.
        content = bytes(70000)
        h = self.played(content)
        bar = ui("UI_BAR_MP3")
        [window] = h.prwow_windows
        self.assertEqual(window[2:4], bytes([0xFF, 0xFF]), "окно центрирует WC")
        self.assertEqual(window[4:7], bytes([66, 21, 0x0D]), "66×21, чёрный фон, рамка голубая")
        self.assertEqual(h.at(TITLE_ROW, TEXT_MP3), "MUSIC.MP3", "слева — место обложки")
        self.assertEqual(h.color(TITLE_ROW, TEXT_MP3), 0x0F, "название ярко-белым")
        self.assertEqual(h.text(SECOND_ROW), "")
        self.assertEqual(h.text(THIRD_ROW), "")
        self.assertEqual(h.at(FORMAT_ROW, TEXT_MP3), "MP3  128 kbps  44.1 kHz  stereo")
        self.assertEqual(h.color(FORMAT_ROW, TEXT_MP3), 0x05)
        self.assertEqual(h.at(TRACK_ROW, TEXT_MP3), "Track 1 of 1")
        self.assertEqual(h.at(TIME_ROW, TEXT_MP3), "00:04 " + PLAYED * bar + " --:--",
                         "файл сыгран целиком, длительность неизвестна")
        self.assertEqual(h.color(TIME_ROW, TEXT_MP3 + 6), 0x0D)
        self.assertEqual(h.at(STATUS_ROW, TEXT_MP3), "", "доигрывание в окне не пишется")
        self.assertEqual(h.color(STATUS_ROW, TEXT_MP3), 0x07)
        self.assertEqual(h.at(VOLUME_ROW, 2), volume_row(200),
                         "громкость 200 из 255; «H Help» в правом нижнем углу")
        self.assertEqual(h.color(VOLUME_ROW, 6), 0x0C)
        self.assertEqual(h.color(VOLUME_ROW, ui("UI_HELP_COL")), 0x0E, "H — жёлтым")
        for row in range(BARS_ROW, NUMBERS_ROW + 1):
            self.assertEqual(h.text(row), "", "без AUDIO_VIEW столбиков нет")
        self.assertEqual(h.audio.commands.count(AUDIO_VIEW), 1, "после отказа — не спрашивать")
        self.assertEqual(h.hint(), HINT_PLAY)

    def test_info_window_shows_the_counters(self) -> None:
        esp = AudioEsp()
        esp.decode_errors, esp.underruns, esp.i2s_faults = 12, 345, 65535
        h = Mp3Harness(bytes(70000), esp=esp)
        h.key_chars = {"i": {3, 4, 5}}          # держится три кадра — одно нажатие
        h.start()
        self.assertEqual([window[4:7] for window in h.prwow_windows],
                         [bytes([66, 21, 0x0D]), bytes([50, 11, 0x70])])
        self.assertEqual(h.calls.count(base.FN_RRESB), 2, "при выходе закрыты оба окна")
        self.assertEqual(h.dialog("UiInfoWindow", INFO_FILE), "File  : MUSIC.MP3")
        self.assertEqual(h.dialog("UiInfoWindow", INFO_FORMAT),
                         "Format: 44100 Hz, 2 ch, 128 kbps")
        self.assertEqual(h.dialog("UiInfoWindow", INFO_SENT), "Sent  : 68 of 68 KB")
        self.assertEqual(h.dialog("UiInfoWindow", INFO_FAULTS),
                         "Faults: decode 12, starve 345, i2s 65535")
        self.assertEqual(h.dialog("UiInfoWindow", INFO_VOLUME), "Volume: 200")
        self.assertEqual(bytes(h.audio.received), bytes(70000))

    def test_rate_above_65535(self) -> None:
        # 96 и 88,2 кГц (FLAC 24/96, TSFM на AY): поле LE16 — младшие 16 бит, флаг
        # ST_F_RATE_HIGH — ещё 65536. Прежде 96000 показывалось как 30.4 kHz.
        for rate, line, info in ((96000, "96 kHz", "96000 Hz"), (88200, "88.2 kHz", "88200 Hz"),
                                 (65535, "65.5 kHz", "65535 Hz"), (65536, "65.5 kHz", "65536 Hz"),
                                 (22050, "22 kHz", "22050 Hz")):
            with self.subTest(rate=rate):
                esp = AudioEsp()
                esp.rate = rate
                h = Mp3Harness(bytes(70000), esp=esp)
                h.key_chars = {"i": {3, 4, 5}}
                h.start()
                self.assertEqual(h.at(FORMAT_ROW, TEXT_MP3), f"MP3  128 kbps  {line}  stereo")
                self.assertEqual(h.dialog("UiInfoWindow", INFO_FORMAT),
                                 f"Format: {info}, 2 ch, 128 kbps")

    def test_help_window_and_escape(self) -> None:
        # H — справка; Esc сначала закрывает её, второй Esc — стоп.
        content = random.Random(13).randbytes(200_000)
        h = Mp3Harness(content, esp=AudioEsp(capacity=4096, played_per_status=512))
        h.key_chars = {"h": {4}}
        h.esc_after = 30
        h.start()
        [main, help_window] = h.prwow_windows
        self.assertEqual(help_window[4:7], bytes([40, 14, 0x70]))
        self.assertEqual(h.dialog("UiHelpWindow", 2), "Space   Pause / resume")
        self.assertEqual(h.dialog("UiHelpWindow", 5), "PgDn    Next file")
        self.assertEqual(h.dialog("UiHelpWindow", 7), "I       Technical info")
        self.assertEqual(h.dialog("UiHelpWindow", 11), "Esc  Close")
        self.assertEqual(h.calls.count(base.FN_RRESB), 2)
        self.assertEqual(h.audio.closes, [0])
        self.assertEqual(h.esc_polls, 31, "первый Esc закрыл справку, второй — стоп")

    def test_help_key_toggles(self) -> None:
        h = Mp3Harness(bytes(70000))
        h.key_chars = {"h": {3, 4}, "H": {9}}
        h.start()
        self.assertEqual(len(h.prwow_windows), 2)
        self.assertEqual(h.calls.count(base.FN_RRESB), 2, "второе нажатие закрыло справку")
        self.assertEqual(h.at(TITLE_ROW, TEXT_MP3), "MUSIC.MP3", "окно перерисовано")

    def test_long_name_and_control_bytes_do_not_break_the_window(self) -> None:
        name = b"A" * 60 + b"\x0D\x01.MP3"
        h = Mp3Harness(bytes(100), name=name)
        h.start()
        self.assertEqual(h.at(TITLE_ROW, TEXT_MP3), "A" * 40, "имя обрезано")
        short = Mp3Harness(bytes(100), name=b"A\x0DB\x01.MP3")
        short.start()
        self.assertEqual(short.at(TITLE_ROW, TEXT_MP3), "A.B..MP3",
                         "управляющие байты заменены точками")

    def test_tags_are_shown(self) -> None:
        cases = ((b"Song\0Artist\0Album\0" b"2001\0" b"3\0", "Song", "Artist", "Album, 2001"),
                 (b"Song\0\0Album\0\0\0", "Song", "", "Album"),
                 (b"\0Artist\0\0" b"1999\0\0", "MUSIC.MP3", "Artist", "1999"),
                 (b"T" * 120 + b"\0" + b"A" * 70 + b"\0\0\0\0", "T" * 45, "A" * 45, ""))
        for tags, title, artist, third in cases:
            with self.subTest(tags=tags[:20]):
                esp = AudioEsp()
                esp.tags_known, esp.tags = True, tags
                h = Mp3Harness(Mp3PluginTest.id3(300) + bytes(20000), esp=esp)
                h.start()
                self.assertIn(AUDIO_TAGS, esp.commands)
                self.assertNotIn(AUDIO_INFO, esp.commands, "метки есть — AUDIO_INFO не нужен")
                self.assertEqual(h.at(TITLE_ROW, TEXT_MP3), title)
                self.assertEqual(h.at(SECOND_ROW, TEXT_MP3), artist)
                self.assertEqual(h.at(THIRD_ROW, TEXT_MP3), third)
                if artist:
                    self.assertEqual(h.color(SECOND_ROW, TEXT_MP3), 0x0E, "исполнитель жёлтым")

    def test_spectrum_bars(self) -> None:
        levels = [(index * 5) % 13 for index in range(32)]
        esp = AudioEsp(capacity=16384, played_per_status=1024)
        esp.view_known, esp.view_values, esp.view_duration = True, levels, 300
        h = Mp3Harness(bytes(300_000), esp=esp)
        h.esc_after = 80
        h.start()
        bar = ui("UI_BAR_MP3")
        self.assertGreater(esp.view_polls, 3)
        for row in range(6):
            line = h.at(BARS_ROW + row, 2)
            threshold = (6 - row) * 2
            expected = "".join(FULL if level >= threshold else
                               LOWER if level + 1 == threshold else " " for level in levels)
            self.assertEqual(line[0:63:2].ljust(32), expected, "строка %d" % row)
            self.assertEqual(line[1:63:2].strip(), "", "между столбиками — пробел")
        self.assertEqual(h.color(BARS_ROW + 5, 2 + 2 * 1), 0x0C, "столбики ярко-зелёным")
        self.assertEqual(h.text(NUMBERS_ROW), "", "у спектра номеров нет")
        # Время — из AUDIO_VIEW, полоса — доля от длительности.
        time = h.at(TIME_ROW, TEXT_MP3)
        self.assertTrue(time.endswith(" 05:00"), time)
        minutes, seconds = int(time[0:2]), int(time[3:5])
        played = minutes * 60 + seconds
        self.assertGreater(played, 0)
        self.assertEqual(time[6:6 + bar],
                         PLAYED * (bar * played // 300) + LEFT * (bar - bar * played // 300))

    def test_peaks_hold_then_fall(self) -> None:
        # Столбик подскочил до 12 и упал до 0: пик стоит 8 опросов, потом
        # опускается на ступень за опрос.
        esp = AudioEsp(capacity=16384, played_per_status=1024)
        esp.view_known = True
        esp.view_values = lambda poll: [12 if poll == 0 else 0] + [0] * 31
        h = Mp3Harness(bytes(300_000), esp=esp)
        h.esc_after = 150                   # Esc опрашивается на каждом блоке
        tops = []
        colors = set()
        address = h.symbol("Ui_DrawTime")
        h.machine.set_breakpoint(address)

        def look() -> None:
            column = [h.at(BARS_ROW + row, 2)[:1] or " " for row in range(6)]
            tops.append("".join(column))
            if column[0] == UPPER:
                colors.add(h.color(BARS_ROW, 2))
        h.hooks[address] = look
        h.start()
        shown = [top for top in tops if top.strip()]
        self.assertEqual(shown[0], FULL * 6, "сначала — столбик во всю высоту")
        steps = [top for index, top in enumerate(shown) if index == 0 or top != shown[index - 1]]
        self.assertEqual(steps[:4], [FULL * 6, UPPER + "     ", LOWER + "     ",
                                     " " + UPPER + "    "],
                         "пик остался наверху, потом опускается по полстроки")
        self.assertGreaterEqual(shown.count(UPPER + "     "), 8, "пик стоит")
        self.assertEqual(colors, {0x0A}, "пик — ярко-красным")

    def test_old_firmware_keeps_the_view_off(self) -> None:
        h = self.played(bytes(30000))
        self.assertEqual(h.audio.commands.count(AUDIO_VIEW), 1)
        self.assertEqual(h.audio.commands.count(AUDIO_TAGS), 0, "без метки меток не спрашивать")

    def test_escape_stops_the_sound_at_once(self) -> None:
        content = random.Random(9).randbytes(60000)
        h = Mp3Harness(content, esp=AudioEsp(capacity=4096, played_per_status=512))
        h.esc_after = 12
        h.start()
        esp = h.audio
        self.assertEqual(esp.closes, [0], "Esc обрывает звук, а не ждёт конца очереди")
        self.assertLess(len(esp.received), len(content))
        self.assertEqual(bytes(esp.received), content[:len(esp.received)])
        self.assertEqual(h.calls[-2:], [base.FN_RRESB, base.FN_GEDPL])

    def test_mwm_kit_is_skipped_in_the_same_direction(self) -> None:
        # Набор волн MWM не играется: код прошлого выхода (куда шёл плеер) — сразу,
        # без окна и без ESP; открыт сам (0) — просто выход; код — в ноль.
        for previous in (2, 4, 0):
            with self.subTest(previous=previous):
                audio = AudioEsp()
                h = Mp3Harness(b"MBMS\x10\x0d" + bytes(3000), name=b"POWERKIT.MWK", esp=audio)
                h.memory[h.symbol("Mp3ExitCode")] = previous
                h.start()
                self.assertEqual(h.machine.a, previous)
                self.assertEqual(h.memory[h.symbol("Mp3ExitCode")], 0)
                self.assertEqual(audio.commands, [], "ESP не трогается")
                self.assertNotIn(base.FN_GEDPL, h.calls, "экран WC не трогается")

    def test_esp_temperature_in_the_volume_row(self) -> None:
        audio = AudioEsp()
        audio.temperature = 47
        h = Mp3Harness(bytes(30000), esp=audio)
        h.start()
        self.assertEqual(h.at(VOLUME_ROW, 2), volume_row(200, 47))

    def test_hot_esp_temperature_is_yellow_then_red(self) -> None:
        # С v1.17: от 70 °C «ESP NN'C» ярко-жёлтым, от 75 °C — ярко-красным;
        # текст строки тот же, «H Help» — своим цветом.
        for temperature, color in ((47, 0x07), (69, 0x07), (70, 0x0E), (74, 0x0E),
                                   (75, 0x0A), (90, 0x0A)):
            with self.subTest(temperature=temperature):
                audio = AudioEsp()
                audio.temperature = temperature
                h = Mp3Harness(bytes(30000), esp=audio)
                h.start()
                row = volume_row(200, temperature)
                self.assertEqual(h.at(VOLUME_ROW, 2), row)
                esp = 2 + row.index("ESP")
                self.assertEqual(h.color(VOLUME_ROW, esp), color)
                self.assertEqual(h.color(VOLUME_ROW, esp + len("ESP %d'C" % temperature) - 1), color)
                self.assertEqual(h.color(VOLUME_ROW, 2 + row.index("H Help")), 0x0E, "«H» — жёлтым")
                self.assertEqual(h.color(VOLUME_ROW, 3 + row.index("H Help")), 0x07, "«Help» — белым")

    def test_volume_keys(self) -> None:
        # Клавиша удерживается много кадров: громкость идёт шагами по 16 и
        # упирается в границы, не перескакивая через них.
        h = Mp3Harness(bytes(30000), esp=AudioEsp(capacity=3072, played_per_status=256))
        h.keys = {FN_UP: set(range(3, 40)), FN_DOWN: set(range(60, 200))}
        h.start()
        volumes = h.audio.volumes
        self.assertEqual(volumes[:4], [216, 232, 248, 255])
        self.assertEqual(max(volumes), 255)
        down = volumes[volumes.index(255) + volumes.count(255):]
        self.assertEqual(down[:3], [239, 223, 207])
        self.assertEqual(h.audio.volume, 0, "вниз — до нуля и не дальше")
        self.assertEqual(bytes(h.audio.received), bytes(30000), "клавиши не мешают потоку")

    def test_14mhz_during_playback_and_clock_restored_on_every_exit(self) -> None:
        for initial in (0, 1, 2, 3):
            for route in ('end', 'escape', 'next', 'previous', 'error', 'menu'):
                with self.subTest(initial=initial,route=route):
                    h=Mp3Harness(bytes(40000))
                    h.initial_clock=h.clock=initial
                    if route in ('escape','error','menu'):h.esc_after=15
                    if route=='error':h.audio.online=False
                    if route=='next':h.keys={FN_PGDN:set(range(6,12))}
                    if route=='previous':h.keys={FN_PGUP:set(range(6,12))}
                    seen=[]
                    def reading():
                        seen.append(h.clock)
                        self.assertEqual(h.clock,2)
                    h.hooks[h.symbol('Mp3_ReadBlock')]=reading
                    h.machine.set_breakpoint(h.symbol('Mp3_ReadBlock'))
                    h.start(CALL_MENU if route=='menu' else CALL_EXTENSION,index=2,count=5)
                    self.assertEqual(h.turbo_requests,[2,0xFF00])
                    self.assertEqual(h.clock,initial)
                    if route=='end':self.assertTrue(seen)

    def test_viewer_menu_call_plays_too(self) -> None:
        h = Mp3Harness(b"x" * 300)
        h.start(CALL_VIEWER, index=2, count=9)
        self.assertEqual(bytes(h.audio.received), b"x" * 300)
        self.assertEqual(h.machine.a, 0, "из меню просмотрщика WC по файлам не ходит")

    def test_next_file_follows_when_the_track_ends(self) -> None:
        for index, count, code in ((3, 9, EXIT_NEXT), (9, 9, 0), (1, 1, 0)):
            with self.subTest(index=index, count=count):
                h = Mp3Harness(bytes(5000))
                h.start(index=index, count=count)
                self.assertEqual(h.machine.a, code)
                self.assertEqual(h.audio.closes, [1], "файл доигран до конца")
                self.assertEqual(bytes(h.audio.received), bytes(5000))

    def test_page_keys_go_to_the_neighbour_file(self) -> None:
        content = random.Random(11).randbytes(60000)
        for key, code in ((FN_PGDN, EXIT_NEXT), (FN_PGUP, EXIT_PREV)):
            with self.subTest(key=key):
                h = Mp3Harness(content, esp=AudioEsp(capacity=4096, played_per_status=512))
                h.keys = {key: set(range(6, 12))}
                h.start(index=4, count=9)
                self.assertEqual(h.machine.a, code)
                self.assertEqual(h.audio.closes, [0], "звук обрывается сразу")
                self.assertLess(len(h.audio.received), len(content))
                self.assertEqual(h.calls[-2:], [base.FN_RRESB, base.FN_GEDPL], "окно закрыто")
        # За последним файлом следующего нет: PgDn просто останавливает.
        h = Mp3Harness(content, esp=AudioEsp(capacity=4096, played_per_status=512))
        h.keys = {FN_PGDN: set(range(6, 12))}
        h.start(index=9, count=9)
        self.assertEqual((h.machine.a, h.audio.closes), (0, [0]))

    def test_space_pauses_and_resumes(self) -> None:
        content = random.Random(12).randbytes(30000)
        h = Mp3Harness(content, esp=AudioEsp(capacity=4096, played_per_status=512))
        h.keys = {FN_SPACE: set(range(5, 12)) | set(range(40, 47))}
        h.start()
        self.assertEqual(h.audio.pauses, [1, 0])
        self.assertEqual(h.calls.count(FN_WAITUP), 2, "одно нажатие — одно переключение")
        self.assertEqual(bytes(h.audio.received), content, "после паузы файл доходит целиком")
        self.assertEqual(h.at(STATUS_ROW, TEXT_MP3), "", "доигрывание в окне не пишется")

    def test_pause_is_shown_and_survives_old_firmware(self) -> None:
        # Пауза стоит до Esc: в окне «Paused». Очередь при этом заполняется.
        h = Mp3Harness(bytes(30000), esp=AudioEsp(capacity=8192, played_per_status=512))
        h.keys = {FN_SPACE: set(range(5, 12))}
        h.esc_after = 60
        h.start()
        self.assertEqual(h.audio.pauses, [1])
        self.assertEqual(h.at(STATUS_ROW, TEXT_MP3), "Paused")
        # Прошивка без команды паузы: плагин не считает звук остановленным.
        old = Mp3Harness(bytes(3000))
        old.audio.pause_known = False
        old.keys = {FN_SPACE: set(range(1, 400))}
        old.start()
        self.assertEqual(bytes(old.audio.received), bytes(3000))
        self.assertEqual(old.at(STATUS_ROW, TEXT_MP3), "")

    def test_buffering_is_shown(self) -> None:
        h = Mp3Harness(bytes(30000), esp=AudioEsp(capacity=4096, played_per_status=64))
        h.audio.buffering = True
        h.esc_after = 30
        h.start()
        self.assertEqual(h.at(STATUS_ROW, TEXT_MP3), "Buffering")

    def test_status_body_feeds_the_tang_screen(self) -> None:
        # Плагин 1.19: в теле каждого AUDIO_STATUS — то, что знает только он (экран оверлеем
        # Tang в PLAYER 1.21): имя, окно поверх, сколько отдано, строка состояния; на выходе
        # Esc — последний запрос с флагом ухода.
        h = Mp3Harness(bytes(80 * 1024), esp=AudioEsp(played_per_status=2048))
        h.esc_after = 200
        h.start()
        blocks = h.audio.ui_blocks
        self.assertTrue(blocks and all(b is not None for b in blocks), 'тело — в каждом запросе')
        self.assertTrue(all(b['size'] <= 11 + 2 * 64 for b in blocks))
        first, last = blocks[0], blocks[-1]
        self.assertEqual(first['flags'], UI_F_OPEN)
        self.assertEqual(first['page'], 0)
        self.assertTrue(first['name'], 'имя файла есть')
        self.assertEqual(last['flags'], UI_F_OPEN | UI_F_LEAVING, 'Esc — уход сразу')
        self.assertTrue(all(b['flags'] == UI_F_OPEN for b in blocks[:-1]))
        self.assertEqual(sum(1 for b in blocks if b['flags'] & UI_F_LEAVING), 1)
        sent = [b['sent'] for b in blocks]
        self.assertEqual(sent, sorted(sent), 'отдано — только растёт')
        self.assertEqual(sent[-1], 80 * 1024)

    def test_leaving_flag_does_not_outlive_the_call(self) -> None:
        # Страница плагина живёт в памяти Evo между вызовами: флаг ухода прошлого выхода не
        # должен доставаться следующему вызову. В 1.19 он оставался — PLAYER считал окно
        # закрытым, и экран оверлеем Tang показывался только до первого выхода в WC.
        h = Mp3Harness(bytes(30000), esp=AudioEsp(played_per_status=2048))
        h.esc_after = 30
        h.start()
        first = len(h.audio.ui_blocks)
        self.assertEqual(h.audio.ui_blocks[-1]['flags'], UI_F_OPEN | UI_F_LEAVING)
        h.esc_polls = 0
        h.start()
        second = h.audio.ui_blocks[first:]
        self.assertGreater(len(second), 1)
        self.assertTrue(all(b['flags'] == UI_F_OPEN for b in second[:-1]), 'окно снова открыто')
        self.assertEqual(second[-1]['flags'], UI_F_OPEN | UI_F_LEAVING)

    def test_silent_esp_does_not_outlive_the_call(self) -> None:
        # ESP не ответила в прошлый раз — в следующий вызов прощальный запрос снова уходит.
        h = Mp3Harness(bytes(50000))
        calls = 0

        def app(command: int, payload: bytes) -> bytes:
            nonlocal calls
            calls += 1
            return b"" if calls > 12 else h.audio(command, payload)
        h.app = app
        h.esc_after = 40
        h.start()
        self.assertEqual(h.at(STATUS_ROW, TEXT_MP3), "ERROR: ESP does not answer")
        h.app = h.audio
        h.esc_polls = 0
        first = len(h.audio.ui_blocks)
        h.start()
        second = h.audio.ui_blocks[first:]
        self.assertTrue(second and second[-1]['flags'] == UI_F_OPEN | UI_F_LEAVING,
                        'на выходе — прощальный запрос')

    def test_progress_bar_follows_the_decoder(self) -> None:
        # Файл в 80 КБ отдан целиком, часть его ещё в очереди: полоса показывает
        # то, что уже ушло в декодер.
        h = Mp3Harness(bytes(80 * 1024), esp=AudioEsp(played_per_status=2048))
        h.esc_after = 200
        h.start()
        bar = ui("UI_BAR_MP3")
        self.assertEqual(len(h.audio.received), 80 * 1024)
        self.assertEqual(h.audio.closes, [1, 0], "Esc пришёлся на доигрывание")
        line = h.at(TIME_ROW, TEXT_MP3)
        cells = line[6:6 + bar]
        self.assertEqual((line[5], line[6 + bar:]), (" ", " --:--"))
        played = 80 - (h.audio.status_queue >> 10)
        self.assertTrue(0 < played < 80)
        full = bar * played // 80
        self.assertEqual(cells, PLAYED * full + LEFT * (bar - full))
        self.assertEqual(h.color(TIME_ROW, TEXT_MP3 + 6 + full), 0x01, "остаток — синим")

    def test_no_esp(self) -> None:
        h = Mp3Harness(b"data")
        h.link.die()
        h.esc_after = 3
        h.start()
        self.assertEqual(h.at(STATUS_ROW, TEXT_MP3), "ERROR: no ESP on the SPI")
        self.assertEqual(h.color(STATUS_ROW, TEXT_MP3), 0x0A, "ошибка — красным")
        self.assertEqual(h.sectors, [], "карта не читалась")
        self.assertEqual(h.text(VOLUME_ROW), "H Help", "громкость ESP не назвала — её нет")
        self.assertEqual(h.machine.a, 0)

    def test_firmware_without_audio_shows_its_answer(self) -> None:
        h = Mp3Harness(b"data")
        h.audio.supported = False
        h.esc_after = 3
        h.start()
        self.assertEqual(h.at(STATUS_ROW, TEXT_MP3), "unsupported:30")
        self.assertEqual(h.audio.received, b"")

    def test_i2s_is_off(self) -> None:
        h = Mp3Harness(b"data")
        h.audio.online = False
        h.esc_after = 3
        h.start()
        self.assertEqual(h.at(STATUS_ROW, TEXT_MP3), "ERROR: ESP audio (I2S) is off")
        self.assertEqual(h.sectors, [1], "прочитан только первый сектор: есть ли метка")
        self.assertEqual(h.audio.received, b"")

    def test_esp_falling_silent_in_the_middle(self) -> None:
        h = Mp3Harness(bytes(50000))
        calls = 0

        def app(command: int, payload: bytes) -> bytes:
            nonlocal calls
            calls += 1
            return b"" if calls > 12 else h.audio(command, payload)
        h.app = app
        h.esc_after = 40                    # Esc — уже после сообщения об ошибке
        h.start()
        self.assertLess(len(h.audio.received), 50000)
        self.assertEqual(h.at(STATUS_ROW, TEXT_MP3), "ERROR: ESP does not answer")

    def test_menu_call_without_a_file(self) -> None:
        h = Mp3Harness()
        h.esc_after = 5
        h.start(CALL_MENU)
        self.assertEqual(h.at(TITLE_ROW, TEXT_MODULE), "(no file)", "без обложки — с края")
        self.assertEqual(h.at(THIRD_ROW, TEXT_MODULE),
                         "Enter on MP3, MOD, XM, S3M, IT, MID; DAC test: audio_test")
        self.assertEqual(h.at(STATUS_ROW, TEXT_MODULE), "Open a file from the panel")
        self.assertNotIn(AUDIO_OPEN, h.audio.commands)
        self.assertEqual(h.sectors, [])
        self.assertEqual(h.machine.a, 0)


class TangScreenTest(unittest.TestCase):
    """Плагин 1.21 и экран проигрывателя на HDMI Tang (PLAYER 1.26, screen.asm): экран показан —
    окна плагина на Evo нет (ни окон WC, ни текста), и по SPI идут только звук и тело
    AUDIO_STATUS: спектра, меток и миниатюры окну не нужно."""

    TANG = 0x0101011E                       # личность 01 (TS-Config), версия 1.30
    WINDOW_ONLY = {AUDIO_VIEW, AUDIO_TAGS, AUDIO_INFO, AUDIO_THUMB}

    def harness(self, content: bytes = bytes(40 * 1024), name: bytes = b"MUSIC.MP3",
                **esp) -> Mp3Harness:
        audio = AudioEsp(played_per_status=2048)
        audio.view_known = audio.tags_known = True
        for key, value in esp.items():
            setattr(audio, key, value)
        return Mp3Harness(content, name=name, esp=audio)

    def assert_no_window(self, h: Mp3Harness) -> None:
        for api in (FN_PRWOW, base.FN_TXTPR, base.FN_RRESB):
            self.assertNotIn(api, h.calls, f'окно на Evo: API {api}')
        self.assertFalse(self.WINDOW_ONLY & set(h.audio.commands),
                         'по SPI — только звук и состояние')

    def test_screen_shown_no_window_and_no_extra_spi(self) -> None:
        content = random.Random(21).randbytes(40 * 1024)
        h = self.harness(content, tang_version=self.TANG, screen=SCREEN_F_TANG | SCREEN_F_SHOWN)
        h.esc_after = 150
        h.start()
        self.assert_no_window(h)
        self.assertIn(TANG_READ, h.audio.commands)
        self.assertEqual(bytes(h.audio.received), content, 'звук — весь')
        blocks = h.audio.ui_blocks
        self.assertTrue(all(b['flags'] == UI_F_OPEN for b in blocks[:-1]), 'экрану: окно открыто')
        self.assertEqual(blocks[-1]['flags'], UI_F_OPEN | UI_F_LEAVING)
        self.assertEqual(h.machine.a, 0)

    def test_module_on_the_screen_has_no_window(self) -> None:
        # Модуль целиком в ESP и до конца песни: ни окна, ни AUDIO_INFO с названием.
        content = random.Random(41).randbytes(50_000)
        h = self.harness(content, name=b"TUNE.XM", tang_version=self.TANG,
                         screen=SCREEN_F_TANG | SCREEN_F_SHOWN)
        h.start(index=2, count=5)
        self.assert_no_window(h)
        self.assertEqual(bytes(h.audio.received), content)
        self.assertEqual(h.machine.a, EXIT_NEXT, 'в конце песни — следующий файл')

    def test_screen_that_never_appears_gives_the_window(self) -> None:
        # Tang есть, а экран не показался (PLAYER не нашёл памяти): через 5 с (250 кадров) — окно,
        # и метки для него спрошены.
        h = self.harness(bytes(2 << 20), tang_version=self.TANG, screen=SCREEN_F_TANG)
        h.audio.capacity = 8192
        h.esc_after = 1500
        h.start()
        self.assertIn(FN_PRWOW, h.calls, 'окно открыто')
        first_window = h.calls.index(FN_PRWOW)
        self.assertGreaterEqual(h.interrupts, 250)
        self.assertIn(AUDIO_TAGS, h.audio.commands, 'метки — для окна')
        self.assertTrue(h.text(TITLE_ROW), 'окно нарисовано')
        self.assertGreater(first_window, 0)

    def test_screen_appearing_later_removes_the_window(self) -> None:
        # Окно было (Tang вернулся позже): экран показан — окно закрыто, спрайт обложки снят.
        h = self.harness(bytes(2 << 20), tang_version=self.TANG, screen=SCREEN_F_TANG)
        h.audio.capacity = 8192
        after = 0
        audio = h.audio

        def app(command: int, payload: bytes) -> bytes:
            nonlocal after
            if command == AUDIO_STATUS and FN_PRWOW in h.calls:
                after += 1                  # окно уже открыто: ещё немного — и экран показан
                if after == 50:
                    audio.screen = SCREEN_F_TANG | SCREEN_F_SHOWN
            return audio(command, payload)
        h.app = app
        h.esc_after = 2000
        h.start()
        self.assertIn(FN_PRWOW, h.calls, 'сначала — окно')
        opened = h.calls.index(FN_PRWOW)
        self.assertIn(base.FN_RRESB, h.calls[opened:], 'экран показан — окно закрыто')

    def test_module_delivery_on_the_screen_has_no_window(self) -> None:
        # Плагин 1.22: новый PLAYER.ELF уходит в кэш ESP — с Tang окна нет и на время передачи
        # (1.21 открывал его: «обычный интерфейс вначале»).
        h = self.harness(tang_version=self.TANG, screen=SCREEN_F_TANG | SCREEN_F_SHOWN)
        player = b"\x7fELF" + random.Random(5).randbytes(3000)
        h.root = {b"WC": {b"ESP": {b"PLAYER.ELF": (player, 0x9C2B, 0x5D43),
                                   b"MP3.ELF": (b"\x7fELF" + bytes(600), 0x9C2B, 0x5D43)}},
                  b"ZIFI": {}}
        h.esc_after = 150
        h.start()
        self.assertEqual(h.audio.stored.get(b"PLAYER.ELF"), player, 'PLAYER.ELF передан')
        self.assert_no_window(h)

    def test_no_tang_window_from_the_start(self) -> None:
        # Tang нет (TANG_READ — отказ): окно сразу, до AUDIO_OPEN, как в 1.20.
        h = self.harness()
        h.esc_after = 60
        h.start()
        self.assertIn(FN_PRWOW, h.calls)
        self.assertEqual(h.audio.commands.count(TANG_READ), 1)
        self.assertNotEqual(h.at(STATUS_ROW, TEXT_MP3), "unsupported:45", 'отказ — не ошибка')

    def test_old_player_with_tang_opens_the_window(self) -> None:
        # Прежний PLAYER (экрана своего нет, ответ AUDIO_OPEN — 5 байт): окно — после ответа.
        h = self.harness(tang_version=self.TANG, screen=None)
        h.esc_after = 60
        h.start()
        self.assertIn(FN_PRWOW, h.calls)
        self.assertIn(AUDIO_VIEW, h.audio.commands, 'окну — спектр')


class TrackNumberTest(unittest.TestCase):
    """«Track N of M» — среди файлов плеера: WC даёт номер объекта панели и число
    объектов вместе с «..», каталогами и чужими файлами (track.asm)."""

    @classmethod
    def setUpClass(cls) -> None:
        need(WMF, SYM)

    def folder(self, sort: int, index: int) -> Mp3Harness:
        content = bytes(5000)
        h = Mp3Harness(content, name=b"B.MP3")
        h.parent = True
        h.panel_dirs = [b"SUB"]
        h.files = {b"COVER.JPG": (100, False, b"j"), b"A.MP3": (5000, False, content),
                   b"B.MP3": (5000, False), b"C.MWK": (50, False, b"k"),
                   b"README.TXT": (10, False, b"t")}
        h.memory[0x7E00 + 0x23] = sort       # IX — структура панели, +#23 — сортировка
        h.start(index=index, count=7)
        self.assertEqual(bytes(h.audio.received), content, "обход каталога звуку не мешает")
        return h

    def test_by_extension_counts_only_player_files(self) -> None:
        # Панель по расширению: .., SUB, COVER.JPG, A.MP3, B.MP3, C.MWK, README.TXT.
        h = self.folder(2, index=5)
        self.assertEqual(h.at(TRACK_ROW, TEXT_MP3), "Track 2 of 2")
        self.assertEqual(h.machine.a, EXIT_NEXT, "дальше — по объектам панели, как прежде")

    def test_other_sorting_drops_parent_and_folders(self) -> None:
        # По имени: .., SUB, A.MP3, B.MP3, C.MWK, COVER.JPG, README.TXT — где чужие
        # файлы, плагин не знает, вычитает «..» и каталоги.
        h = self.folder(1, index=4)
        self.assertEqual(h.at(TRACK_ROW, TEXT_MP3), "Track 2 of 5")

    def test_unsorted_drops_only_the_parent(self) -> None:
        # Без сортировки каталоги вперемешку с файлами: известен только «..» — он первый.
        h = self.folder(0, index=5)
        self.assertEqual(h.at(TRACK_ROW, TEXT_MP3), "Track 4 of 6")


class OwnFileTest(unittest.TestCase):
    """Свой файл: по имени из BC, затем заново по 8.3 и с начала — потоками WC."""

    @classmethod
    def setUpClass(cls) -> None:
        need(WMF, SYM)

    def test_found_by_the_long_name_and_reopened(self) -> None:
        name = b"01. Fort Knox.mp3"
        h = Mp3Harness(bytes(30000), name=name)
        h.start()
        self.assertEqual(h.short, b"01FORT~1.MP3")
        # Первый клон потока — обход каталога для «Track N of M» (track.asm).
        self.assertEqual(h.fs_calls[:4], [FN_STREAM, FN_STREAM, FN_FENTRY, FN_TENTRY],
                         "своя запись — после поиска по имени из BC")
        self.assertEqual(h.found_names[0], name, "искали по длинному имени")
        reopen = h.fs_calls.index(FN_GFILE)
        self.assertEqual(h.fs_calls[reopen - 2:reopen + 1], [FN_STREAM, FN_FENTRY, FN_GFILE])
        self.assertIn(b"01FORT~1.MP3", h.found_names, "заново — по имени 8.3 из записи")
        self.assertEqual(h.hint(), HINT_PLAY)
        self.assertEqual(h.audio.opens[0][5], 3, "перемотка есть, свой файл найден")
        self.assertEqual(bytes(h.audio.received), bytes(30000))
        self.assertEqual(h.stream_reads(None), [], "поток панели не читался: звук — в клоне")

    def test_lost_file_falls_back_to_the_plain_stream(self) -> None:
        # Нет файла (TENTRY отдаёт чужую запись) или длина не та.
        for files in ({}, {b"SONG.MP3": (12345, False)}):
            with self.subTest(files=files):
                h = Mp3Harness(Mp3PluginTest.id3(60000) + bytes(100000), name=b"Song.mp3")
                h.files = files
                h.keys = {FN_RIGHT: {40}}
                h.start()
                self.assertIn(FN_GIPAGPL, h.fs_calls, "поток — снова на начало файла плагина")
                self.assertEqual(h.hint(), HINT_LOST)
                self.assertEqual(h.audio.opens[0][5] & 3, 0, "ни перемотки, ни своего файла")
                self.assertNotIn(AUDIO_COVER, h.audio.commands)
                self.assertEqual(h.audio.flushes, [], "без своего файла не перематывается")
                self.assertEqual(bytes(h.audio.received), bytes(100000))


class Mp3SeekTest(unittest.TestCase):
    """Перемотка MP3: AUDIO_FLUSH, свой файл заново, LOADNONE и хвост сектора."""

    @classmethod
    def setUpClass(cls) -> None:
        need(WMF, SYM)

    def harness(self, size: int = 600_000, **esp) -> Mp3Harness:
        content = bytes((index * 7 + (index >> 9)) & 0xFF for index in range(size))
        return Mp3Harness(content, esp=AudioEsp(capacity=16384, played_per_status=1024, **esp))

    def test_right_jumps_ahead_and_reads_from_there(self) -> None:
        h = self.harness()
        h.keys = {FN_RIGHT: {40}}
        h.esc_after = 120
        h.start()
        [ms] = h.audio.flushes
        seconds = ms // 1000
        self.assertEqual(ms, seconds * 1000)
        self.assertGreaterEqual(seconds, 10, "вперёд на 10 с от сыгранного")
        target = seconds * BYTES_PER_SECOND
        self.assertEqual(h.audio.flush_offsets, [target], "место — и в AUDIO_FLUSH: по нему WAV встаёт на отсчёт")
        before, _, after = bytes(h.audio.received).partition(b"<FLUSH>")
        self.assertEqual(before, h.content[:len(before)])
        self.assertTrue(after, "после перемотки звук идёт дальше")
        self.assertEqual(after, h.content[target:target + len(after)],
                         "с нового места, по порядку")
        skipped = sum(count for stream, position, count in h.skips if stream == 0)
        self.assertEqual(skipped, target // 512, "до сектора места — без чтения")
        reads = [(stream, position, count) for stream, _, position, count in h.reads]
        if target % 512:
            self.assertIn((0, target // 512 * 512, 1), reads,
                          "сектор места прочитан: его хвост — первым блоком")

    def test_left_goes_back_but_not_before_the_start(self) -> None:
        h = self.harness()
        h.keys = {FN_LEFT: {30}}
        h.esc_after = 90
        h.start()
        [ms] = h.audio.flushes
        self.assertEqual(ms, 0, "раньше начала — на начало")
        after = bytes(h.audio.received).partition(b"<FLUSH>")[2]
        self.assertEqual(after, h.content[:len(after)])

    def test_seek_while_draining_sends_the_file_again(self) -> None:
        # Файл отдан целиком, поток закрыт для доигрывания — и тут «влево»:
        # PLAYER 1.7 открывает поток снова, плагин возвращается к передаче с
        # нового места и в конце снова закрывает поток для доигрывания.
        for reopens in (True, False):
            with self.subTest(reopens=reopens):
                size = 100_000
                content = bytes((index * 7 + (index >> 9)) & 0xFF for index in range(size))
                h = Mp3Harness(content, esp=AudioEsp(capacity=262_144, played_per_status=1024))
                h.audio.flush_while_draining = reopens
                address = h.symbol("Mp3_WaitEnd")
                h.machine.set_breakpoint(address)
                pressed = []

                def press() -> None:
                    if not pressed:
                        pressed.append(h.interrupts)
                        h.keys = {FN_LEFT: set(range(h.interrupts + 5, h.interrupts + 9))}
                h.hooks[address] = press
                h.start(index=2, count=5)
                self.assertEqual(h.audio.flushes, [0], "перемотка — во время доигрывания")
                before, _, after = bytes(h.audio.received).partition(b"<FLUSH>")
                self.assertEqual(before, content)
                self.assertEqual(h.machine.a, EXIT_NEXT, "доиграл — к следующему файлу")
                if reopens:
                    self.assertEqual(after, content, "файл заново с начала, целиком")
                    self.assertEqual(h.audio.closes, [1, 1], "и снова доигрывание")
                else:
                    self.assertEqual(after, b"", "старый PLAYER не перемотал: всё как было")
                    self.assertEqual(h.audio.closes, [1])

    def test_seek_past_the_end_goes_to_the_next_file(self) -> None:
        h = self.harness(size=60_000)       # меньше 4 с при 128 кбит/с
        h.keys = {FN_RIGHT: set(range(20, 30))}
        h.start(index=2, count=5)
        self.assertEqual(h.audio.flushes, [])
        self.assertEqual(h.machine.a, EXIT_NEXT)
        self.assertEqual(h.audio.closes, [0])


class CoverTest(unittest.TestCase):
    """Обложка: фоновая досылка метки потоком 1, миниатюра спрайтом, отказы."""

    @classmethod
    def setUpClass(cls) -> None:
        need(WMF, SYM)

    TAG = 60000
    APIC = (3000, 50000)

    def harness(self, audio_size: int = 300_000, **esp) -> Mp3Harness:
        tag = Mp3PluginTest.id3(self.TAG)
        audio = bytes((index * 13 + (index >> 10)) & 0xFF for index in range(audio_size))
        model = AudioEsp(capacity=16384, played_per_status=1024)
        model.cover_apic = self.APIC
        for name, value in esp.items():
            setattr(model, name, value)
        h = Mp3Harness(tag + audio, esp=model)
        h.tag_length = len(tag)
        h.audio_bytes = audio
        return h

    def sprite_rows(self, h: Mp3Harness) -> bytes:
        page = h.page_bytes(VIDEO_BASE + 0)         # COVER_VPAGE
        return b"".join(page[row * 256:row * 256 + 32] for row in range(64))

    def assert_audio_in_order(self, h: Mp3Harness) -> None:
        """Звук — потоком 0 подряд: чтение обложки его позицию не трогает."""
        self.assertEqual(bytes(h.audio.received), h.audio_bytes, "звук дошёл целиком и по порядку")
        ops = h.stream_ops(0)
        self.assertEqual(ops[0][0], 0)
        self.assertTrue(all(position + 512 * count == next_position
                            for (position, count), (next_position, _) in zip(ops, ops[1:])),
                        "поток 0 читается и пропускается подряд, без возвратов")

    def test_cover_goes_in_the_background_and_is_shown(self) -> None:
        h = self.harness()
        h.start(index=1, count=1)
        esp = h.audio
        self.assert_audio_in_order(h)
        self.assertEqual(esp.cover_state, 1)
        self.assertEqual(bytes(esp.cover_data), h.content[self.APIC[0]:self.APIC[1]],
                         "тело APIC дошло целиком")
        offsets = [offset for offset, _ in esp.cover_chunks]
        self.assertEqual(offsets[0], 0, "метка — с начала: ESP разбирает её заголовок")
        self.assertEqual(offsets[1], self.APIC[0], "дальше — сразу то место, что просит ESP")
        self.assertTrue(all(0 < size <= 512 for _, size in esp.cover_chunks),
                        "кусок — до конца сектора")
        self.assertEqual(esp.cover_chunks[1][1], 512 - self.APIC[0] % 512)
        body = esp.cover_chunks[1:]
        self.assertTrue(all(offset + size == next_offset for (offset, size), (next_offset, _)
                            in zip(body, body[1:])), "куски подряд, ни один не ушёл дважды")
        self.assertLess(sum(size for _, size in body), self.APIC[1] - self.APIC[0] + 512)
        self.assertIn((1, 512, self.APIC[0] // 512 - 1), h.skips,
                      "поток 1 — вперёд к месту без чтения")
        # Миниатюра — в видеостранице плагина 0, строки по 256 байтов.
        self.assertEqual(self.sprite_rows(h), esp.thumb)
        self.assertEqual(set(h.vpages), {0})
        self.assertEqual(h.sgpages, [0])
        # Окно 66×21 в середине экрана 80×30: X=7, Y=4; обложка — со столбца
        # 2 и строки 2 окна (знак — 4 точки спрайта в ширину, 8 в высоту).
        [window] = h.prwow_windows
        self.assertEqual(window[4], 66)
        x, y = (7 + 2) * 4, (4 + 2) * 8
        [descriptors] = h.sfile
        self.assertEqual(descriptors,
                         bytes([y & 0xFF, y >> 8 | 0x6E, x & 0xFF, x >> 8 | 0x0E, 0, 0,
                                0, 0x40, 0, 0, 0, 0, 0, 0x40, 0, 0, 0, 0]))
        writes = [write for write in h.ts_writes if write[0] != TS_PAGE3]
        self.assertEqual(writes, [(TS_FMADDR, 0x1B), (TS_FMADDR, 0x00), (TS_CONFIG, 0x80),
                                  (TS_CONFIG, 0x00)],
                         "окно FMADDR на #B000, затем спрайты; при выходе — выключены")
        self.assertIn((TS_PAGE3, h.memory[0x6003]), h.ts_writes, "окно #C000 возвращено")
        self.assertEqual(h.machine.a, 0)

    PICTURE = b"\xFF\xD8\xFF" + bytes((index * 31) & 0xFF for index in range(9000))

    def assert_picture(self, h: Mp3Harness) -> None:
        esp = h.audio
        self.assertEqual(bytes(esp.picture), self.PICTURE, "картинка дошла целиком")
        chunks = esp.picture_chunks
        self.assertEqual(chunks[0], (0, 512))
        self.assertTrue(all(offset + size == next_offset for (offset, size), (next_offset, _)
                            in zip(chunks, chunks[1:])), "куски подряд, ни один дважды")
        self.assertEqual(self.sprite_rows(h), esp.thumb, "миниатюра на экране")
        self.assert_audio_in_order(h)

    def test_folder_picture_when_the_tag_has_none(self) -> None:
        h = self.harness(cover_apic=None)
        h.files[b"folder.jpg"] = (len(self.PICTURE), False, self.PICTURE)
        h.start()
        self.assertEqual(h.audio.cover_chunks[0][0], 0, "сначала разобрана метка")
        self.assert_picture(h)

    def test_folder_picture_without_a_tag(self) -> None:
        audio = bytes((index * 13 + (index >> 10)) & 0xFF for index in range(200_000))
        model = AudioEsp(capacity=16384, played_per_status=1024)
        h = Mp3Harness(audio, esp=model)
        h.audio_bytes = audio
        h.files[b"COVER.JPG"] = (len(self.PICTURE), False, self.PICTURE)
        h.start()
        self.assertNotIn(AUDIO_COVER, model.commands, "метки нет — разбирать нечего")
        self.assert_picture(h)

    def test_no_folder_picture_or_firmware_without_it(self) -> None:
        h = self.harness(cover_apic=None)
        h.start()
        self.assertNotIn(AUDIO_PICTURE, h.audio.commands)
        self.assertEqual(bytes(h.audio.received), h.audio_bytes)
        old = self.harness(cover_apic=None, picture_known=False)
        old.files[b"front.jpg"] = (len(self.PICTURE), False, self.PICTURE)
        old.start()
        self.assertEqual(old.audio.commands.count(AUDIO_PICTURE), 1, "после отказа — не слать")
        self.assertNotIn(AUDIO_THUMB, old.audio.commands)
        self.assertEqual(bytes(old.audio.received), old.audio_bytes)
        self.assertEqual(old.hint(), HINT_PLAY)

    def test_short_tag_is_parsed_too_and_lost_file_has_no_cover(self) -> None:
        # Короткую метку ESP получила со звуком, но разобрать её ради обложки и
        # названия надо всё равно: досылка идёт и для неё.
        model = AudioEsp(capacity=16384, played_per_status=1024)
        model.cover_apic = None
        model.title = b"Artist - Song"
        short = Mp3Harness(Mp3PluginTest.id3(300) + bytes(40000), esp=model)
        short.start()
        self.assertIn(AUDIO_COVER, model.commands, "короткая метка тоже разбирается")
        self.assertEqual(model.cover_chunks[0][0], 0, "с начала файла")
        self.assertIn(AUDIO_TAGS, model.commands, "метка пройдена — спросить метки")
        self.assertIn(AUDIO_INFO, model.commands, "прошивка без AUDIO_TAGS — название")
        self.assertEqual(short.at(TITLE_ROW, TEXT_MP3), "Artist - Song")
        self.assertNotIn(AUDIO_THUMB, model.commands, "картинки нет — миниатюру не ждать")
        # Свой файл не нашёлся заново — обложки нет: читать метку нечем.
        h = self.harness()
        h.files = {}
        h.start()
        self.assertNotIn(AUDIO_COVER, h.audio.commands)
        self.assertEqual(bytes(h.audio.received), h.audio_bytes)

    def test_firmware_without_cover(self) -> None:
        h = self.harness(cover_known=False)
        h.start()
        self.assertEqual(h.audio.commands.count(AUDIO_COVER), 1, "после отказа — не спрашивать")
        self.assertEqual(bytes(h.audio.received), h.audio_bytes)
        self.assertEqual(h.at(STATUS_ROW, TEXT_MP3), "", "доигрывание в окне не пишется")
        self.assertNotIn((TS_CONFIG, 0x80), h.ts_writes)

    def test_no_picture_or_not_decoded(self) -> None:
        h = self.harness()
        h.audio.cover_apic = None
        h.start()
        self.assertEqual(h.audio.commands.count(AUDIO_COVER), 1)
        self.assertNotIn(AUDIO_THUMB, h.audio.commands)
        h = self.harness(thumb_fail=True)
        h.start()
        self.assertEqual(h.audio.commands.count(AUDIO_THUMB), 1)
        self.assertNotIn((TS_CONFIG, 0x80), h.ts_writes)
        self.assertEqual(bytes(h.audio.received), h.audio_bytes)

    def test_escape_switches_the_sprite_off(self) -> None:
        h = self.harness(audio_size=2_000_000)
        h.esc_after = 400
        h.start()
        self.assertIn((TS_CONFIG, 0x80), h.ts_writes, "спрайт успел появиться")
        self.assertEqual(h.ts_writes[-1], (TS_CONFIG, 0x00))
        self.assertLess(len(h.audio.received), 2_000_000)

    def test_module_window_has_no_cover_place(self) -> None:
        h = Mp3Harness(bytes(5000), name=b"A.MOD")
        h.start()
        [window] = h.prwow_windows
        self.assertEqual(window[4], 66)
        self.assertEqual(h.at(TITLE_ROW, TEXT_MODULE), "Test Module Title", "текст — с края")
        self.assertNotIn(AUDIO_COVER, h.audio.commands)

    def test_dialog_hides_the_cover_sprite(self) -> None:
        h = self.harness(audio_size=2_000_000)
        h.esc_after = 400
        shown = []
        address = h.symbol("Ui_Keys")
        h.machine.set_breakpoint(address)

        def press() -> None:
            # H — как только спрайт включён, и ещё раз через 10 кадров.
            if (TS_CONFIG, 0x80) in h.ts_writes and not shown:
                shown.append(h.interrupts)
                h.key_chars = {"h": {h.interrupts, h.interrupts + 10}}
        h.hooks[address] = press
        h.start()
        self.assertTrue(shown, "спрайт успел появиться")
        config = [value for port, value in h.ts_writes if port == TS_CONFIG]
        self.assertEqual(config, [0x80, 0x00, 0x80, 0x00],
                         "справка — без спрайта, закрыта — снова он; при выходе — выключен")


class ModulePlayTest(unittest.TestCase):
    """Модуль и MIDI: файл целиком в ESP, разбор, название, позиции, конец песни."""

    @classmethod
    def setUpClass(cls) -> None:
        need(WMF, SYM)

    def test_kind_by_extension(self) -> None:
        content = random.Random(40).randbytes(3000)
        cases = ((b"SONG.MOD", OPEN_MODULE, b"MOD"), (b"song.xm", OPEN_MODULE, b"XM "),
                 (b"x.S3m", OPEN_MODULE, b"S3M"), (b"Tune.it", OPEN_MODULE, b"IT "),
                 (b"song.mid", OPEN_MODULE | OPEN_MIDI, b"MID"),
                 (b"A.RMI", OPEN_MODULE | OPEN_MIDI, b"RMI"), (b"x.Kar", OPEN_MODULE | OPEN_MIDI, b"KAR"),
                 (b"long song.midi", OPEN_MODULE | OPEN_MIDI, b"MID"),
                 (b"tune.vgm", OPEN_MODULE, b"VGM"), (b"T.VGZ", OPEN_MODULE, b"VGZ"),
                 (b"x.dro", OPEN_MODULE, b"DRO"), (b"KEEN.IMF", OPEN_MODULE, b"IMF"),
                 (b"wolf.Wlf", OPEN_MODULE, b"WLF"), (b"Belair.mwm", OPEN_MODULE, b"MWM"),
                 (b"XAK_1.Mfm", OPEN_MODULE, b"MFM"), (b"tralala.mo", OPEN_MODULE, b"MO "),
                 (b"a.mp3", 0, b"MP3"), (b"NOEXT", 0, b"   "), (b"a.b.Mp3", 0, b"MP3"),
                 (b"Take 1.wav", 0, b"WAV"))
        for name, kind, ext in cases:
            with self.subTest(name=name):
                h = Mp3Harness(content, name=name)
                if kind & OPEN_MIDI:
                    h.midi_card()
                elif ext in (b"VGM", b"VGZ", b"DRO", b"IMF", b"WLF", b"MWM", b"MFM"):
                    h.opl_card()                # OPL встроенный плеер не играет
                h.esc_after = 400               # если что-то пойдёт не так — не висеть
                h.start()
                [opened] = h.audio.opens
                self.assertEqual(opened[:5], bytes([kind]) + len(content).to_bytes(4, "little"))
                if kind:
                    self.assertEqual(opened[5:], ext, "расширение — ESP (IMF без подписи)")
                else:
                    self.assertEqual(opened[6:], ext, "поток: расширение — за флагами")

    def test_module_is_sent_whole_then_plays_to_the_end(self) -> None:
        content = random.Random(41).randbytes(50_000)
        h = Mp3Harness(content, name=b"TUNE.XM")
        h.start(index=2, count=5)
        esp = h.audio
        self.assertEqual(bytes(esp.received), content)
        self.assertTrue(all(0 < block <= 1024 for block in esp.blocks))
        self.assertEqual(h.own_sectors(), -(-len(content) // 512))
        self.assertIn(AUDIO_INFO, esp.commands)
        self.assertNotIn(1, esp.closes, "модуль не «доигрывают»: он играет до конца песни")
        self.assertEqual((esp.state, esp.finished), (STATE_IDLE, True))
        self.assertEqual(h.machine.a, EXIT_NEXT, "в конце песни — следующий файл")
        self.assertEqual(h.at(TITLE_ROW, TEXT_MODULE), "Test Module Title")
        self.assertEqual(h.at(SECOND_ROW, TEXT_MODULE), "TUNE.XM")
        self.assertEqual(h.at(FORMAT_ROW, TEXT_MODULE), "XM  8 channels  6 positions")
        self.assertEqual(h.at(TIME_ROW, TEXT_MODULE)[:5], "00:18", "время из записи состояния")
        self.assertEqual(h.at(TIME_ROW, TEXT_MODULE)[6:], PLAYED * ui("UI_BAR_MODULE") + " --:--",
                         "полоса — по позициям; загрузки больше нет")
        self.assertEqual(h.hint(), HINT_MODULE)

    def test_module_without_title_shows_the_name_once(self) -> None:
        esp = AudioEsp()
        esp.title = b""
        h = Mp3Harness(bytes(20_000), name=b"tune.xm", esp=esp)
        h.esc_after = 60
        h.start()
        self.assertEqual(h.at(TITLE_ROW, TEXT_MODULE), "tune.xm")
        self.assertEqual(h.text(SECOND_ROW), "", "имя уже в первой строке")

    def test_window_shows_the_position(self) -> None:
        h = Mp3Harness(bytes(20_000), name=b"A.MOD", esp=AudioEsp())
        h.audio.module_format = FORMAT_MOD
        h.audio.length = 40
        h.esc_after = 60
        h.start()
        track = h.at(TRACK_ROW, TEXT_MODULE)
        self.assertTrue(track.startswith("Position "), track)
        self.assertIn(" of 40   Row ", track)
        self.assertEqual(h.at(FORMAT_ROW, TEXT_MODULE), "MOD  8 channels  40 positions")
        self.assertEqual(h.audio.closes, [0], "Esc — сразу")

    def test_samples_and_effects(self) -> None:
        esp = AudioEsp()
        esp.tags_known = True
        esp.tags = b"Tracker Song\0\0\0\0\0" + (31).to_bytes(2, "little") + bytes([12])
        esp.module_format = FORMAT_IT
        h = Mp3Harness(bytes(20_000), name=b"tune.it", esp=esp)
        h.esc_after = 60
        h.start()
        self.assertEqual(h.at(TITLE_ROW, TEXT_MODULE), "Tracker Song")
        self.assertEqual(h.at(SECOND_ROW, TEXT_MODULE), "tune.it")
        self.assertEqual(h.at(THIRD_ROW, TEXT_MODULE), "Samples: 31  Effects: 12")
        self.assertEqual(h.at(FORMAT_ROW, TEXT_MODULE), "IT  8 channels  6 positions")
        self.assertEqual(h.screen.get(FORMAT_ROW - 1, b"").decode("cp866").strip(),
                         "IT  8 channels  6 positions",
                         "у модуля альбома нет: строки от формата — на одну выше")

    def test_space_pauses_the_module(self) -> None:
        # Короткое нажатие пробела, пока модуль играет: у WC событие нажатия
        # одно, и KBSCN (клавиши H и I) не должен забрать его раньше пробела.
        h = Mp3Harness(bytes(10_000), name=b"BELAIR.MWM", esp=AudioEsp())
        h.opl_card()
        h.esc_after = 120
        address = h.symbol("Mp3_WaitEnd")
        h.machine.set_breakpoint(address)

        def press() -> None:
            h.keys = {FN_SPACE: set(range(h.interrupts + 5, h.interrupts + 9))}
        h.hooks[address] = press
        h.start()
        self.assertEqual(h.audio.pauses, [1])
        self.assertEqual(h.at(STATUS_ROW, TEXT_MODULE), "Paused")

    def test_seek_keys_move_by_positions(self) -> None:
        h = Mp3Harness(bytes(10_000), name=b"A.S3M")
        h.audio.length = 30
        h.keys = {FN_RIGHT: {20}, FN_LEFT: {40}}
        h.esc_after = 60
        h.start()
        self.assertEqual(h.audio.seeks, [1, -1])

    def test_big_file_goes_to_the_esp(self) -> None:
        # Предела в плагине нет: IT ESP берёт потоком любой длины, прочее
        # больше 6 МиБ не откроет сама (test_esp_without_room). Файл в стенде
        # пустой: ESP его ждёт, Esc прерывает.
        size = 9 * 1024 * 1024
        h = Mp3Harness(b"", name=b"GOBLIN.IT")
        h.esc_after = 5
        h.start(size=size)
        self.assertEqual(h.audio.opens, [bytes([OPEN_MODULE]) + size.to_bytes(4, "little") + b"IT "])

    def test_refused_midway_is_not_sent_further(self) -> None:
        # PLAYER 1.2: IT потоком не поместился — ответ 2 посреди приёма; плагин
        # перестаёт слать, ждёт пропуска («играет», потом «доиграл») и уходит дальше.
        content = random.Random(43).randbytes(60_000)
        h = Mp3Harness(content, name=b"BIG.IT")
        h.audio.module_refuse_at = 8_000
        h.esc_after = 400
        h.start(index=1, count=3)
        self.assertLess(len(h.audio.received), 12_000, "после отказа файл дальше не шлётся")
        self.assertEqual(h.machine.a, EXIT_NEXT, "пропуск — к следующему файлу панели")

    def test_esp_without_room(self) -> None:
        h = Mp3Harness(bytes(5000), name=b"A.MOD")
        h.audio.module_take = False
        h.esc_after = 5
        h.start()
        self.assertEqual(h.at(STATUS_ROW, TEXT_MODULE), "ERROR: no room on the ESP for the file")
        self.assertEqual(h.audio.received, b"")

    def test_not_a_module(self) -> None:
        h = Mp3Harness(bytes(5000), name=b"A.MOD")
        h.audio.module_ok = False
        h.esc_after = 12                    # Esc опрашивается и на каждом блоке
        h.start()
        self.assertEqual(bytes(h.audio.received), bytes(5000))
        self.assertEqual(h.at(STATUS_ROW, TEXT_MODULE), "ERROR: ESP could not load the file")

    def test_escape_while_sending(self) -> None:
        h = Mp3Harness(bytes(200_000), name=b"A.XM")
        h.esc_after = 3
        h.start()
        self.assertLess(len(h.audio.received), 200_000)
        self.assertEqual(h.audio.closes, [0])

    def test_previous_file_is_not_shown_while_loading(self) -> None:
        # Страница плагина живёт между запусками: запись состояния прошлого
        # файла (XM, 11 позиций) не должна попасть в окно нового.
        h = Mp3Harness(bytes(100_000), name=b"next.xm")
        status = h.symbol("Mp3Status")
        h.memory[status + 1] = 2                     # формат распознан
        h.memory[status + 24] = FORMAT_XM
        h.memory[status + 28] = 11
        h.memory[status + 30] = 8
        seen = []
        address = h.symbol("Ui_ShowLoad")
        h.machine.set_breakpoint(address)
        h.hooks[address] = lambda: seen.append((h.at(FORMAT_ROW, TEXT_MODULE),
                                                h.at(TRACK_ROW, TEXT_MODULE)))
        h.esc_after = 30
        h.start()
        self.assertTrue(seen)
        self.assertEqual(seen[0], ("Module", ""))

    def test_loading_bar_while_the_module_goes(self) -> None:
        # Пока модуль идёт в ESP — на месте времени полоса загрузки.
        h = Mp3Harness(bytes(200_000), name=b"A.XM")
        h.esc_after = 60
        h.start()
        width = ui("UI_BAR_MODULE") - 1     # «Loading » на два знака длиннее времени
        sent = len(h.audio.received) >> 10
        total = 200_000 >> 10
        self.assertTrue(0 < sent < total, sent)
        line = h.at(TIME_ROW, TEXT_MODULE)
        cells = width * sent // total
        self.assertEqual(line, "Loading " + PLAYED * cells + LEFT * (width - cells) +
                         " %d%%" % (100 * sent // total), "хвоста строки времени нет")

    def test_channel_meters(self) -> None:
        esp = AudioEsp()
        esp.view_known, esp.view_kind = True, 2
        esp.view_values = [12, 0, 6, 1, 2, 3, 11, 12]
        h = Mp3Harness(bytes(20_000), name=b"A.MOD", esp=esp)
        h.esc_after = 40
        h.start()
        bottom = h.at(BARS_ROW + 5, 2)
        top = h.at(BARS_ROW, 2)
        pad = " " * ((63 - (8 * 4 - 1)) // 2)  # группа из 8 каналов — посередине
        self.assertEqual(bottom.rstrip(), pad + "███     ███ ▄▄▄ ███ ███ ███ ███",
                         "каналы — по три знака через пробел")
        self.assertEqual(top.rstrip(), pad + "███                     ▄▄▄ ███")
        self.assertEqual(h.at(NUMBERS_ROW, 2).rstrip(),
                         pad + " 1   2   3   4   5   6   7   8", "номера — под столбиками")

    def test_midi_window(self) -> None:
        levels = [(index * 3) % 13 for index in range(32)]
        esp = AudioEsp()
        esp.module_format = FORMAT_MIDI
        esp.tags_known = True
        esp.tags = b"MIDI Song\0\0\0\0\0" + (14).to_bytes(2, "little") + bytes([9])
        esp.view_known, esp.view_kind, esp.view_values, esp.view_duration = True, 1, levels, 200
        esp.length = 40
        h = Mp3Harness(b"MThd" + bytes(20_000), name=b"Song.mid", esp=esp)
        h.midi_card()
        h.keys = {FN_RIGHT: {40}}
        h.esc_after = 80
        h.start(index=2, count=5)
        self.assertEqual(esp.opens, [bytes([OPEN_MODULE | OPEN_MIDI]) +
                                     (20_004).to_bytes(4, "little") + b"MID"])
        self.assertEqual(h.at(TITLE_ROW, TEXT_MODULE), "MIDI Song", "обложки нет — с края")
        self.assertEqual(h.at(SECOND_ROW, TEXT_MODULE), "Song.mid")
        self.assertEqual(h.at(THIRD_ROW, TEXT_MODULE), "Instruments: 14  Tracks: 9")
        self.assertEqual(h.at(FORMAT_ROW, TEXT_MODULE), "MIDI  8 channels", "позиций нет")
        self.assertEqual(h.at(TRACK_ROW, TEXT_MODULE), "Track 2 of 5")
        time = h.at(TIME_ROW, TEXT_MODULE)
        self.assertTrue(time.endswith(" 03:20"), time)
        self.assertTrue(h.at(BARS_ROW + 5, 2)[0:63:2].strip(), "столбики спектра")
        self.assertEqual(h.text(NUMBERS_ROW), "", "у спектра номеров нет")
        self.assertEqual(esp.seeks, [1], "перемотка — у ESP (шаг 10 с)")
        self.assertEqual(h.hint(), HINT_PLAY)
        self.assertNotIn(AUDIO_COVER, esp.commands)


class ModuleDeliveryTest(unittest.TestCase):
    """Модули ESP из /WC/ESP/: в кэш ESP, только если их там нет, и APP_START."""

    TIME, DATE = 0x9C2B, 0x5D43              # 19:33:22, 03.10.2026

    @classmethod
    def setUpClass(cls) -> None:
        need(WMF, SYM)

    @staticmethod
    def elf(size: int, seed: int) -> bytes:
        rng = random.Random(seed)
        return b"\x7fELF" + bytes(rng.randrange(256) for _ in range(size - 4))

    def card(self, h: Mp3Harness, **modules) -> dict:
        esp = {name.replace("_", ".").encode(): (content, self.TIME, self.DATE)
               for name, content in modules.items()}
        h.root = {b"WC": {b"ESP": esp}, b"ZIFI": {}}
        return esp

    def harness(self, content: bytes = bytes(20000), name: bytes = b"Song.mp3",
                esp: AudioEsp | None = None) -> Mp3Harness:
        return Mp3Harness(content, name=name, esp=esp)

    def test_modules_go_once_then_come_from_the_cache(self) -> None:
        player, mp3 = self.elf(3000, 1), self.elf(70000, 2)     # 70000: смещения за 64 КБ
        audio = AudioEsp()
        h = self.harness(esp=audio)
        self.card(h, PLAYER_ELF=player, MP3_ELF=mp3, TRACKER_ELF=self.elf(900, 3))
        h.start()
        stamp = self.TIME | self.DATE << 16
        self.assertEqual(audio.queries, [(b"PLAYER.ELF", 3000, stamp, 0),
                                         (b"MP3.ELF", 70000, stamp, 0)])
        self.assertEqual(audio.stored, {b"PLAYER.ELF": player, b"MP3.ELF": mp3})
        self.assertEqual(audio.puts[:6], [(offset, 512) for offset in range(0, 3000 - 440, 512)] +
                         [(2560, 440)], "по сектору, последний — остаток")
        self.assertIn((65536, 512), audio.puts)
        self.assertEqual(audio.started, [b"PLAYER.ELF"])
        self.assertLess(audio.commands.index(APP_START), audio.commands.index(AUDIO_OPEN))
        self.assertIn(b"SONG.MP3", h.found_names, "свой файл найден заново")
        self.assertEqual(bytes(audio.received), bytes(20000), "и прочитан с начала")
        self.assertEqual(h.hint(), HINT_PLAY)

        # Второй запуск: всё уже в кэше.
        audio.puts.clear()
        audio.queries.clear()
        audio.received.clear()
        again = self.harness(esp=audio)
        self.card(again, PLAYER_ELF=player, MP3_ELF=mp3)
        again.start()
        self.assertEqual(len(audio.queries), 2)
        self.assertEqual(audio.puts, [], "ничего не передаётся")
        self.assertEqual(audio.started, [b"PLAYER.ELF", b"PLAYER.ELF"])
        self.assertEqual(bytes(audio.received), bytes(20000))

    def test_changed_module_is_sent_again(self) -> None:
        player, mp3 = self.elf(1500, 4), self.elf(2000, 5)
        audio = AudioEsp()
        audio.cache = {b"PLAYER.ELF": (1500, self.TIME | self.DATE << 16),
                       b"MP3.ELF": (2000, self.TIME | (self.DATE - 1) << 16)}  # вчерашний
        h = self.harness(esp=audio)
        self.card(h, PLAYER_ELF=player, MP3_ELF=mp3)
        h.start()
        self.assertEqual(list(audio.stored), [b"MP3.ELF"])
        self.assertEqual(audio.started, [b"PLAYER.ELF"])

    def test_tracker_decoder_for_a_module(self) -> None:
        tracker = self.elf(1200, 6)
        audio = AudioEsp()
        h = self.harness(b"Extended Module: " + bytes(5000), name=b"Tune.xm", esp=audio)
        self.card(h, PLAYER_ELF=self.elf(800, 7), MP3_ELF=self.elf(900, 8), TRACKER_ELF=tracker)
        h.start()
        self.assertEqual([query[0] for query in audio.queries], [b"PLAYER.ELF", b"TRACKER.ELF"])
        self.assertEqual(audio.stored[b"TRACKER.ELF"], tracker)
        self.assertEqual(audio.started, [b"PLAYER.ELF"])
        self.assertEqual(bytes(audio.received), b"Extended Module: " + bytes(5000))

    def test_builtin_player_without_the_modules(self) -> None:
        cases = {
            "no /WC/ESP": {b"WC": {}},
            "no /WC": {},
            "no decoder": {b"WC": {b"ESP": {b"PLAYER.ELF": (self.elf(600, 9), 0, 0)}}},
            "no player": {b"WC": {b"ESP": {b"MP3.ELF": (self.elf(600, 9), 0, 0)}}},
            "esp is a file": {b"WC": {b"ESP": (b"", 0, 0)}},
        }
        for label, root in cases.items():
            with self.subTest(label):
                audio = AudioEsp()
                h = self.harness(esp=audio)
                h.root = root
                h.start()
                self.assertEqual(audio.started, [], "приложение не запускалось")
                self.assertEqual(bytes(audio.received), bytes(20000))
                self.assertIn(b"SONG.MP3", h.found_names)
                self.assertEqual(h.hint(), HINT_PLAY)

    def test_firmware_without_modules(self) -> None:
        audio = AudioEsp()
        audio.modules_known = False
        h = self.harness(esp=audio)
        self.card(h, PLAYER_ELF=self.elf(600, 10), MP3_ELF=self.elf(700, 11))
        h.start()
        self.assertEqual(audio.commands.count(MOD_QUERY), 1, "после отказа — дальше без модулей")
        self.assertNotIn(MOD_PUT, audio.commands)
        self.assertNotIn(APP_START, audio.commands)
        self.assertEqual(bytes(audio.received), bytes(20000), "отказ — не ошибка плеера")
        self.assertEqual(h.hint(), HINT_PLAY)

    def test_no_room_or_not_an_elf(self) -> None:
        for label in ("no room", "not an ELF", "app does not start"):
            with self.subTest(label):
                audio = AudioEsp()
                h = self.harness(esp=audio)
                player = self.elf(1000, 12)
                if label == "no room":
                    audio.cache_room = 999
                elif label == "not an ELF":
                    player = b"MZ" + player[2:]
                else:
                    audio.start_result = -2                     # ZM_NO_MEMORY
                self.card(h, PLAYER_ELF=player, MP3_ELF=self.elf(500, 13))
                h.start()
                if label == "app does not start":
                    self.assertEqual(audio.started, [b"PLAYER.ELF"])
                else:
                    self.assertNotIn(APP_START, audio.commands)
                    self.assertNotIn(b"PLAYER.ELF", audio.stored)
                self.assertEqual(bytes(audio.received), bytes(20000))
                self.assertEqual(h.hint(), HINT_PLAY)

    def test_window_shows_what_goes_and_how_far(self) -> None:
        audio = AudioEsp()
        h = self.harness(esp=audio)
        self.card(h, PLAYER_ELF=self.elf(40000, 14), MP3_ELF=self.elf(4000, 15))
        seen = []
        address = h.symbol("Ui_ShowLoad")
        h.machine.set_breakpoint(address)
        h.hooks[address] = lambda: seen.append((h.at(STATUS_ROW, TEXT_MP3),
                                                h.at(TIME_ROW, TEXT_MP3)[:8]))
        h.start()
        states = {state for state, _ in seen}
        self.assertEqual(states, {"Sending to the ESP: PLAYER.ELF", "Sending to the ESP: MP3.ELF"})
        self.assertIn("Loading ", {time for _, time in seen}, "полоса загрузки на месте времени")
        self.assertEqual(h.at(VOLUME_ROW, 2), volume_row(200), "потом — громкость, как обычно")

    # --- MIDI: модуль, банк звуков ресурсом ---------------------------------------

    BANK = bytes((index * 11) & 0xFF for index in range(30000))

    def midi(self, audio: AudioEsp, **card) -> Mp3Harness:
        h = self.harness(b"MThd" + bytes(3000), name=b"Song.mid", esp=audio)
        self.card(h, **card)
        return h

    def test_midi_bank_goes_as_a_resource_once(self) -> None:
        player, midi = self.elf(2000, 16), self.elf(3000, 17)
        audio = AudioEsp()
        h = self.midi(audio, PLAYER_ELF=player, MP3_ELF=self.elf(900, 18), MIDI_ELF=midi,
                      MIDI_SF2=self.BANK)
        h.start()
        stamp = self.TIME | self.DATE << 16
        self.assertEqual(audio.queries, [(b"PLAYER.ELF", 2000, stamp, 0),
                                         (b"MIDI.ELF", 3000, stamp, 0),
                                         (b"MIDI.SF2", 30000, stamp, 1)], "банк — ресурсом")
        self.assertEqual(audio.stored[b"MIDI.SF2"], self.BANK)
        self.assertEqual(audio.resources, {b"MIDI.SF2": 30000})
        self.assertEqual(audio.started, [b"PLAYER.ELF"])
        self.assertLess(audio.commands.index(APP_START), audio.commands.index(AUDIO_OPEN))
        self.assertEqual(audio.opens, [bytes([OPEN_MODULE | OPEN_MIDI]) +
                                       (3004).to_bytes(4, "little") + b"MID"])
        self.assertEqual(bytes(audio.received), b"MThd" + bytes(3000))
        # Следующий MIDI: банк уже у ESP.
        audio.puts.clear()
        audio.queries.clear()
        audio.received.clear()
        again = self.midi(audio, PLAYER_ELF=player, MIDI_ELF=midi, MIDI_SF2=self.BANK)
        again.start()
        self.assertEqual([query[0] for query in audio.queries],
                         [b"PLAYER.ELF", b"MIDI.ELF", b"MIDI.SF2"])
        self.assertEqual(audio.puts, [], "ничего не передаётся")
        self.assertEqual(bytes(audio.received), b"MThd" + bytes(3000))

    def test_modules_from_the_volume_wc_came_from(self) -> None:
        # Музыка на другом устройстве (Nemo IDE): /WC/ESP — на томе WC (STREAM #FD),
        # на томе панели его нет. WC без #FD — прежний поиск на томе панели.
        player, midi = self.elf(2000, 23), self.elf(3000, 24)
        audio = AudioEsp()
        h = self.midi(audio, PLAYER_ELF=player, MIDI_ELF=midi, MIDI_SF2=self.BANK)
        h.home, h.root = h.root, {b"MUSIC": {}}
        h.start()
        self.assertEqual([query[0] for query in audio.queries],
                         [b"PLAYER.ELF", b"MIDI.ELF", b"MIDI.SF2"])
        self.assertEqual(audio.started, [b"PLAYER.ELF"])
        self.assertEqual(bytes(audio.received), b"MThd" + bytes(3000))
        old = AudioEsp()
        h = self.midi(old, PLAYER_ELF=player, MIDI_ELF=midi, MIDI_SF2=self.BANK)
        h.wc_stream = False
        h.start()
        self.assertEqual(old.started, [b"PLAYER.ELF"], "WC без #FD: том панели")
        self.assertEqual(bytes(old.received), b"MThd" + bytes(3000))

    def test_aac_and_ogg_files_play_with_their_decoders(self) -> None:
        # Поток, как MP3, но декодер — по расширению: AAC.ELF или OGG.ELF (не MP3.ELF).
        content = random.Random(70).randbytes(30000)
        for name, ext, decoder in ((b"Song.aac", b"AAC", b"AAC.ELF"), (b"Song.ogg", b"OGG", b"OGG.ELF"),
                                   (b"Song.oga", b"OGA", b"OGG.ELF"),
                                   (b"Song.flac", b"FLA", b"FLAC.ELF")):
            with self.subTest(name=name):
                audio = AudioEsp()
                h = self.harness(content, name=name, esp=audio)
                self.card(h, PLAYER_ELF=self.elf(800, 71), MP3_ELF=self.elf(900, 72),
                          AAC_ELF=self.elf(1000, 73), OGG_ELF=self.elf(1100, 74), FLAC_ELF=self.elf(1200, 75))
                h.start()
                self.assertEqual([query[0] for query in audio.queries], [b"PLAYER.ELF", decoder])
                self.assertEqual(audio.opens, [bytes([0]) + len(content).to_bytes(4, "little") +
                                               bytes([3]) + ext])
                self.assertEqual(bytes(audio.received), content)
                self.assertEqual(h.at(FORMAT_ROW, TEXT_MP3),
                                 ("FLAC" if ext == b"FLA" else ext.decode()) + "  128 kbps  44.1 kHz  stereo")
                self.assertEqual(h.hint(), HINT_PLAY, "перематывается, как MP3")

    def test_flac_with_empty_short_extension_plays_and_advances(self) -> None:
        # WC с форматом #11 сравнивает расширение полного имени (.flac — запись
        # FLAC с кодом 0), даже если у короткого имени FAT расширения нет.
        for name in (b"Song.flac", b"Song.FLAC", b"Artist. Song.FlAc"):
            for index, code in ((2, EXIT_NEXT), (5, 0)):
                with self.subTest(name=name,index=index):
                    content = random.Random(180).randbytes(40000)
                    h = self.harness(content, name=name, esp=AudioEsp(capacity=512*1024))
                    item = h.files.pop(h.short)
                    h.short = b"SONGXX~1"
                    h.files[h.short] = item
                    h.long_names[name.upper()] = h.short
                    h.start(index=index, count=5, extension=0)
                    self.assertEqual(bytes(h.audio.received), content)
                    self.assertEqual(h.machine.a, code)
                    self.assertEqual(h.audio.opens[0][6:], b"FLA")

    def test_midi_four_letter_extension_plays_as_midi(self) -> None:
        # .midi — запись MIDI (код 2): то же, что .mid; ESP получает формат MID.
        player, midi = self.elf(800, 190), self.elf(900, 191)
        audio = AudioEsp()
        h = self.harness(b"MThd" + bytes(3000), name=b"Song.midi", esp=audio)
        self.card(h, PLAYER_ELF=player, MIDI_ELF=midi, MIDI_SF2=self.BANK)
        h.start(extension=2)
        self.assertEqual([query[0] for query in audio.queries][:2], [b"PLAYER.ELF", b"MIDI.ELF"])
        self.assertEqual(audio.opens[-1][-3:], b"MID")
        self.assertEqual(bytes(audio.received), b"MThd" + bytes(3000))

    def test_flac_folder_cover_after_metadata(self) -> None:
        content = random.Random(170).randbytes(200000)
        for filename in (b"cover.jpg", b"folder.jpg", b"front.jpg"):
            with self.subTest(filename=filename):
                audio = AudioEsp(capacity=16384, played_per_status=1024)
                h = self.harness(content, name=b"Song.flac", esp=audio)
                self.card(h, PLAYER_ELF=self.elf(800, 171), FLAC_ELF=self.elf(1200, 172))
                h.files[filename] = (len(CoverTest.PICTURE), False, CoverTest.PICTURE)
                h.start()
                self.assertEqual(bytes(audio.picture), CoverTest.PICTURE)
                self.assertEqual(CoverTest.sprite_rows(self, h), audio.thumb)
                self.assertEqual(bytes(audio.received), content)
                self.assertNotIn(AUDIO_COVER, audio.commands)

    def test_flac_embedded_cover_prevents_folder_search(self) -> None:
        audio = AudioEsp(capacity=16384, played_per_status=1024)
        audio.decoder_cover_at = 3
        h = self.harness(random.Random(173).randbytes(60000), name=b"Song.flac", esp=audio)
        self.card(h, PLAYER_ELF=self.elf(800, 174), FLAC_ELF=self.elf(1200, 175))
        h.files[b"cover.jpg"] = (len(CoverTest.PICTURE), False, CoverTest.PICTURE)
        h.start()
        self.assertIn(AUDIO_THUMB, audio.commands)
        self.assertNotIn(AUDIO_PICTURE, audio.commands)
        self.assertEqual(CoverTest.sprite_rows(self, h), audio.thumb)

    def test_ogg_title_comes_from_the_decoder(self) -> None:
        # Метки ID3 у Ogg нет: название — из комментариев Vorbis, их находит декодер;
        # флаг 32 в AUDIO_STATUS — плагин перечитывает метки (PLAYER 1.5).
        audio = AudioEsp()
        audio.tags_known = True
        audio.tags = b"\0\0\0\0\0"
        audio.tags_next = b"Kiss the Dawn\0Guano Apes\0Walking on a Thin Line\0\0\0"
        audio.tags_flag_at = 3
        h = self.harness(random.Random(81).randbytes(40000), name=b"Kiss.ogg", esp=audio)
        self.card(h, PLAYER_ELF=self.elf(800, 82), OGG_ELF=self.elf(1100, 83))
        h.start()
        self.assertFalse(audio.tags_flag, "метки прочитаны — флаг снят")
        self.assertEqual(h.at(TITLE_ROW, TEXT_MP3), "Kiss the Dawn")
        self.assertEqual(h.at(SECOND_ROW, TEXT_MP3), "Guano Apes")

    def test_ogg_cover_from_the_decoder(self) -> None:
        # Обложку в комментариях Vorbis находит декодер: флаг 64 — плагин ждёт
        # миниатюру, как после метки MP3; картинку из папки не ищет дальше.
        audio = AudioEsp(capacity=16384, played_per_status=1024)
        audio.decoder_cover_at = 3
        h = self.harness(random.Random(84).randbytes(60000), name=b"Kiss.ogg", esp=audio)
        self.card(h, PLAYER_ELF=self.elf(800, 85), OGG_ELF=self.elf(1100, 86))
        h.start()
        self.assertFalse(audio.decoder_cover, "AUDIO_THUMB снял флаг")
        self.assertIn(AUDIO_THUMB, audio.commands)
        page = h.page_bytes(VIDEO_BASE + 0)
        self.assertEqual(b"".join(page[row * 256:row * 256 + 32] for row in range(64)),
                         audio.thumb, "миниатюра на экране")
        self.assertNotIn(AUDIO_PICTURE, audio.commands)
        x, y = (7 + 2) * 4, (4 + 2) * 8          # окно X=7, Y=4; обложка — столбец 2, строка 2
        print("sfile", [d.hex() for d in h.sfile])
        self.assertEqual(h.sfile[-1],
                         bytes([y & 0xFF, y >> 8 | 0x6E, x & 0xFF, x >> 8 | 0x0E, 0, 0,
                                0, 0x40, 0, 0, 0, 0, 0, 0x40, 0, 0, 0, 0]),
                         "описатели — как у обложки MP3")

    def test_aac_without_its_decoder_says_so(self) -> None:
        # Встроенный плеер прошивки AAC и Ogg не играет: ошибка с именем декодера.
        for name, text in ((b"Song.aac", "ERROR: missing /wc/esp/AAC.ELF"),
                           (b"Song.ogg", "ERROR: missing /wc/esp/OGG.ELF"),
                           (b"Song.flac", "ERROR: missing /wc/esp/FLAC.ELF")):
            with self.subTest(name=name):
                audio = AudioEsp()
                h = self.harness(bytes(3000), name=name, esp=audio)
                self.card(h, PLAYER_ELF=self.elf(800, 75), MP3_ELF=self.elf(900, 76))
                h.esc_after = 400
                h.start()
                self.assertEqual(h.at(STATUS_ROW, TEXT_MP3), text, "окно потока: слева место обложки")
                self.assertEqual(audio.opens, [], "поток не открывался")
                self.assertEqual(h.machine.a, 0)

    def test_radio_takes_every_stream_decoder_there_is(self) -> None:
        playlist = b"#EXTM3U\r\n#EXTINF:-1,Radio Paradise\r\nhttp://stream.radioparadise.com/aac-128\r\n"
        audio = AudioEsp()
        audio.module_format = FORMAT_MP3
        audio.tags_known = True
        audio.tags = b"Radio Paradise\0Buffering...\0Station 1 of 1\0\0\0AAC\0"
        audio.length = 400
        h = self.harness(playlist, name=b"RADIO.M3U", esp=audio)
        self.card(h, PLAYER_ELF=self.elf(800, 77), MP3_ELF=self.elf(900, 78),
                  AAC_ELF=self.elf(1000, 79), OGG_ELF=self.elf(1100, 80))
        h.esc_after = 60
        h.start()
        self.assertEqual([query[0] for query in audio.queries],
                         [b"PLAYER.ELF", b"MP3.ELF", b"AAC.ELF", b"OGG.ELF"],
                         "кодек станции узнаёт PLAYER: на ESP — все декодеры потока")
        self.assertEqual(audio.started, [b"PLAYER.ELF"])
        self.assertEqual(audio.opens, [bytes([OPEN_MODULE]) + len(playlist).to_bytes(4, "little") +
                                       b"M3U"])

    def test_midi_says_what_is_missing(self) -> None:
        player, midi = self.elf(800, 19), self.elf(900, 20)
        cases = (
            ("no bank", dict(PLAYER_ELF=player, MIDI_ELF=midi), {},
             "ERROR: missing /wc/esp/MIDI.SF2"),
            ("no decoder", dict(PLAYER_ELF=player, MIDI_SF2=self.BANK), {},
             "ERROR: missing /wc/esp/MIDI.ELF"),
            ("no room for the bank", dict(PLAYER_ELF=player, MIDI_ELF=midi, MIDI_SF2=self.BANK),
             dict(resource_room=1000), "ERROR: no room on the ESP for MIDI.SF2"),
            ("firmware without modules", dict(PLAYER_ELF=player, MIDI_ELF=midi,
                                              MIDI_SF2=self.BANK),
             dict(modules_known=False), "ERROR: ESP failed on PLAYER.ELF"),
            ("app does not start", dict(PLAYER_ELF=player, MIDI_ELF=midi, MIDI_SF2=self.BANK),
             dict(start_result=-2), "ERROR: ESP failed on PLAYER.ELF"),
        )
        for label, card, esp, text in cases:
            with self.subTest(label):
                audio = AudioEsp()
                for key, value in esp.items():
                    setattr(audio, key, value)
                h = self.midi(audio, **card)
                h.esc_after = 400
                h.start()
                self.assertEqual(h.at(STATUS_ROW, TEXT_MODULE), text)
                self.assertEqual([body for body in audio.opens if body != bytes([OPEN_RELEASE])],
                                 [], "MIDI встроенный плеер не играет")
                self.assertEqual(audio.releases, 1 if label == "no room for the bank" else 0,
                                 "нет места — ресурсы освобождены, и ещё раз")
                self.assertEqual(h.machine.a, 0)
        # Нет /WC/ESP вовсе.
        audio = AudioEsp()
        h = self.harness(b"MThd" + bytes(300), name=b"Song.mid", esp=audio)
        h.esc_after = 400
        h.start()
        self.assertEqual(h.at(STATUS_ROW, TEXT_MODULE), "ERROR: missing /wc/esp/")

    def test_escape_while_the_bank_goes(self) -> None:
        player, midi = self.elf(800, 21), self.elf(900, 22)
        audio = AudioEsp()
        stamp = self.TIME | self.DATE << 16
        audio.cache = {b"PLAYER.ELF": (800, stamp), b"MIDI.ELF": (900, stamp)}
        h = self.midi(audio, PLAYER_ELF=player, MIDI_ELF=midi, MIDI_SF2=self.BANK)
        h.esc_after = 20
        h.start()
        self.assertEqual(len(audio.puts), 19, "Esc опрашивается перед каждым сектором")
        self.assertNotIn(MOD_COMMIT, audio.commands)
        self.assertLess(audio.commands.index(APP_START), audio.commands.index(MOD_PUT),
                        "банк — после запуска приложения")
        self.assertNotIn(AUDIO_OPEN, audio.commands)
        self.assertEqual(h.calls[-2:], [base.FN_RRESB, base.FN_GEDPL], "окно закрыто")
        self.assertEqual(h.machine.a, 0)



class OplTest(unittest.TestCase):
    """Музыка OPL (OPL.ELF): окно как у MP3, ПЗУ OPL4 — по просьбе декодера."""

    ROM = bytes(range(256)) * 8                 # «YRW801.ROM» стенда: 2 КБ

    @classmethod
    def setUpClass(cls) -> None:
        need(WMF, SYM)

    def test_window_tags_and_chips(self) -> None:
        audio = AudioEsp()
        audio.module_format = FORMAT_OPL
        audio.tags_known = True
        audio.tags = b"Battle Theme\0Composer\0Some Game\x001994\0\0YMF262 (OPL3)\0"
        audio.view_known, audio.view_kind, audio.view_values, audio.view_duration = \
            True, 2, [5, 7, 0, 9], 95
        audio.length = 40
        h = Mp3Harness(b"\x1f\x8b" + bytes(5000), name=b"Battle.vgz", esp=audio)
        h.opl_card()
        h.keys = {FN_RIGHT: {40}}
        h.esc_after = 80
        h.start(index=3, count=7)
        self.assertEqual([query[0] for query in audio.queries], [b"PLAYER.ELF", b"OPL.ELF"],
                         "ПЗУ не просили — не шлётся")
        self.assertEqual(audio.opens, [bytes([OPEN_MODULE]) + (5002).to_bytes(4, "little") + b"VGZ"])
        self.assertEqual(h.at(TITLE_ROW, TEXT_MODULE), "Battle Theme", "обложки нет — с края")
        self.assertEqual(h.at(SECOND_ROW, TEXT_MODULE), "Composer")
        self.assertEqual(h.at(THIRD_ROW, TEXT_MODULE), "Some Game, 1994")
        self.assertEqual(h.at(FORMAT_ROW, TEXT_MODULE), "VGZ  YMF262 (OPL3)")
        self.assertEqual(h.at(TRACK_ROW, TEXT_MODULE), "Track 3 of 7")
        self.assertTrue(h.at(TIME_ROW, TEXT_MODULE).endswith(" 01:35"), h.at(TIME_ROW, TEXT_MODULE))
        self.assertEqual(audio.seeks, [1], "перемотка — у ESP (шаг 10 с)")
        self.assertEqual(h.hint(), HINT_PLAY)

    def test_radio_playlist(self) -> None:
        # .M3U: файл целиком на ESP (расширение M3U), декодер — MP3.ELF; окно как у
        # OPL: станция, песня, «Station 1 of 11», формат потока за метками. Новая
        # песня (флаг 32 в AUDIO_STATUS) — метки перечитываются; влево-вправо —
        # AUDIO_SEEK (у ESP — соседняя станция).
        playlist = b"#EXTM3U\r\n#EXTINF:-1,Radio ROKS\r\nhttp://online.radioroks.ua/RadioROKS\r\n"
        audio = AudioEsp()
        audio.module_format = FORMAT_MP3
        audio.tags_known = True
        audio.tags = b"Radio ROKS\0Connecting...\0Station 1 of 11\0\0\0MP3\0"
        audio.tags_next = (b"Radio ROKS\0Band - Song\0Station 1 of 11\0\0\0"
                           b"MP3  128 kbps  44.1 kHz  stereo\0")
        audio.tags_flag_at = 6
        audio.length = 400
        h = Mp3Harness(playlist, name=b"RADIO.M3U", esp=audio)
        h.radio_card()
        h.keys = {FN_LEFT: {60}}
        h.esc_after = 120
        h.start(index=2, count=5)
        self.assertEqual([query[0] for query in audio.queries], [b"PLAYER.ELF", b"MP3.ELF"],
                         "поток станции — MP3, OPL.ELF не нужен")
        self.assertEqual(audio.opens, [bytes([OPEN_MODULE]) + len(playlist).to_bytes(4, "little") +
                                       b"M3U"])
        self.assertEqual(bytes(audio.received), playlist)
        self.assertGreaterEqual(audio.tags_reads, 2, "песня сменилась — метки перечитаны")
        self.assertFalse(audio.tags_flag)
        self.assertEqual(h.at(TITLE_ROW, TEXT_MODULE), "Radio ROKS")
        self.assertEqual(h.at(SECOND_ROW, TEXT_MODULE), "Band - Song")
        self.assertEqual(h.at(THIRD_ROW, TEXT_MODULE), "Station 1 of 11")
        self.assertEqual(h.at(FORMAT_ROW, TEXT_MODULE), "M3U  MP3  128 kbps  44.1 kHz  stereo")
        self.assertEqual(h.at(TRACK_ROW, TEXT_MODULE), "Track 2 of 5")
        self.assertEqual(audio.seeks, [-1], "влево — у ESP предыдущая станция")
        self.assertEqual(audio.closes, [0], "Esc — сразу")

    def test_radio_page_keys_switch_stations_and_show_the_list(self) -> None:
        # У радио PgDn и PgUp — соседняя станция (не файл панели), автоповтор листает;
        # ESP отвечает на AUDIO_VIEW списком станций — он на месте столбиков.
        playlist = b"#EXTM3U\r\nhttp://a.example/1\r\nhttp://b.example/2\r\n"
        audio = AudioEsp()
        audio.module_format = FORMAT_MP3
        audio.tags_known = True
        audio.tags = b"Station 2\0\0Station 2 of 2\0\0\0MP3\0"
        audio.view_known, audio.view_kind = True, 3
        audio.view_text = (b"\x07   1. Station 1" + b" " * 48 + b"\x0d" +
                           b"\x06\x06>  2. Station 2" + b" " * 48)
        audio.length = 400
        h = Mp3Harness(playlist, name=b"RADIO.M3U", esp=audio)
        h.radio_card()
        h.keys = {FN_PGDN: {50, 51, 52}, FN_PGUP: {90}}
        h.esc_after = 140
        h.start(index=2, count=5)
        self.assertEqual(audio.seeks, [1, -1], "PgDn — следующая станция, PgUp — предыдущая")
        self.assertEqual(audio.closes, [0], "плеер не ушёл к соседнему файлу: выход — по Esc")
        self.assertEqual(h.text(BARS_ROW), "1. Station 1")
        self.assertEqual(h.text(BARS_ROW + 1), ">  2. Station 2")

    def test_untagged_file_shows_its_name(self) -> None:
        audio = AudioEsp()
        audio.module_format = FORMAT_OPL
        audio.tags_known = True
        audio.tags = b"\0\0\0\0\0YM3812 (OPL2)\0"
        audio.length = 4
        h = Mp3Harness(b"DBRAWOPL" + bytes(900), name=b"doofus.dro", esp=audio)
        h.opl_card()
        h.start()
        self.assertEqual(h.at(TITLE_ROW, TEXT_MODULE), "doofus.dro")
        self.assertEqual(h.at(SECOND_ROW, TEXT_MODULE), "", "имя уже в первой строке")
        self.assertEqual(h.at(FORMAT_ROW, TEXT_MODULE), "DRO  YM3812 (OPL2)")

    def test_rom_goes_when_asked_and_the_file_again(self) -> None:
        audio = AudioEsp()
        audio.module_format = FORMAT_OPL
        audio.needs_rom = True
        content = b"Vgm " + bytes(3000)
        h = Mp3Harness(content, name=b"Moon.vgm", esp=audio)
        h.opl_card(rom=self.ROM)
        h.esc_after = 400
        h.start()
        self.assertEqual([(query[0], query[3]) for query in audio.queries],
                         [(b"PLAYER.ELF", 0), (b"OPL.ELF", 0), (b"YRW801.ROM", 1)], "ПЗУ — ресурсом")
        self.assertEqual(audio.stored[b"YRW801.ROM"], self.ROM)
        body = bytes([OPEN_MODULE]) + len(content).to_bytes(4, "little") + b"VGM"
        self.assertEqual(audio.opens, [body, body], "файл — заново, уже с ПЗУ")
        self.assertEqual(bytes(audio.received), content)
        self.assertEqual(audio.releases, 0)
        self.assertTrue(audio.finished, "доиграл")

    def test_without_the_rom_it_plays_as_is(self) -> None:
        audio = AudioEsp()
        audio.module_format = FORMAT_OPL
        audio.needs_rom = True
        h = Mp3Harness(b"Vgm " + bytes(2000), name=b"Moon.vgm", esp=audio)
        h.opl_card()
        h.esc_after = 400
        h.start()
        self.assertEqual([query[0] for query in audio.queries], [b"PLAYER.ELF", b"OPL.ELF"])
        self.assertEqual(len(audio.opens), 1, "без ПЗУ — как есть, файл не заново")
        self.assertTrue(audio.finished)

    @staticmethod
    def mwm_song(kit: bytes) -> bytes:
        """Песня MoonBlaster Wave: имя набора — 8 знаков с байта 276."""
        return b"MBMS\x10\x08" + bytes(270) + kit.ljust(8) + bytes(1500)

    def test_mwm_kit_goes_from_the_song_folder_before_the_song(self) -> None:
        audio = AudioEsp()
        audio.module_format = FORMAT_OPL
        kit = bytes(range(256)) * 12
        content = self.mwm_song(b"TALL_ONE")
        h = Mp3Harness(content, name=b"Darkness.mwm", esp=audio)
        h.opl_card()
        h.files[b"TALL_ONE.MWK"] = (len(kit), False, kit)
        h.esc_after = 400
        h.start()
        self.assertEqual([(query[0], query[3]) for query in audio.queries],
                         [(b"PLAYER.ELF", 0), (b"OPL.ELF", 0), (b"TALL_ONE.MWK", 1)],
                         "набор — ресурсом, из папки песни")
        self.assertEqual(audio.stored[b"TALL_ONE.MWK"], kit)
        commands = audio.commands
        last_commit = len(commands) - 1 - commands[::-1].index(MOD_COMMIT)
        self.assertLess(last_commit, commands.index(AUDIO_OPEN), "набор — до открытия песни")
        self.assertEqual(audio.opens, [bytes([OPEN_MODULE]) + len(content).to_bytes(4, "little") + b"MWM"])
        self.assertEqual(bytes(audio.received), content, "песня — с начала, целиком")
        self.assertTrue(audio.finished)

    def test_mwm_without_a_kit_plays_as_is(self) -> None:
        for kit, files in ((b"NONE", {}), (b"POWERKIT", {})):
            with self.subTest(kit=kit):
                audio = AudioEsp()
                audio.module_format = FORMAT_OPL
                content = self.mwm_song(kit)
                h = Mp3Harness(content, name=b"Song.mwm", esp=audio)
                h.opl_card()
                h.esc_after = 400
                h.start()
                self.assertIn(kit + b".MWK", h.found_names)
                self.assertEqual([query[0] for query in audio.queries], [b"PLAYER.ELF", b"OPL.ELF"])
                self.assertEqual(len(audio.opens), 1)
                self.assertEqual(bytes(audio.received), content)
                self.assertTrue(audio.finished)

    def test_rom_frees_the_midi_bank_when_there_is_no_room(self) -> None:
        audio = AudioEsp()
        audio.module_format = FORMAT_OPL
        audio.needs_rom = True
        audio.resources = {b"MIDI.SF2": 5000}
        audio.resource_room = 6000
        content = b"Vgm " + bytes(1000)
        h = Mp3Harness(content, name=b"Moon.vgm", esp=audio)
        h.opl_card(rom=self.ROM)
        h.esc_after = 400
        h.start()
        body = bytes([OPEN_MODULE]) + len(content).to_bytes(4, "little") + b"VGM"
        self.assertEqual(audio.opens, [body, bytes([OPEN_RELEASE]), body])
        self.assertEqual(audio.releases, 1)
        self.assertEqual(audio.resources, {b"YRW801.ROM": len(self.ROM)})
        self.assertEqual(bytes(audio.received), content)


class SaaTest(unittest.TestCase):
    def test_extensions_controls_and_module_delivery(self):
        for version in (1, 2):
            for ext in ('saa', 'cop'):
                with self.subTest(version=version, ext=ext):
                    audio=AudioEsp();audio.module_format=8;audio.tags_known=True
                    audio.tags=b'\0\0\0\0\0SAA1099 E-Tracker\0';audio.length=100
                    audio.view_known,audio.view_kind,audio.view_values,audio.view_duration=True,2,[2,4,6,8,10,12],185
                    content=bytes(4424)
                    h=Mp3Harness(content,name=('Song.'+ext).encode(),esp=audio)
                    h.link.core.version=version;h.link.core.from_capacity=16384
                    helper=ModuleDeliveryTest()
                    helper.card(h,PLAYER_ELF=helper.elf(1024,1),SAA_ELF=helper.elf(2048,2))
                    h.keys={FN_SPACE:{40,60},FN_RIGHT:{80}};h.esc_after=120
                    h.start(index=2,count=4,extension=6)   # код обработчика SAA/COP
                    self.assertEqual([q[0] for q in audio.queries],[b'PLAYER.ELF',b'SAA.ELF'])
                    self.assertEqual(audio.opens,[bytes([OPEN_MODULE])+len(content).to_bytes(4,'little')+ext.upper().encode()])
                    self.assertEqual(bytes(audio.received),content)
                    self.assertEqual(audio.pauses,[1,0]);self.assertEqual(audio.seeks,[1])
                    self.assertEqual(h.at(FORMAT_ROW,TEXT_MODULE),ext.upper()+'  SAA1099 E-Tracker')
                    self.assertEqual(h.at(TITLE_ROW,TEXT_MODULE),'Song.'+ext)
                    self.assertEqual(h.turbo_requests,[2,0xFF00])

    def test_finish_goes_to_next_file(self):
        audio=AudioEsp();audio.module_format=8;audio.length=4
        h=Mp3Harness(bytes(1024),name=b'Song.cop',esp=audio)
        helper=ModuleDeliveryTest()
        helper.card(h,PLAYER_ELF=helper.elf(1024,1),SAA_ELF=helper.elf(2048,2))
        h.start(index=2,count=5)
        self.assertTrue(audio.finished)
        self.assertEqual(h.machine.a,EXIT_NEXT)
        self.assertEqual(h.turbo_requests,[2,0xFF00])


class FastMediaTest(unittest.TestCase):
    def enable(self,h):
        h.link.core.version=2
        h.link.core.from_capacity=16384
        return h

    def test_waits_for_input_queue_space_without_losing_audio_or_resource(self):
        helper = ModuleDeliveryTest()
        data = random.Random(53).randbytes(20001)
        for ext in ('flac', 'mid'):
            with self.subTest(ext=ext):
                h = self.enable(Mp3Harness(data, name=('Song.' + ext).encode()))
                if ext == 'mid':
                    helper.card(h, PLAYER_ELF=helper.elf(1024, 1),
                                MIDI_ELF=helper.elf(1024, 2), MIDI_SF2=data)
                handle = h.link.core.handle_request
                attempts = []
                def busy(request, *args, **kwargs):
                    if request[2] & 0x10:
                        attempts.append(request[5])
                        if len(attempts) <= 4:
                            kwargs['put_available'] = False
                    return handle(request, *args, **kwargs)
                h.link.core.handle_request = busy
                h.start()
                self.assertGreater(len(attempts), 4)
                self.assertEqual(attempts[:5], [0x6A if ext == 'mid' else 0x31] * 5)
                self.assertEqual(bytes(h.audio.received), data)
                if ext == 'mid':
                    self.assertEqual(h.audio.stored[b'MIDI.SF2'], data)
                self.assertEqual(h.link.core.put_bad, 0)

    def test_queue_wait_has_a_timeout_and_restores_clock(self):
        h = self.enable(Mp3Harness(bytes(20001)))
        h.esc_after = 200                # dismiss the existing error dialog
        handle = h.link.core.handle_request
        attempts = []
        def busy(request, *args, **kwargs):
            if request[2] & 0x10:
                attempts.append(request[1])
                kwargs['put_available'] = False
            return handle(request, *args, **kwargs)
        h.link.core.handle_request = busy
        h.start()
        self.assertEqual(len(attempts), 100)
        self.assertEqual(h.audio.received, b'')
        self.assertEqual(h.turbo_requests, [2, 0xFF00])

    def test_raw_dma_preserves_all_file_lengths(self):
        for size in (0,1,511,512,513,1023,1024,8191,8192,8193,70000):
            with self.subTest(size=size):
                data=random.Random(size).randbytes(size)
                h=self.enable(Mp3Harness(data,name=b'Song.flac'))
                h.start()
                self.assertEqual(bytes(h.audio.received),data)
                self.assertTrue(all(0<n<=8192 for n in h.audio.blocks))
                self.assertEqual(h.own_sectors(),-(-size//512))
                self.assertEqual(h.link.core.put_bad,0)
                if size>8192+512:self.assertIn(8192,h.audio.blocks)

    def test_every_audio_format_uses_the_same_raw_dma(self):
        data=random.Random(15).randbytes(20001)
        helper=ModuleDeliveryTest()
        for ext in ('mp3','flac','wav','aac','ogg','oga','mod','xm','s3m','it',
                    'mid','rmi','kar','vgm','vgz','dro','imf','wlf','mwm','mfm','saa','cop'):
            with self.subTest(ext=ext):
                h=self.enable(Mp3Harness(data,name=('Song.'+ext).encode()))
                helper.card(h,**{name:b'\x7fELF'+bytes(1020) for name in
                    ('PLAYER_ELF','MP3_ELF','FLAC_ELF','AAC_ELF','OGG_ELF','MIDI_ELF','OPL_ELF','TRACKER_ELF','SAA_ELF')},
                    MIDI_SF2=bytes(20001))
                h.start()
                self.assertEqual(bytes(h.audio.received),data)
                self.assertIn(8192,h.audio.blocks)
                self.assertEqual(h.link.core.put_bad,0)

    def test_sf2_and_opl4_rom_use_large_raw_dma_with_exact_offsets(self):
        resource=random.Random(44).randbytes(70001)
        helper=ModuleDeliveryTest()
        for ext,name in (('mid',b'MIDI.SF2'),('vgm',b'YRW801.ROM')):
            with self.subTest(ext=ext):
                audio=AudioEsp()
                h=self.enable(Mp3Harness(bytes(20001),name=('Song.'+ext).encode(),esp=audio))
                if ext=='mid':
                    helper.card(h,PLAYER_ELF=helper.elf(1024,1),MIDI_ELF=helper.elf(1024,2),MIDI_SF2=resource)
                else:
                    audio.module_format=FORMAT_OPL
                    audio.needs_rom=True
                    h.opl_card(rom=resource)
                h.start()
                self.assertEqual(audio.stored[name],resource)
                self.assertIn((65536,4465),audio.puts)
                self.assertIn((0,8192),audio.puts)
                self.assertEqual(bytes(audio.received),bytes(20001))
                self.assertEqual(h.link.core.put_bad,0)

    def test_raw_payload_has_no_xor_retry(self):
        data=random.Random(39).randbytes(20000)
        h=self.enable(Mp3Harness(data))
        h.link.corrupt_puts=1
        h.start()
        expected=bytearray(data);expected[100]^=4
        self.assertEqual(h.audio.received,expected)
        self.assertEqual(h.link.core.put_bad,0,'raw data is deliberately not checksum-validated')


if __name__ == "__main__":
    unittest.main()
