#!/bin/sh
# Стенд Verilator ядра FT812 (tb/vsys): ядро слота 1 целиком + SDRAM + вывод 720p60, хост Evo —
# игра в замкнутом цикле по REG_DLSWAP (tb/vsys/vsys_main.cpp).
#
#   scripts/run_vsys.sh build [КОРЕНЬ [КАТАЛОГ [ключи Verilator…]]]   — собрать vsys.exe; КОРЕНЬ —
#       дерево с rtl/ и scripts/run_fb_sim.sh (по умолчанию . — текущее ядро; out/core_1.19 —
#       выпущенное), КАТАЛОГ — куда (по умолчанию sim/vsys/cur); ключи — например -GH_FP=440 (вывод
#       720p50, как у ядра FT812 до 1.20)
#   КАТАЛОГ/vsys.exe --trace T.bin --ramg R.bin [--seconds S] …  — прогон (ключи — vsys_main.cpp)
#
# Нужны Verilator 5 (verilator_bin; VERILATOR_ROOT) и clang++ (CXX). Список RTL — из SRC
# run_fb_sim.sh того же дерева (без tb/).
set -e
VR=${VERILATOR_ROOT:-E:/zx/tools/rtl/ucrt64/share/verilator}
VB=${VERILATOR_BIN:-/e/zx/tools/rtl/ucrt64/bin/verilator_bin.exe}
CXX=${CXX:-/e/zx/llvm/bin/clang++}
export VERILATOR_ROOT="$VR"

cmd=${1:-build}
root=${2:-.}
out=${3:-sim/vsys/cur}
[ $# -ge 3 ] && shift 3 || shift $#
case "$cmd" in
build)
    src=$(sed -n '/^SRC="/,/"$/p' "$root/scripts/run_fb_sim.sh" | sed 's/^SRC="//; s/"$//' |
          tr ' ' '\n' | grep '^rtl/' | sed "s#^#$root/#" | tr '\n' ' ')
    [ -n "$src" ] || { echo "нет SRC в $root/scripts/run_fb_sim.sh"; exit 1; }
    sdf=""
    [ -f "$root/rtl/sd_bridge.sv" ] && sdf="-DVSYS_SD"      # ядро с мостом SDRAM (SD_FAST)
    mkdir -p "$out"
    rm -rf "$out/obj"
    "$VB" --cc -O3 --x-assign fast --x-initial fast --noassert -Wno-fatal -Wno-lint -Wno-style \
        -Wno-MULTIDRIVEN -DSIMULATION $sdf -I"$root/rtl" --top-module vsys_top --Mdir "$out/obj" "$@" \
        $src tb/vsys/sdram_cyc.sv tb/vsys/vsys_top.sv
    "$CXX" -O2 -std=c++20 -w -D_CRT_SECURE_NO_WARNINGS -I"$out/obj" -I"$VR/include" -I"$VR/include/vltstd" \
        "$out"/obj/*.cpp "$VR/include/verilated.cpp" "$VR/include/verilated_threads.cpp" \
        tb/vsys/vsys_main.cpp -o "$out/vsys.exe"
    echo "собран $out/vsys.exe ($root)"
    ;;
*)
    echo "команда: build"; exit 2 ;;
esac
