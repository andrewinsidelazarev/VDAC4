#!/usr/bin/env python3
"""Итог подбора сдвига клока SDRAM ядра FT812 (1.38+) на плате: регистр SD_TRAIN (0xEC)
через сетевую консоль ESP (порт 23, команда tang rd). Только чтение; консоль после ответа
отпускается сразу.

    python scripts/sd_train_read.py [192.168.1.113]

Ядро FT812 должно быть загружено (идёт программа FT812 на Evo): у TS-Config в 0xEC — 0.
"""
import re
import socket
import sys
import time


def console(host, commands, seconds=3.0):
    out = b""
    with socket.create_connection((host, 23), timeout=5) as sock:
        sock.settimeout(0.3)
        for c in commands:
            sock.sendall(c.encode("ascii") + b"\n")
        until = time.monotonic() + seconds
        while time.monotonic() < until and out.count(b"tang ") < len(commands):
            try:
                data = sock.recv(4096)
            except socket.timeout:
                continue
            if not data:
                break
            out += data
    return out.decode("ascii", "replace")


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.113"
    text = console(host, ["tang rd 04", "tang rd EC"])
    got = dict(re.findall(r"tang ([0-9A-F]{2}) = ([0-9A-F]{8})", text))
    if not all(a in got for a in ("04", "EC")):
        print(text)
        print("ответ консоли неполный")
        return 1
    v, s = (int(got[a], 16) for a in ("04", "EC"))
    print(f"VERSION {v:08X}: личность {v >> 24:02X}, ядро {(v >> 8) & 0xFF}.{v & 0xFF:02d}")
    if v >> 24 != 0x02 or (v & 0xFFFF) < 0x0126:
        print("это не ядро FT812 1.38+: подбора в нём нет")
        return 1
    print(f"SD_TRAIN {s:08X}: сдвиг {s & 15} ({(s & 15) * 22.5:.1f}°), такт {(s >> 4) & 3}, "
          f"идёт {(s >> 6) & 1}, закончен {(s >> 7) & 1}, годных нет {(s >> 8) & 1}, "
          f"сверка не сошлась {(s >> 9) & 1}, участок {(s >> 10) & 127} пар, середина k={(s >> 17) & 63}")
    print("k = 16·такт + 15 − сдвиг; шаг сдвига — 1/16 такта 121,5 МГц (0,51 нс)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
