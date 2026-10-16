#!/usr/bin/env python3
"""Обновление прошивки Tang Nano 20K на плате VDAC4 по сети, через ESP (порт 8268).

    python tools/tang_tool.py update 192.168.1.113 "../../Tang Nano 20k/Firmware/firmware/ts_fb_hdmi_top.fs"

Образ — битстрим Gowin: .fs (текст из нулей и единиц, его упаковывает этот скрипт) или
уже упакованный .bin — то же, что Gowin Programmer пишет во flash Tang с адреса 0.
ESP принимает образ целиком в PSRAM, проверяет SHA-256 и заголовок (слово синхронизации
A5C3, IDCODE GW2AR-18), стирает и пишет flash Tang через ядро Tang (мост к flash, ядро 1.1+)
и читает обратно для сверки. Сектор настроек (0x7FF000) не трогается. Новое ядро Tang
загрузит при следующем включении питания.

Нужна только стандартная библиотека Python; COM-порты не используются.
"""

from __future__ import annotations

import argparse
import hashlib
import socket
import sys
from pathlib import Path

PORT = 8268
CONNECT_TIMEOUT = 10.0
STEP_TIMEOUT = 60.0
IDCODE_CHECK = bytes([0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x1B])


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


def receive_line(sock: socket.socket) -> str:
    data = bytearray()
    while True:
        byte = sock.recv(1)
        if not byte:
            raise ToolError("ESP закрыл соединение")
        if byte == b"\n":
            return data.decode("utf-8", errors="replace").rstrip("\r")
        data += byte


def update(address: str, path: Path) -> None:
    image = load_image(path)
    check_image(image)
    digest = hashlib.sha256(image).hexdigest()
    print(f"Образ: {path}")
    print(f"Размер: {len(image)} байт, SHA-256: {digest}")
    with socket.create_connection((address, PORT), timeout=CONNECT_TIMEOUT) as sock:
        sock.settimeout(STEP_TIMEOUT)
        banner = receive_line(sock).split()
        if len(banner) != 3 or banner[0] != "ZIFI-TANG/1":
            raise ToolError("на порту не служба обновления Tang: " + " ".join(banner))
        maximum = int(banner[2])
        print(f"ESP: {banner[1]}, наибольший образ {maximum} байт")
        if len(image) > maximum:
            raise ToolError(f"образ {len(image)} байт больше {maximum}")
        sock.sendall(f"BEGIN {len(image)} {digest}\n".encode("ascii"))
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
                print("Записано и проверено. Новое ядро Tang загрузится при следующем включении.")
                return
            raise ToolError("ESP: " + line)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Обновление прошивки Tang через ESP")
    sub = parser.add_subparsers(dest="command", required=True)
    up = sub.add_parser("update", help="записать битстрим во flash Tang")
    up.add_argument("address", help="IP-адрес ESP")
    up.add_argument("image", type=Path, help=".fs или упакованный .bin")
    arguments = parser.parse_args(argv)
    try:
        if arguments.command == "update":
            update(arguments.address, arguments.image)
    except (OSError, ToolError, ValueError) as error:
        print(f"ОШИБКА: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
