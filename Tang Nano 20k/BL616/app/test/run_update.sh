#!/bin/sh
# Сквозная проверка обновления BL616 по сети на ПК: код ESP + код BL616 + модель ПЛИС.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
ESP="$ROOT/VDAC4/ESP32 S3 Zero N8R8/Firmware"
CC=${CC:-clang}
CXX=${CXX:-clang++}
OUT=${OUT:-$HERE/update_test}
WORK=$(dirname "$OUT")
cc1() {
    "$1" -O2 -Wall -Wextra -Wno-unused-but-set-global -D_CRT_SECURE_NO_WARNINGS -DFPGA_LINK_HOST_TEST -DLINK_MODEL_ONLY \
        -I"$HERE/stub" -I"$HERE/.." -I"$HERE/../../copro" -I"$ESP/include" "$2" -c "$3" -o "$4"
}
for f in link_test fpga_link fw_update sha256; do
    src="$HERE/$f.c"
    [ -f "$src" ] || src="$HERE/../$f.c"
    cc1 "$CC" -std=c11 "$src" "$WORK/upd_$f.o"
done
cc1 "$CXX" -std=c++17 "$ESP/src/bl616_push.cpp" "$WORK/upd_bl616_push.o"
cc1 "$CXX" -std=c++17 "$HERE/update_test.cpp" "$WORK/upd_update_test.o"
"$CXX" "$WORK"/upd_*.o -o "$OUT"
"$OUT"
