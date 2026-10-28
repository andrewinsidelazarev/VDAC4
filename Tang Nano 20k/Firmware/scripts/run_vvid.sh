#!/bin/sh
# Стенд Verilator ядра видео целиком (tb/vsys/vvid_top.sv, vvid_main.cpp): ts_fb_core HAS_VIDEO +
# потактовая SDRAM, хост FTVIEW4 по выводам SPI, сверка выхода 720p50 с моделью развёртки.
#
#   scripts/run_vvid.sh [КАТАЛОГ]  — собрать КАТАЛОГ/vvid.exe (по умолчанию E:/zx/cache/tmp/vvid)
#   КАТАЛОГ/vvid.exe поток.avi ожидание.txt [--dump КАТАЛОГ]  — прогон (поток — make_vp_avi.py)
#
# Нужны Verilator 5 (verilator_bin; VERILATOR_ROOT) и clang++ (CXX). Список RTL — из SRC
# scripts/run_fb_sim.sh (без tb/) и файлы ядра видео.
set -e
VR=${VERILATOR_ROOT:-E:/zx/tools/rtl/ucrt64/share/verilator}
VB=${VERILATOR_BIN:-/e/zx/tools/rtl/ucrt64/bin/verilator_bin.exe}
CXX=${CXX:-/e/zx/llvm/bin/clang++}
export VERILATOR_ROOT="$VR"

out=${1:-E:/zx/cache/tmp/vvid}
src=$(sed -n '/^SRC="/,/"$/p' scripts/run_fb_sim.sh | sed 's/^SRC="//; s/"$//' |
      tr ' ' '\n' | grep '^rtl/' | grep -v ts_fb_hdmi_top | tr '\n' ' ')
[ -n "$src" ] || { echo "нет SRC в scripts/run_fb_sim.sh"; exit 1; }
jpeg=third_party/core_jpeg/src_v
mkdir -p "$out"
rm -rf "$out/obj"
"$VB" --cc -O3 --x-assign fast --x-initial fast --noassert -Wno-fatal -Wno-lint -Wno-style \
    -Wno-MULTIDRIVEN -DSIMULATION -Irtl -I"$jpeg" --top-module vvid_top --Mdir "$out/obj" \
    $src rtl/vp_core.sv rtl/vp_avi.sv rtl/vp_pace.sv rtl/vp_fbwrite.sv "$jpeg"/*.v \
    tb/vsys/sdram_cyc.sv tb/vsys/vvid_top.sv
"$CXX" -O2 -std=c++20 -w -D_CRT_SECURE_NO_WARNINGS -I"$out/obj" -I"$VR/include" -I"$VR/include/vltstd" \
    "$out"/obj/*.cpp "$VR/include/verilated.cpp" "$VR/include/verilated_threads.cpp" \
    tb/vsys/vvid_main.cpp -o "$out/vvid.exe"
echo "собран $out/vvid.exe"
