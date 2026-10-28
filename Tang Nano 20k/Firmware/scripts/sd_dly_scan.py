"""Окно чтения SDRAM ядра FT812 на плате: точка выборки SD READ (CTRL [7:6]) × тонкая задержка
DQ (SD_DLY 0xEC, IODELAYA, шаг ~18 пс, ядро 1.27+) — сеткой; на каждой — `tang ftspeed` ESP
(запись RAM_G с 0x080000 и сверка 64 случайных слов чтением). Нужно ядро FT812 на плате (идёт
программа FT812 или `tang boot`) и сетевая консоль ESP.

    python sd_dly_scan.py IP [--points 1,2,3] [--step 4] [--kb 16] [--set P:D]
    python sd_dly_scan.py IP --phases [0-15] [--points 0,1,2,3] [--kb 16] [--keep]
    python sd_dly_scan.py IP --set-phase F

--set P:D — только поставить точку P и задержку D (без обхода). После обхода CTRL и SD_DLY
возвращаются как были, если не задан --keep (тогда остаётся лучшая найденная пара).

С ядра FT812 1.33 (SD_DLY читается с меткой D2) клок микросхемы SDRAM — CLKOUTP второй PLL со
сдвигом F/16 периода clk_sd (123,75 МГц, шаг ~0,5 нс; запись SD_DLY с битом 24, [23:20]):
--phases — сетка «сдвиг × точка выборки»; сдвиг меняет и запись, и чтение, поэтому на каждой паре
ftspeed пишет RAM_G заново и сверяет. Пока идёт обход, содержимое SDRAM (RAM_G программы, кадры)
может портиться — после него программу FT812 лучше перезапустить. --set-phase F — только сдвиг.
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


def set_phase(ip: str, f: int) -> None:
    wr(ip, 0xEC, (1 << 24) | ((f & 15) << 20))


def phase_grid(ip: str, ctrl0: int, dly0: int, phases: list[int], points: list[int], kb: int, keep: bool) -> int:
    """Сетка «сдвиг клока × точка выборки»: число верных слов из 64 в каждой клетке."""
    f0 = dly0 >> 20 & 15
    grid = {}
    try:
        for f in phases:
            set_phase(ip, f)
            for p in points:
                set_point(ip, ctrl0, p)
                # в негодной клетке ESP может не дописать RAM_G («write failed») — это 0 из 64
                try:
                    good, ns = measure(ip, kb)
                except RuntimeError as e:
                    good, ns = 0, 0
                    print(f"  ({e})", flush=True)
                grid[f, p] = good
                print(f"сдвиг {f:2} ({f * 22.5:5.1f}°, ~{f * 8.08 / 16:.2f} нс), SD READ {p}: {good:2}/64, {ns:6} нс/слово",
                      flush=True)
                if dly0 >> 24 == 0xD2 and rd(ip, 0xEC) >> 24 != 0xD2:
                    raise RuntimeError("ядро FT812 ушло (SD_DLY без метки D2): программа FT812 на Evo остановилась?")
    except BaseException:
        # вернуть — только если ядро FT812 ещё работает: CTRL TS-Config чужим словом не портить
        if rd(ip, 0xEC) >> 24 == 0xD2:
            set_phase(ip, f0)
            wr(ip, 0x40, ctrl0)
            print(f"обход прерван — сдвиг и CTRL возвращены ({f0}, {ctrl0:08X})", flush=True)
        raise
    print("сдвиг \\ точка " + " ".join(f"{p:>3}" for p in points))
    for f in phases:
        print(f"{f:13} " + " ".join(f"{grid[f, p]:3}" for p in points))
    # лучшая клетка — середина самого длинного (по кругу сдвигов) участка, где все 64 верны
    best = None
    for p in points:
        ok = [grid.get((f, p), 0) == 64 for f in range(16)]
        if all(ok):
            run = (16, 0)
        else:
            run, start = (0, 0), ok.index(False)
            n = 0
            for k in range(1, 17):
                f = (start + k) % 16
                n = n + 1 if ok[f] else 0
                if n > run[0]:
                    run = (n, (f - n + 1) % 16)
        if run[0] and (best is None or run[0] > best[0]):
            best = (run[0], (run[1] + run[0] // 2) % 16, p)
    if best:
        print(f"лучшее: сдвиг {best[1]}, SD READ {best[2]} — годных сдвигов подряд {best[0]}")
    else:
        print("годных клеток (64/64) нет")
    if keep and best:
        set_phase(ip, best[1])
        set_point(ip, ctrl0, best[2])
        print("оставлено лучшее")
    else:
        set_phase(ip, f0)
        wr(ip, 0x40, ctrl0)
        print(f"сдвиг и CTRL возвращены ({f0}, {ctrl0:08X})")
    return 0


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
    ap.add_argument("--phases", nargs="?", const="0-15")
    ap.add_argument("--set-phase", type=int)
    a = ap.parse_args()
    ctrl0 = rd(a.ip, 0x40)
    dly0 = rd(a.ip, 0xEC)
    if dly0 >> 24 not in (0xD1, 0xD2):
        print(f"SD_DLY = {dly0:08X}: в этом ядре тонкой задержки нет (нужно ядро FT812 1.27+)")
        return 1
    if a.phases is not None or a.set_phase is not None:
        if dly0 >> 24 != 0xD2:
            print(f"SD_DLY = {dly0:08X}: сдвига клока SDRAM нет (нужно ядро FT812 1.33+)")
            return 1
        if a.set_phase is not None:
            set_phase(a.ip, a.set_phase)
            print(f"сдвиг {a.set_phase}: SD_DLY {rd(a.ip, 0xEC):08X}")
            return 0
        lo, _, hi = a.phases.partition("-")
        phases = list(range(int(lo), int(hi or lo) + 1))
        points = [int(v) for v in (a.points if a.points != "1,2,3" else "0,1,2,3").split(",")]
        print(f"CTRL {ctrl0:08X} (SD READ {ctrl0 >> 6 & 3}), SD_DLY {dly0:08X} (сдвиг {dly0 >> 20 & 15})")
        return phase_grid(a.ip, ctrl0, dly0, phases, points, a.kb, a.keep)
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
