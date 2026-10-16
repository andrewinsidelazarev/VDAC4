"""Сетевая консоль платы VDAC4 (порт 23, src/net_console.cpp): читать вывод.

    python tools/net_console.py 192.168.1.110 [--seconds 30] [--command log]
        [--save E:/zx/esp/console1.txt]

Печатает историю (последние 32 КБ вывода прошивки) и всё новое, пока не
пройдёт --seconds (0 — до Ctrl+C). --command посылает команду консоли (help,
mem, ver, log, tang) сразу после подключения. Всё, кроме «tang wr», только читает;
«tang wr АДРЕС ЗНАЧЕНИЕ» (hex) пишет регистр Tang по LINK — например,
«tang wr 40 …» меняет CTRL (источник, MUX, SD READ), как кнопки Tang.
"""
import argparse
import socket
import sys
import time


def main() -> int:
    parser = argparse.ArgumentParser(description="VDAC4 network console reader")
    parser.add_argument("address")
    parser.add_argument("--port", type=int, default=23)
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--command", action="append", default=[])
    parser.add_argument("--save")
    arguments = parser.parse_args()

    output = open(arguments.save, "wb") if arguments.save else None
    try:
        with socket.create_connection((arguments.address, arguments.port), timeout=5) as sock:
            sock.settimeout(0.5)
            for command in arguments.command:
                sock.sendall(command.encode("ascii") + b"\n")
            until = time.monotonic() + arguments.seconds if arguments.seconds > 0 else None
            while until is None or time.monotonic() < until:
                try:
                    data = sock.recv(4096)
                except socket.timeout:
                    continue
                if not data:
                    break
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
                if output:
                    output.write(data)
    except OSError as error:
        print(f"console: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        pass
    finally:
        if output:
            output.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
