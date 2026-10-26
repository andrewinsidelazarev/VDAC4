#!/bin/sh
# Проверка прошивки BL616 на ПК: fpga_link.c и ядро сопроцессора поверх модели ПЛИС (link_test.c).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
CC=${CC:-clang}
OUT=${OUT:-$HERE/link_test}
"$CC" -std=c11 -O2 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS -DFPGA_LINK_HOST_TEST -I"$HERE/stub" -I"$HERE/.." -I"$HERE/../../copro" \
    "$HERE/link_test.c" "$HERE/../fpga_link.c" "$HERE/../../copro/ft_copro.c" "$HERE/../../copro/ft_inflate.c" "$HERE/../../copro/ft_image.c" "$HERE/../../copro/ft_3d.c" -o "$OUT"
for exec in 8 512; do       # очередь ПЛИС: медленная (прошивка ждёт места) и быстрая
    "$OUT" "$HERE/../../copro/test/data" "$ROOT/VDAC4/ESP32 S3 Zero N8R8/Network/Weather screensaver VDAC2/build/ramg.bin" $exec
done
# Перезагрузка ПЛИС по JTAG с BL616 (fpga_jtag.c) против модели TAP Gowin
"$CC" -std=c11 -O2 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS -DFPGA_JTAG_HOST_TEST -I"$HERE/stub" -I"$HERE/.." \
    "$HERE/jtag_test.c" "$HERE/../fpga_jtag.c" -o "$OUT.jtag"
"$OUT.jtag"
# Загрузка ядра в SRAM ПЛИС по JTAG (core_jtag.c): образ — свой и, если есть, NEXT.BIN ядра Next
"$CC" -std=c11 -O2 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS -DCORE_JTAG_HOST_TEST -I"$HERE/stub" -I"$HERE/.." \
    "$HERE/core_jtag_test.c" "$HERE/../core_jtag.c" -o "$OUT.core_jtag"
"$OUT.core_jtag"
NEXT_BIN="$ROOT/../Next Tang/NextNano-VDAC4/release/NEXT.BIN"
[ -f "$NEXT_BIN" ] && "$OUT.core_jtag" "$NEXT_BIN"
# Кэш ядер во flash BL616 (core_cache.c): запись, круг, загрузка, ошибки, порча каталога
"$CC" -std=c11 -O2 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS -I"$HERE/stub" -I"$HERE/.." -I"$HERE/../../copro" \
    "$HERE/core_cache_test.c" "$HERE/../core_cache.c" "$HERE/../sha256.c" -o "$OUT.core_cache"
if [ -f "$NEXT_BIN" ]; then "$OUT.core_cache" "$NEXT_BIN"; else "$OUT.core_cache"; fi
