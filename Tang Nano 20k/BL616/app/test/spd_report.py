"""Отчёт самопроверки частот SPI BL616 (прошивка 9, fpga_link.c: fpga_speed_test) с платы:
32 слова RAM_G с 0x1DFF00 и CP_INFO — через службу обновления Tang в ESP (порт 8268, FTREAD,
как tang_tool.py ftdump; только чтение). Разметка — fpga_link.h (FPGA_REPORT_*).

    python spd_report.py IP
"""
import socket
import sys

PORT = 8268
REPORT = 0x1DFF00
MAGIC2 = 0x32445053           # "SPD2"
WORDS = 16384                 # 64 КБ на замер


def line(sock):
    data = bytearray()
    while True:
        b = sock.recv(1)
        if not b:
            raise SystemExit("ESP закрыл соединение")
        if b == b"\n":
            return data.decode("utf-8", "replace").rstrip("\r")
        data += b


def exact(sock, n):
    out = bytearray()
    while len(out) < n:
        chunk = sock.recv(n - len(out))
        if not chunk:
            raise SystemExit("ESP закрыл соединение")
        out += chunk
    return bytes(out)


def rate(us):
    return f"{64 * 1e6 / us / 1024:5.2f} МБ/с" if us else "   —"


def main(ip):
    with socket.create_connection((ip, PORT), timeout=10) as s:
        s.settimeout(30)
        banner = line(s)
        if not banner.startswith("ZIFI-TANG/1"):
            raise SystemExit("не служба Tang: " + banner)
        s.sendall(b"FTREAD\n")
        if line(s) != "READY":
            raise SystemExit("ESP не дала FTREAD (в Tang нет ядра FT812?)")
        s.sendall(b"LINK 30\n")
        info = int(line(s).split()[1], 16)
        s.sendall(f"R {REPORT:X} 32\n".encode())
        hdr = line(s).split()
        if hdr[0] != "DATA":
            raise SystemExit("ESP: " + " ".join(hdr))
        raw = exact(s, 128)
        s.sendall(b"END\n")
    w = [int.from_bytes(raw[4 * i:4 * i + 4], "little") for i in range(32)]
    code = (info >> 10) & 3
    print(f"CP_INFO {info:08X}: прошивка {(info >> 16) & 0xFF}, ошибка {(info >> 12) & 15}, "
          f"запись — {['ещё не проверена (40 МГц)', '20 МГц', '40 МГц', '80 МГц'][code]}")
    if w[0] != MAGIC2:
        print(f"отчёта v2 нет (слово 0 = {w[0]:08X})")
        return
    print(f"выбрано: запись {w[1] / 1e6:g} МГц, слова чтения {w[2] / 1e6:g} МГц; "
          f"непрерывный режим {'да' if w[3] & 1 else 'нет'}, чтение по 32 бита {'да' if w[3] & 2 else 'нет'}")
    for name, base in (("запись 80 МГц, паузы", 4), ("запись 80 МГц, непрерывно", 9)):
        e, us, dma, wait, snaps = w[base:base + 5]
        print(f"  {name:28s} ошибок {e:5d}, {us / 1000:6.2f} мс ({rate(us)}); DMA {dma / 1000:6.2f} мс, "
              f"ожидание места {wait / 1000:5.2f} мс, снимков {snaps}")
    print(f"  {'запись 40 МГц, непрерывно':28s} ошибок {w[14]:5d}, {w[15] / 1000:6.2f} мс ({rate(w[15])})")
    for name, base in (("чтение 20 МГц, по байту", 16), ("чтение 20 МГц, по 32 бита", 18),
                       ("чтение 40 МГц, по 32 бита", 20), ("чтение 53,3 МГц (2+1)", 22),
                       ("чтение 53,3 МГц (1+2)", 24), ("чтение 80 МГц, по 32 бита", 26)):
        e, us = w[base], w[base + 1]
        print(f"  {name:28s} ошибок {e:5d} из {WORDS}, {us / 1000:6.2f} мс ({rate(us)})")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "192.168.1.113")
