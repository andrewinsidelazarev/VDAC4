"""Read the firmware log of the VDAC4 ESP32-S3 module from its USB port.

The module's console is USB-Serial-JTAG, so the log is only readable while the
module is out of the board and connected to a PC:

    python tools/serial_log.py COM13 --reset --seconds 12 --save boot.log

--reset restarts the chip into the application (not into the bootloader) and
captures the log from the very first line. Nothing is written to the flash.
Needs pyserial (it is present in the ESP-IDF Python environment).
"""

from __future__ import annotations

import argparse
import sys
import time

import serial


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("port")
    parser.add_argument("--seconds", type=float, default=10.0,
                        help="how long to listen (default: 10)")
    parser.add_argument("--reset", action="store_true",
                        help="restart the chip into the application first")
    parser.add_argument("--save", help="also write the raw log to this file")
    options = parser.parse_args()

    port = serial.Serial()
    port.port = options.port
    port.baudrate = 115200
    port.timeout = 0.1
    # DTR выбирает режим загрузки, RTS — сброс. Оба сняты до открытия порта:
    # иначе чип ушёл бы в загрузчик.
    port.dtr = False
    port.rts = False
    port.open()
    try:
        if options.reset:
            # Драйвер usbser.sys в Windows отправляет состояние RTS только
            # вместе с записью DTR, поэтому DTR каждый раз переписывается тем
            # же значением (так же поступает esptool).
            port.rts = True
            port.dtr = port.dtr
            time.sleep(0.2)
            port.rts = False
            port.dtr = port.dtr
        collected = bytearray()
        deadline = time.monotonic() + options.seconds
        while time.monotonic() < deadline:
            chunk = port.read(4096)
            if chunk:
                collected += chunk
                sys.stdout.write(chunk.decode("utf-8", "replace"))
                sys.stdout.flush()
    finally:
        port.close()
    if options.save:
        with open(options.save, "wb") as stream:
            stream.write(collected)
    print(f"\n--- {len(collected)} bytes in {options.seconds:g} s", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
