#!/bin/sh
# Сборка и прогон проверки сопроцессора на ПК (clang или gcc).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
CC=${CC:-clang}
OUT=${OUT:-$HERE/host_test}
"$CC" -std=c11 -O2 -Wall -Wextra -Wno-unused-function -D_CRT_SECURE_NO_WARNINGS -I"$HERE/.." \
    "$HERE/host_test.c" "$HERE/../ft_copro.c" "$HERE/../ft_inflate.c" "$HERE/../ft_image.c" "$HERE/../ft_3d.c" -DFTC3D_PROBE -o "$OUT"
"$OUT" "$HERE/data" "$ROOT/VDAC4/ESP32 S3 Zero N8R8/Network/Weather screensaver VDAC2/build/ramg.bin"
