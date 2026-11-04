#!/bin/sh
# Сборка и прогон проверки сопроцессора на ПК (clang или gcc). Данные вписывания и видео (data/fit,
# data/video) не хранятся — делаются генераторами, если их нет (нужен Python с Pillow: PYTHON).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)          # каталог VDAC4
CC=${CC:-clang}
PY=${PYTHON:-python}
OUT=${OUT:-$HERE/host_test}
[ -f "$HERE/data/fit/list.txt" ] || "$PY" "$HERE/make_image_data.py" fit
[ -f "$HERE/data/video/list.txt" ] || "$PY" "$HERE/make_video_data.py"
"$CC" -std=c11 -O2 -Wall -Wextra -Wno-unused-function -D_CRT_SECURE_NO_WARNINGS -I"$HERE/.."     "$HERE/host_test.c" "$HERE/../ft_copro.c" "$HERE/../ft_inflate.c" "$HERE/../ft_image.c" "$HERE/../ft_3d.c" -DFTC3D_PROBE -o "$OUT"
"$OUT" "$HERE/data" "$ROOT/ESP32 S3 Zero N8R8/Network/Weather screensaver VDAC2/build/ramg.bin"
