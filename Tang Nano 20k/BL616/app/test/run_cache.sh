#!/bin/sh
# Сквозная проверка кэша ядер на ПК: код ESP (core_cache_client.cpp) + код BL616 (core_cache.c,
# fpga_link.c) + модель ПЛИС (link_test.c). Ядро — NEXT.BIN ядра Next, если он есть.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
ESP="$ROOT/VDAC4/ESP32 S3 Zero N8R8/Firmware"
CC=${CC:-clang}
CXX=${CXX:-clang++}
OUT=${OUT:-$HERE/cache_test}
WORK=$(dirname "$OUT")
cc1() {
    "$1" -O2 -Wall -Wextra -Wno-unused-but-set-global -D_CRT_SECURE_NO_WARNINGS -DFPGA_LINK_HOST_TEST -DLINK_MODEL_ONLY \
        -I"$HERE/stub" -I"$HERE/.." -I"$HERE/../../copro" -I"$ESP/include" "$2" -c "$3" -o "$4"
}
for f in link_test fpga_link core_cache sha256; do
    src="$HERE/$f.c"
    [ -f "$src" ] || src="$HERE/../$f.c"
    cc1 "$CC" -std=c11 "$src" "$WORK/cch_$f.o"
done
cc1 "$CXX" -std=c++17 "$ESP/src/core_cache_client.cpp" "$WORK/cch_client.o"
cc1 "$CXX" -std=c++17 "$HERE/cache_test.cpp" "$WORK/cch_cache_test.o"
"$CXX" "$WORK"/cch_*.o -o "$OUT"
NEXT_BIN="$ROOT/../Next Tang/NextNano-VDAC4/release/NEXT.BIN"
if [ -f "$NEXT_BIN" ]; then "$OUT" "$NEXT_BIN"; else "$OUT"; fi
