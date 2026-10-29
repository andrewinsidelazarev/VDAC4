#!/bin/sh
# Стенд Verilator ядра видео целиком (rtl/vp_core.sv: медиа-FIFO и регистры FT812, разбор AVI
# vp_avi, декодер third_party/core_jpeg, запись кадров vp_fbwrite, темп vp_pace) — tb/jpeg/tb_vpc.cpp.
#
#   scripts/run_vpc_sim.sh [КАТАЛОГ]  — собрать КАТАЛОГ/tb_vpc.exe (по умолчанию E:/zx/cache/tmp/vpc_sim)
#
# Сторож в стенде короче (WD_CYCLES = 300000), деблокер и зерно выключены (DEBLOCK = 0, DITHER = 0:
# эталон стенда — кадр декодера как есть; их проверяет run_vpd_sim.sh), остальное — как в ядре.
# VP_VFLAGS — ключи Verilator,
# VP_CXXFLAGS — компилятора: VP_VFLAGS=--public-flat-rw VP_CXXFLAGS=-DVP_TRACE — журнал чанков
# разбора и состояние при остановке.
# Нужны Verilator 5 (verilator_bin; VERILATOR_ROOT) и clang++ (CXX), как у run_vsys.sh.
set -e
VR=${VERILATOR_ROOT:-E:/zx/tools/rtl/ucrt64/share/verilator}
VB=${VERILATOR_BIN:-/e/zx/tools/rtl/ucrt64/bin/verilator_bin.exe}
CXX=${CXX:-/e/zx/llvm/bin/clang++}
export VERILATOR_ROOT="$VR"

out=${1:-E:/zx/cache/tmp/vpc_sim}
src=third_party/core_jpeg/src_v
mkdir -p "$out"
rm -rf "$out/obj"
"$VB" --cc -O3 --x-assign fast --x-initial fast --noassert -Wno-fatal -Wno-lint -Wno-style \
    -I"$src" -Irtl --top-module vp_core -GWD_CYCLES=300000 -GDEBLOCK=0 -GDITHER=0 --Mdir "$out/obj" $VP_VFLAGS \
    "$src"/*.v rtl/vp_dbk_lp.sv rtl/vp_noise_rom.sv rtl/vp_fbwrite.sv rtl/vp_avi.sv rtl/vp_pace.sv rtl/vp_core.sv
"$CXX" -O2 -std=c++20 -w -D_CRT_SECURE_NO_WARNINGS $VP_CXXFLAGS -I"$out/obj" -I"$VR/include" -I"$VR/include/vltstd" \
    "$out"/obj/*.cpp "$VR/include/verilated.cpp" "$VR/include/verilated_threads.cpp" \
    tb/jpeg/tb_vpc.cpp -o "$out/tb_vpc.exe"
echo "собран $out/tb_vpc.exe"
