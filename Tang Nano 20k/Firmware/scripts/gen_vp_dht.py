#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Генератор ПЗУ стандартных таблиц Хаффмана для ядра видео: rtl/vp_dht_std.svh.

Запуск из fpga/vdac2:  py -3 scripts/gen_vp_dht.py
Результат детерминирован; файл rtl/vp_dht_std.svh не править руками.

Кадр MJPEG без DHT (камеры, «AVI1») декодируется стандартными таблицами ISO/IEC 10918-1,
приложение K.3, как у FT812. Разбор AVI (vp_avi) вставляет их в поток перед SOS: байт FF
маркера SOS уже ушёл в декодер, поэтому вставка — сегмент DHT без своего FF (C4, длина 0x01A2,
четыре таблицы) и FF, за которыми идёт DA. Всего 420 байтов, индекс 0…419.
Таблицы — как в libjpeg (jstdhuff.c): ДЦ и АЦ яркости, ДЦ и АЦ цветности.
"""
from pathlib import Path

Y_DC = bytes.fromhex(
    "000001050101010101010000000000000000010203040506"
    "0708090a0b")

Y_AC = bytes.fromhex(
    "100002010303020403050504040000017d01020300041105"
    "122131410613516107227114328191a1082342b1c11552d1"
    "f02433627282090a161718191a25262728292a3435363738"
    "393a434445464748494a535455565758595a636465666768"
    "696a737475767778797a838485868788898a929394959697"
    "98999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4"
    "c5c6c7c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7e8e9"
    "eaf1f2f3f4f5f6f7f8f9fa")

C_DC = bytes.fromhex(
    "010003010101010101010101000000000000010203040506"
    "0708090a0b")

C_AC = bytes.fromhex(
    "110002010204040304070504040001027700010203110405"
    "2131061241510761711322328108144291a1b1c109233352"
    "f0156272d10a162434e125f11718191a262728292a353637"
    "38393a434445464748494a535455565758595a6364656667"
    "68696a737475767778797a82838485868788898a92939495"
    "969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2"
    "c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae2e3e4e5e6e7e8"
    "e9eaf2f3f4f5f6f7f8f9fa")


def main():
    seg = Y_DC + Y_AC + C_DC + C_AC
    assert len(seg) == 416
    seq = bytes([0xC4, 0x01, 0xA2]) + seg + bytes([0xFF])
    assert len(seq) == 420 and 2 + len(seg) == 0x01A2
    out = [
        "// SPDX-License-Identifier: Apache-2.0",
        "//",
        "// Стандартные таблицы Хаффмана (K.3) для кадра MJPEG без DHT (vp_avi). СГЕНЕРИРОВАНО",
        "// scripts/gen_vp_dht.py — не править руками. vp_dht_byte(i), i = 0…419: сегмент DHT без",
        "// первого FF (C4, длина 01A2, таблицы 00, 10, 01, 11) и FF маркера SOS, который идёт следом.",
        "//",
        "localparam int VP_DHT_LEN = %d;" % len(seq),
        "function automatic [7:0] vp_dht_byte(input [8:0] i);",
        "    case (i)",
    ]
    for k, b in enumerate(seq):
        out.append("        9'd%d: vp_dht_byte = 8'h%02X;" % (k, b))
    out += ["        default: vp_dht_byte = 8'h00;", "    endcase", "endfunction", ""]
    Path("rtl/vp_dht_std.svh").write_text("\n".join(out), encoding="utf-8", newline="\n")
    print("rtl/vp_dht_std.svh: %d байтов" % len(seq))


if __name__ == "__main__":
    main()
