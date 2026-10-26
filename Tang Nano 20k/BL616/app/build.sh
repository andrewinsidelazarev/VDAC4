#!/bin/sh
# Сборка прошивки BL616. bouffalo_sdk не собирает проект из пути с пробелами, поэтому app и copro
# копируются в WORK (по умолчанию — во временный каталог) и собираются там; готовый образ —
# в out/vdac4_copro_bl616.bin. Нужны BL_SDK_BASE и в PATH: riscv64-unknown-elf-gcc (T-Head), make,
# ninja, cmake, python3 — см. README.md.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-${TMP:-/tmp}/vdac4_copro_build}
mkdir -p "$WORK/app" "$WORK/copro"
cp -p "$HERE"/CMakeLists.txt "$HERE"/Makefile "$HERE"/Kconfig "$HERE"/defconfig "$HERE"/*.c "$HERE"/*.h "$WORK/app/"
cp -p "$HERE/../copro/"*.c "$HERE/../copro/"*.h "$WORK/copro/"
cp -rp "$HERE/../copro/third_party" "$WORK/copro/"
(cd "$WORK/app" && make CHIP=bl616 BOARD=bl616dk)
mkdir -p "$HERE/out"
cp "$WORK/app/build/build_out/vdac4_copro_bl616.bin" "$HERE/out/"
ls -l "$HERE/out/vdac4_copro_bl616.bin"
