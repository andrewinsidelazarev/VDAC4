#!/bin/sh
# Стенд Verilator декодера JPEG core_jpeg (third_party/core_jpeg/src_v, tb/jpeg/tb_jpeg.cpp).
#
#   scripts/run_jpeg_sim.sh [КАТАЛОГ [ключи Verilator…]]  — собрать КАТАЛОГ/tb_jpeg.exe
#       (по умолчанию E:/zx/cache/tmp/jpeg_sim; ключи — например -GSUPPORT_WRITABLE_DHT=1)
#
# Нужны Verilator 5 (verilator_bin; VERILATOR_ROOT) и clang++ (CXX), как у run_vsys.sh.
set -e
VR=${VERILATOR_ROOT:-E:/zx/tools/rtl/ucrt64/share/verilator}
VB=${VERILATOR_BIN:-/e/zx/tools/rtl/ucrt64/bin/verilator_bin.exe}
CXX=${CXX:-/e/zx/llvm/bin/clang++}
export VERILATOR_ROOT="$VR"

out=${1:-E:/zx/cache/tmp/jpeg_sim}
[ $# -ge 1 ] && shift
src=third_party/core_jpeg/src_v
mkdir -p "$out"
rm -rf "$out/obj"
"$VB" --cc -O3 --x-assign fast --x-initial fast --noassert -Wno-fatal -Wno-lint -Wno-style \
    -I"$src" --top-module jpeg_core --Mdir "$out/obj" "$@" "$src"/*.v
"$CXX" -O2 -std=c++20 -w -D_CRT_SECURE_NO_WARNINGS -I"$out/obj" -I"$VR/include" -I"$VR/include/vltstd" \
    "$out"/obj/*.cpp "$VR/include/verilated.cpp" "$VR/include/verilated_threads.cpp" \
    tb/jpeg/tb_jpeg.cpp -o "$out/tb_jpeg.exe"
echo "собран $out/tb_jpeg.exe"
