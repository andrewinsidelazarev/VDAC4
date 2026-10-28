#!/bin/sh
# Стенд Verilator ядра видео: декодер JPEG (third_party/core_jpeg) → запись кадра (rtl/vp_fbwrite.sv)
# → модель SDRAM (tb/jpeg/vp_tb_top.sv, tb/jpeg/tb_vp.cpp).
#
#   scripts/run_vp_sim.sh [КАТАЛОГ]  — собрать КАТАЛОГ/tb_vp.exe (по умолчанию E:/zx/cache/tmp/vp_sim)
#
# Нужны Verilator 5 (verilator_bin; VERILATOR_ROOT) и clang++ (CXX), как у run_vsys.sh.
set -e
VR=${VERILATOR_ROOT:-E:/zx/tools/rtl/ucrt64/share/verilator}
VB=${VERILATOR_BIN:-/e/zx/tools/rtl/ucrt64/bin/verilator_bin.exe}
CXX=${CXX:-/e/zx/llvm/bin/clang++}
export VERILATOR_ROOT="$VR"

out=${1:-E:/zx/cache/tmp/vp_sim}
src=third_party/core_jpeg/src_v
mkdir -p "$out"
rm -rf "$out/obj"
"$VB" --cc -O3 --x-assign fast --x-initial fast --noassert -Wno-fatal -Wno-lint -Wno-style \
    -I"$src" -Irtl --top-module vp_tb_top --Mdir "$out/obj" "$src"/*.v rtl/vp_fbwrite.sv tb/jpeg/vp_tb_top.sv
"$CXX" -O2 -std=c++20 -w -D_CRT_SECURE_NO_WARNINGS -I"$out/obj" -I"$VR/include" -I"$VR/include/vltstd" \
    "$out"/obj/*.cpp "$VR/include/verilated.cpp" "$VR/include/verilated_threads.cpp" \
    tb/jpeg/tb_vp.cpp -o "$out/tb_vp.exe"
echo "собран $out/tb_vp.exe"
