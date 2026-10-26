#!/usr/bin/env python3
"""Обновление прошивки Tang Nano 20K на плате VDAC4 по сети, через ESP (порт 8268).

    python tools/tang_tool.py update 192.168.1.113 "../../Tang Nano 20k/Firmware/firmware/ts_fb_hdmi_top.fs"
    python tools/tang_tool.py update --slot 1 192.168.1.113 ts_fb_slot1.fs
    python tools/tang_tool.py bl616 192.168.1.113 vdac4_copro_bl616.bin
    python tools/tang_tool.py sd-put 192.168.1.113 NEXT.BIN CORES/NEXT.BIN   # файл на microSD Tang (ESP 0.1.91+)

Образ — битстрим Gowin: .fs (текст из нулей и единиц, его упаковывает этот скрипт) или
уже упакованный .bin — то же, что Gowin Programmer пишет во flash Tang с адреса 0.
ESP принимает образ целиком в PSRAM, проверяет SHA-256 и заголовок (слово синхронизации
A5C3, IDCODE GW2AR-18), стирает и пишет flash Tang через ядро Tang (мост к flash, ядро 1.1+)
и читает обратно для сверки. Сектор настроек (0x7FF000) не трогается. Новое ядро Tang
загрузит при следующем включении питания.

--slot N (ESP 0.1.76+) — слот multiboot: образ пишется с N × 0x100000. При включении ПЛИС
всегда грузит слот 0; регистр BOOT ядра 1.11+ (сетевая консоль: «tang boot») перезагружает
её в слот, адрес которого записан в заголовке текущего битстрима (слот 0 → 1, слот 1 → 0).
Скрипт печатает этот адрес и предупреждает, если он ведёт в тот же слот.

--slot 2+ с образом шрифтов ПЗУ FT812 (VD4FONT1, ESP 0.1.77+): его собирает
fpga/vdac2/scripts/ft_rom_fonts.py из образа ПЗУ, снятого с bt8xxemu (данные Bridgetek — в
репозиторий не кладутся). Ядро FT812 1.18+ берёт шрифты из слота 2: ESP переписывает их в SDRAM
после каждой загрузки ядра.

    python tools/tang_tool.py update --slot 2 192.168.1.113 romfonts.bin

bl616 — прошивка BL616 модуля Tang (сопроцессор FT812, образ bouffalo_sdk с заголовком BFNP,
до 256 КБ). Нужны ядро Tang 1.8+ и прошивка BL616 с обновлением по сети (версия 2+; первая
ставится по USB). ESP принимает образ и проверяет SHA-256, затем кусками передаёт его BL616
через почтовый ящик ядра Tang; BL616 собирает образ в RAM, проверяет SHA-256 и заголовок,
пишет во flash с 0x40000 и перезагружается. OK приходит, когда новая прошивка уже работает.

ftdump (ESP 0.1.78+) — захват кадра FT812 с платы, пока программа Evo работает: регистры,
дисплей-лист и RAM_G читаются окном FT_ADDR/FT_DATA ядра FT812 (программа этого не замечает).
Лист берётся из заднего банка RAM_DL, пока смена листа ждёт (REG_DLSWAP ≠ 0): сопроцессор
его дописал, а следующий CMD_DLSTART ждёт смены. В каталог — ramg.bin, dl.bin (8 КБ), regs.bin
(0x302000…), info.json (FT_RTIME — время кадра растеризатора на плате); дальше кадр идёт в
fpga/vdac2/scripts/ft_rtl_check.py prep/check КАТАЛОГ ramg.bin dl.bin.

    python tools/tang_tool.py ftdump 192.168.1.113 zuma_frame

Нужна только стандартная библиотека Python; COM-порты не используются.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import socket
import sys
import time
from pathlib import Path

PORT = 8268
CONNECT_TIMEOUT = 10.0
STEP_TIMEOUT = 60.0
IDCODE_CHECK = bytes([0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x1B])
SLOT_BYTES = 0x100000
JUMP_COMMAND = bytes([0xD2, 0x00, 0xFF, 0xFF])   # + 4 байта адреса перехода multiboot (UG290)
FONT_MAGIC = b"VD4FONT1"
FONT_SLOT = 2


class ToolError(RuntimeError):
    pass


def load_image(path: Path) -> bytes:
    if path.suffix.lower() == ".fs":
        bits = []
        for line in path.read_text(encoding="ascii", errors="replace").splitlines():
            line = line.strip()
            if not line or line.startswith("//"):
                continue
            if set(line) - {"0", "1"}:
                raise ToolError(f"{path}: в строке битстрима не только 0 и 1")
            bits.append(line)
        data = "".join(bits)
        if len(data) % 8:
            raise ToolError(f"{path}: число битов {len(data)} не кратно 8")
        return bytes(int(data[i:i + 8], 2) for i in range(0, len(data), 8))
    return path.read_bytes()


def check_image(image: bytes) -> None:
    head = image[:64]
    if b"\xa5\xc3" not in head:
        raise ToolError("в заголовке нет слова синхронизации A5C3 — это не битстрим Gowin")
    if IDCODE_CHECK not in head:
        raise ToolError("заголовок не проверяет IDCODE 0x0000081B — битстрим не для GW2AR-18")


def check_fonts(image: bytes) -> list[tuple[int, int, int]]:
    """Образ шрифтов ПЗУ FT812: куски {адрес, длина, смещение} — те же проверки, что у ESP."""
    if len(image) < 12:
        raise ToolError("образ шрифтов короче заголовка")
    count = int.from_bytes(image[8:12], "little")
    if not 1 <= count <= 8 or len(image) < 12 + 12 * count:
        raise ToolError(f"образ шрифтов: число кусков {count}")
    spans = []
    for i in range(count):
        e = image[12 + 12 * i:24 + 12 * i]
        addr, length, offset = (int.from_bytes(e[k:k + 4], "little") for k in (0, 4, 8))
        if (not length or (addr | length | offset) & 3 or addr < 0x200000 or addr + length > 0x300000
                or offset < 12 + 12 * count or offset + length > len(image)):
            raise ToolError(f"образ шрифтов: кусок {i} негодный (0x{addr:X}, {length}, {offset})")
        spans.append((addr, length, offset))
    return spans


def jump_address(image: bytes) -> int | None:
    at = image[:128].find(JUMP_COMMAND)
    if at < 0 or at + 8 > len(image):
        return None
    return int.from_bytes(image[at + 4:at + 8], "big")


def receive_line(sock: socket.socket) -> str:
    data = bytearray()
    while True:
        byte = sock.recv(1)
        if not byte:
            raise ToolError("ESP закрыл соединение")
        if byte == b"\n":
            return data.decode("utf-8", errors="replace").rstrip("\r")
        data += byte


def bl616(address: str, path: Path) -> None:
    image = path.read_bytes()
    if image[:4] != b"BFNP":
        raise ToolError("нет заголовка BFNP — это не образ BL616")
    if not 4096 <= len(image) <= 256 * 1024:
        raise ToolError(f"размер {len(image)} байт: нужно 4 КБ…256 КБ")
    digest = hashlib.sha256(image).hexdigest()
    print(f"Образ BL616: {path}")
    print(f"Размер: {len(image)} байт, SHA-256: {digest}")
    with socket.create_connection((address, PORT), timeout=CONNECT_TIMEOUT) as sock:
        sock.settimeout(STEP_TIMEOUT)
        banner = receive_line(sock).split()
        if len(banner) != 3 or banner[0] != "ZIFI-TANG/1":
            raise ToolError("на порту не служба обновления Tang: " + " ".join(banner))
        print(f"ESP: {banner[1]}")
        sock.sendall(f"BL616 {len(image)} {digest}\n".encode("ascii"))
        reply = receive_line(sock).split(maxsplit=1)
        if reply[0] != "READY":
            raise ToolError("ESP отказал: " + " ".join(reply))
        sock.sendall(image)
        print("Передано; ESP передаёт образ BL616 ...")
        while True:
            line = receive_line(sock)
            parts = line.split()
            if parts and parts[0] == "PROGRESS" and len(parts) == 4:
                print(f"\r  {parts[1]:7} {parts[2]}/{parts[3]}   ", end="", flush=True)
                continue
            print()
            if parts and parts[0] == "OK":
                if len(parts) != 3 or parts[1].lower() != digest:
                    raise ToolError("подтверждение ESP с другим SHA-256: " + line)
                info = int(parts[2], 16)
                print(f"Записано; новая прошивка BL616 работает: версия {(info >> 16) & 0xFF}, "
                      f"CP_INFO {info:08X}.")
                return
            raise ToolError("ESP: " + line)


def core_session(address: str, command: str):
    sock = socket.create_connection((address, PORT), timeout=CONNECT_TIMEOUT)
    sock.settimeout(STEP_TIMEOUT)
    banner = receive_line(sock).split()
    if len(banner) != 3 or banner[0] != "ZIFI-TANG/1":
        sock.close()
        raise ToolError("на порту не служба обновления Tang: " + " ".join(banner))
    print(f"ESP: {banner[1]}")
    sock.sendall((command + "\n").encode("ascii"))
    return sock


def core_reply(sock: socket.socket) -> list[str]:
    while True:
        line = receive_line(sock)
        parts = line.split()
        if parts and parts[0] == "PROGRESS" and len(parts) == 4:
            print(f"\r  {parts[1]:7} {parts[2]}/{parts[3]}   ", end="", flush=True)
            continue
        print()
        if parts and parts[0] == "OK":
            return parts
        raise ToolError("ESP: " + line)


def core_info(address: str) -> None:
    with core_session(address, "COREINFO") as sock:
        mb = int(core_reply(sock)[1])
    if mb == 0:
        print("Кэша ядер нет: flash BL616 мала")
    elif mb == 255:
        print("На месте кэша ядер во flash BL616 — чьи-то данные: кэш выключен (нужен FORMAT)")
    else:
        print(f"Кэш ядер: flash BL616 {mb} МБ")


def core_put(address: str, path: Path, name: str) -> None:
    image = path.read_bytes()
    if image[:2] != b"\xff\xff" or b"\xa5\xc3" not in image[:64]:
        raise ToolError("не двоичный битстрим Gowin (.bin): нет синхрослова A5C3")
    digest = hashlib.sha256(image).hexdigest()
    print(f"Ядро {name}: {path}, {len(image)} байт, SHA-256 {digest}")
    with core_session(address, f"COREPUT {name} {len(image)} {digest}") as sock:
        reply = receive_line(sock).split(maxsplit=1)
        if reply[0] != "READY":
            raise ToolError("ESP отказал: " + " ".join(reply))
        sock.sendall(image)
        print("Передано; ESP передаёт ядро в кэш BL616 ...")
        parts = core_reply(sock)
    if len(parts) != 2 or parts[1].lower() != digest:
        raise ToolError("подтверждение ESP с другим SHA-256: " + " ".join(parts))
    print("Ядро в кэше BL616.")


def sd_put(address: str, path: Path, target: str) -> None:
    # файл на microSD Tang (мост ядра TS-Config 1.30+): ESP пишет его во временный, читает обратно
    # со сверкой SHA-256 и только тогда заменяет прежний
    data = path.read_bytes()
    if not data or len(data) > 4 * 1024 * 1024:
        raise ToolError(f"размер {len(data)} байт: нужно 1 байт…4 МБ")
    target = target.replace("\\", "/").lstrip("/")
    digest = hashlib.sha256(data).hexdigest()
    print(f"{path} -> SD Tang /{target}: {len(data)} байт, SHA-256 {digest}")
    with core_session(address, f"SDPUT {target} {len(data)} {digest}") as sock:
        reply = receive_line(sock).split(maxsplit=1)
        if reply[0] != "READY":
            raise ToolError("ESP отказал: " + " ".join(reply))
        sock.sendall(data)
        print("Передано; ESP пишет файл на карту Tang ...")
        parts = core_reply(sock)
    if len(parts) != 2 or parts[1].lower() != digest:
        raise ToolError("подтверждение ESP с другим SHA-256: " + " ".join(parts))
    print("Записано на карту Tang и прочитано обратно.")


def core_format(address: str) -> None:
    # пустой каталог кэша: данные на месте кэша (INFO 255) пропадут
    with core_session(address, "COREFORMAT") as sock:
        core_reply(sock)
    print("Кэш ядер BL616: записан пустой каталог")


def core_load(address: str, name: str) -> None:
    with core_session(address, f"CORELOAD {name}") as sock:
        sock.settimeout(STEP_TIMEOUT + 20)
        parts = core_reply(sock)
    version = int(parts[1], 16)
    print(f"Работает ядро {name}: личность {version >> 24:02X}, VERSION {version:08X}")


def update(address: str, path: Path, slot: int = 0) -> None:
    image = load_image(path)
    fonts = image[:8] == FONT_MAGIC
    if fonts:
        if slot < FONT_SLOT:
            raise ToolError(f"образ шрифтов ПЗУ — только в слот {FONT_SLOT}+ (слоты 0 и 1 — ядра)")
        spans = check_fonts(image)
    else:
        check_image(image)
    digest = hashlib.sha256(image).hexdigest()
    base = slot * SLOT_BYTES
    print(f"Образ: {path}")
    print(f"Размер: {len(image)} байт, SHA-256: {digest}")
    if slot and len(image) > SLOT_BYTES:
        raise ToolError(f"образ {len(image)} байт больше слота ({SLOT_BYTES} байт)")
    jump = None if fonts else jump_address(image)
    if fonts:
        print(f"Слот {slot} (0x{base:06X}): шрифты ПЗУ FT812, кусков {len(spans)}: "
              + ", ".join(f"0x{a:06X}+{n}" for a, n, _ in spans))
        if slot != FONT_SLOT:
            print(f"ВНИМАНИЕ: ESP берёт шрифты только из слота {FONT_SLOT}")
    elif jump is None:
        print(f"Слот {slot} (0x{base:06X}); перехода multiboot в заголовке нет")
    else:
        print(f"Слот {slot} (0x{base:06X}), переход multiboot по BOOT: 0x{jump:06X}")
        if jump == base:
            print("ВНИМАНИЕ: BOOT перезагрузит ПЛИС в этот же слот")
    with socket.create_connection((address, PORT), timeout=CONNECT_TIMEOUT) as sock:
        sock.settimeout(STEP_TIMEOUT)
        banner = receive_line(sock).split()
        if len(banner) != 3 or banner[0] != "ZIFI-TANG/1":
            raise ToolError("на порту не служба обновления Tang: " + " ".join(banner))
        maximum = int(banner[2])
        print(f"ESP: {banner[1]}, наибольший образ {maximum} байт")
        if len(image) > maximum:
            raise ToolError(f"образ {len(image)} байт больше {maximum}")
        # адрес слота — только для слотов 1+: ESP до 0.1.76 его не знает
        command = f"BEGIN {len(image)} {digest}" + (f" {base:X}" if slot else "")
        sock.sendall((command + "\n").encode("ascii"))
        reply = receive_line(sock).split(maxsplit=1)
        if reply[0] != "READY":
            raise ToolError("ESP отказал: " + " ".join(reply))
        sock.sendall(image)
        print("Передано; ESP проверяет и пишет flash Tang ...")
        while True:
            line = receive_line(sock)
            parts = line.split()
            if parts and parts[0] == "PROGRESS" and len(parts) == 4:
                print(f"\r  {parts[1]:7} {parts[2]}/{parts[3]}   ", end="", flush=True)
                continue
            print()
            if parts and parts[0] == "OK":
                if len(parts) != 2 or parts[1].lower() != digest:
                    raise ToolError("подтверждение ESP с другим SHA-256: " + line)
                if fonts:
                    print("Записано и проверено. Шрифты попадут в ядро FT812 при его следующей загрузке.")
                elif slot:
                    print(f"Записано и проверено. Слот {slot} загрузится по BOOT («tang boot»).")
                else:
                    print("Записано и проверено. Новое ядро Tang загрузится при следующем включении.")
                return
            raise ToolError("ESP: " + line)


REG_DLSWAP = 0x302054
REG_CMD_DL = 0x302100
FT_CLOCK_KHZ = 61875


def receive_exact(sock: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        part = sock.recv(min(65536, size - len(data)))
        if not part:
            raise ToolError("ESP закрыл соединение посреди данных (обмен LINK сорвался?)")
        data += part
    return bytes(data)


def ftdump(address: str, out: Path, ramg_bytes: int, tries: int = 100) -> None:
    with socket.create_connection((address, PORT), timeout=CONNECT_TIMEOUT) as sock:
        sock.settimeout(STEP_TIMEOUT)
        banner = receive_line(sock).split()
        if len(banner) != 3 or banner[0] != "ZIFI-TANG/1":
            raise ToolError("на порту не служба обновления Tang: " + " ".join(banner))
        print(f"ESP: {banner[1]}")
        sock.sendall(b"FTREAD\n")
        reply = receive_line(sock)
        if reply != "READY":
            raise ToolError("ESP: " + reply)

        def link(reg: int) -> int:
            sock.sendall(f"LINK {reg:X}\n".encode("ascii"))
            parts = receive_line(sock).split()
            if len(parts) != 2 or parts[0] != "VALUE":
                raise ToolError("ESP: " + " ".join(parts))
            return int(parts[1], 16)

        def read(addr: int, words: int) -> bytes:
            sock.sendall(f"R {addr:X} {words}\n".encode("ascii"))
            parts = receive_line(sock).split()
            if len(parts) != 2 or parts[0] != "DATA" or int(parts[1]) != 4 * words:
                raise ToolError("ESP: " + " ".join(parts))
            return receive_exact(sock, 4 * words)

        def r32(addr: int) -> int:
            return int.from_bytes(read(addr, 1), "little")

        rtime, rstat, cpinfo = link(0x2C), link(0x28), link(0x30)
        regs = read(0x302000, 0x180)
        # Лист: задний банк RAM_DL, пока смена ждёт — сопроцессор его дописал и не трогает
        dl = None
        for attempt in range(1, tries + 1):
            if r32(REG_DLSWAP) == 0:
                continue
            length = r32(REG_CMD_DL)
            t0 = time.monotonic()
            candidate = read(0x300000, 2048)
            if r32(REG_DLSWAP) != 0:
                dl = candidate
                print(f"Лист: {length} байт по REG_CMD_DL, прочитан за "
                      f"{(time.monotonic() - t0) * 1000:.0f} мс, попытка {attempt}")
                break
        if dl is None:
            raise ToolError(f"за {tries} попыток лист не прочитан целиком до смены (REG_DLSWAP)")
        t0 = time.monotonic()
        # Кусками по ~0,5 с: служба ESP 0.1.78 читает команду без уступки ядра, и 1 МБ одной
        # командой (~17 с) будил сторожевой таймер задач; между командами она ждёт в recv
        piece = 8192
        ramg = b"".join(read(a, min(piece, ramg_bytes // 4 - a // 4))
                        for a in range(0, ramg_bytes, 4 * piece))
        print(f"RAM_G: {ramg_bytes} байт за {time.monotonic() - t0:.1f} с")
        sock.sendall(b"END\n")
    out.mkdir(parents=True, exist_ok=True)
    (out / "dl.bin").write_bytes(dl)
    (out / "ramg.bin").write_bytes(ramg)
    (out / "regs.bin").write_bytes(regs)
    frame_ms = (rtime & 0xFFFF) * 1024 / FT_CLOCK_KHZ
    info = {"ft_rtime": f"{rtime:08X}", "ft_rstat": f"{rstat:08X}", "cp_info": f"{cpinfo:08X}",
            "frame_ms": round(frame_ms, 2), "cmd_dl": length,
            "hsize": int.from_bytes(regs[0x34:0x38], "little"),
            "vsize": int.from_bytes(regs[0x48:0x4C], "little")}
    (out / "info.json").write_text(json.dumps(info, indent=1), encoding="utf-8")
    print(f"Кадр растеризатора на плате: {frame_ms:.1f} мс (FT_RTIME {rtime:08X}), "
          f"FT_RSTAT {rstat:08X}; HSIZE×VSIZE {info['hsize']}×{info['vsize']} -> {out}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Обновление прошивки Tang через ESP")
    sub = parser.add_subparsers(dest="command", required=True)
    up = sub.add_parser("update", help="записать битстрим во flash Tang")
    up.add_argument("--slot", type=int, default=0, choices=range(0, 7),
                    help="слот flash Tang по 1 МБ: 0 — ядро при включении (по умолчанию), 1 — ядро FT812, "
                         "2 — шрифты ПЗУ FT812")
    up.add_argument("address", help="IP-адрес ESP")
    up.add_argument("image", type=Path, help=".fs, упакованный .bin или образ шрифтов VD4FONT1")
    fd = sub.add_parser("ftdump", help="захватить кадр FT812 с платы (регистры, лист, RAM_G)")
    fd.add_argument("address", help="IP-адрес ESP")
    fd.add_argument("out", type=Path, help="каталог для ramg.bin, dl.bin, regs.bin, info.json")
    fd.add_argument("--ramg", type=lambda v: int(v, 0), default=0x100000,
                    help="сколько байт RAM_G читать (по умолчанию 0x100000)")
    bl = sub.add_parser("bl616", help="записать прошивку BL616 модуля Tang (сопроцессор FT812)")
    bl.add_argument("address", help="IP-адрес ESP")
    bl.add_argument("image", type=Path, help="образ bouffalo_sdk (vdac4_copro_bl616.bin)")
    ci = sub.add_parser("core-info", help="кэш ядер во flash BL616: объём")
    ci.add_argument("address", help="IP-адрес ESP")
    cp = sub.add_parser("core-put", help="положить ядро (двоичный битстрим .bin с меткой VDAC4) в кэш BL616")
    cp.add_argument("address", help="IP-адрес ESP")
    cp.add_argument("image", type=Path, help="двоичный битстрим Gowin (.bin), например NEXT.BIN")
    cp.add_argument("name", help="имя ядра в кэше: до 15 знаков A-Z 0-9 _ - .")
    sp = sub.add_parser("sd-put", help="записать файл на microSD Tang (например, CORES/NEXT.BIN)")
    sp.add_argument("address", help="IP-адрес ESP")
    sp.add_argument("file", type=Path, help="файл на ПК")
    sp.add_argument("target", help="путь от корня карты Tang: CORES/NEXT.BIN")
    cf = sub.add_parser("core-format", help="пустой каталог кэша ядер BL616 (данные на месте кэша пропадут)")
    cf.add_argument("address", help="IP-адрес ESP")
    cf.add_argument("--yes", action="store_true", help="да, стереть каталог кэша")
    cl = sub.add_parser("core-load", help="загрузить ядро из кэша BL616 в ПЛИС Tang")
    cl.add_argument("address", help="IP-адрес ESP")
    cl.add_argument("name", help="имя ядра в кэше")
    arguments = parser.parse_args(argv)
    try:
        if arguments.command == "update":
            update(arguments.address, arguments.image, arguments.slot)
        elif arguments.command == "ftdump":
            ftdump(arguments.address, arguments.out, arguments.ramg)
        elif arguments.command == "bl616":
            bl616(arguments.address, arguments.image)
        elif arguments.command == "core-info":
            core_info(arguments.address)
        elif arguments.command == "core-put":
            core_put(arguments.address, arguments.image, arguments.name)
        elif arguments.command == "sd-put":
            sd_put(arguments.address, arguments.file, arguments.target)
        elif arguments.command == "core-format":
            if not arguments.yes:
                raise ToolError("core-format стирает каталог кэша ядер BL616: повторите с --yes")
            core_format(arguments.address)
        elif arguments.command == "core-load":
            core_load(arguments.address, arguments.name)
    except (OSError, ToolError, ValueError) as error:
        print(f"ОШИБКА: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
