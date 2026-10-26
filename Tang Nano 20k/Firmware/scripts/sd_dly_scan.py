"""Окно чтения SDRAM ядра FT812 на плате: точка выборки SD READ (CTRL [7:6]) × тонкая задержка
DQ (SD_DLY 0xEC, IODELAYA, шаг ~18 пс, ядро 1.27+) — сеткой; на каждой — `tang ftspeed` ESP
(запись RAM_G с 0x080000 и сверка 64 случайных слов чтением). Нужно ядро FT812 на плате (идёт
программа FT812 или `tang boot`) и сетевая консоль ESP.

    python sd_dly_scan.py IP [--points 1,2,3] [--step 4] [--kb 16] [--set P:D]

--set P:D — только поставить точку P и задержку D (без обхода). После обхода CTRL и SD_DLY
возвращаются как были, если не задан --keep (тогда остаётся лучшая найденная пара).
"""
import argparse
import re
import socket
import sys
import time

PORT = 23


def command(ip: str, line: str, timeout: float = 30.0) -> str:
    with socket.create_connection((ip, PORT), timeout=timeout) as s:
        s.settimeout(timeout)
        s.sendall((line + "\n").encode("ascii"))
        data = b""
        end = time.time() + timeout
        while time.time() < end:
            try:
                chunk = s.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            data += chunk
            if b"\ntang " in data and data.rstrip().endswith((b"kHz", b"ok", b")")) and line.split()[1] in ("ftspeed",):
                pass
        return data.decode("utf-8", "replace")


def tang(ip: str, args: str) -> str:
    out = command(ip, "tang " + args)
    lines = [l for l in out.splitlines() if l.startswith("tang")]
    return lines[-1] if lines else out.strip().splitlines()[-1] if out.strip() else ""


def rd(ip: str, reg: int) -> int:
    m = re.search(r"=\s*([0-9A-Fa-f]{8})", tang(ip, f"rd {reg:02X}"))
    if not m:
        raise RuntimeError(f"нет ответа на rd {reg:02X}")
    return int(m.group(1), 16)


def wr(ip: str, reg: int, value: int) -> None:
    tang(ip, f"wr {reg:02X} {value:08X}")


def set_point(ip: str, ctrl0: int, point: int) -> None:
    wr(ip, 0x40, (ctrl0 & ~0xC0) | (point << 6))


def set_delay(ip: str, d: int) -> None:
    wr(ip, 0xEC, d & 127)
    for _ in range(50):
        if not rd(ip, 0xEC) >> 15 & 1:
            return
        time.sleep(0.01)


def measure(ip: str, kb: int) -> tuple[int, int]:
    line = tang(ip, f"ftspeed {kb}")
    m = re.search(r"(\d+) ns/word.*check (\d+)/64", line)
    if not m:
        raise RuntimeError("ftspeed: " + line)
    return int(m.group(2)), int(m.group(1))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("ip")
    ap.add_argument("--points", default="1,2,3")
    ap.add_argument("--step", type=int, default=4)
    ap.add_argument("--kb", type=int, default=16)
    ap.add_argument("--set")
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    ctrl0 = rd(a.ip, 0x40)
    dly0 = rd(a.ip, 0xEC)
    if dly0 >> 24 != 0xD1:
        print(f"SD_DLY = {dly0:08X}: в этом ядре тонкой задержки нет (нужно ядро FT812 1.27+)")
        return 1
    print(f"CTRL {ctrl0:08X} (SD READ {ctrl0 >> 6 & 3}), SD_DLY {dly0:08X} (задержка {dly0 >> 8 & 127})")
    if a.set:
        p, d = (int(v) for v in a.set.split(":"))
        set_point(a.ip, ctrl0, p)
        set_delay(a.ip, d)
        good, ns = measure(a.ip, a.kb)
        print(f"SD READ {p}, задержка {d}: {good}/64, {ns} нс/слово")
        return 0
    best = (-1, 0, 0)
    for p in (int(v) for v in a.points.split(",")):
        set_point(a.ip, ctrl0, p)
        row = []
        for d in range(0, 128, a.step):
            set_delay(a.ip, d)
            good, ns = measure(a.ip, a.kb)
            row.append(good)
            print(f"SD READ {p}, задержка {d:3} (~{d * 18 / 1000:.2f} нс): {good:2}/64, {ns:6} нс/слово", flush=True)
            if good > best[0]:
                best = (good, p, d)
        print(f"  точка {p}: " + " ".join(f"{g:2}" for g in row))
    print(f"лучшее: SD READ {best[1]}, задержка {best[2]} — {best[0]}/64")
    if a.keep:
        set_point(a.ip, ctrl0, best[1])
        set_delay(a.ip, best[2])
        print("оставлено лучшее")
    else:
        wr(a.ip, 0x40, ctrl0)
        set_delay(a.ip, dly0 & 127)
        print(f"CTRL и SD_DLY возвращены ({ctrl0:08X}, задержка {dly0 & 127})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
