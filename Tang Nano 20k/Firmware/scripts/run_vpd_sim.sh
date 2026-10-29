#!/bin/sh
# Стенд Verilator деблокера записи кадра: декодер JPEG → vp_fbwrite (DEBLOCK 0, 1, 2) → модель
# SDRAM (tb/jpeg/vp_tb_top.sv, tb/jpeg/tb_vp.cpp) и эталонный декодер tb_jpeg.exe; Д — DEBLOCK,
# Т — DITHER (варианты 00, 11, 21, 20).
#
#   scripts/run_vpd_sim.sh [КАТАЛОГ]  — собрать КАТАЛОГ/jpeg/tb_jpeg.exe и КАТАЛОГ/vpДТ/tb_vp.exe
#                                       (по умолчанию E:/zx/cache/tmp/deblock)
set -e
VR=${VERILATOR_ROOT:-E:/zx/tools/rtl/ucrt64/share/verilator}
VB=${VERILATOR_BIN:-/e/zx/tools/rtl/ucrt64/bin/verilator_bin.exe}
CXX=${CXX:-/e/zx/llvm/bin/clang++}
export VERILATOR_ROOT="$VR"
export TMP=E:/zx/cache/tmp TEMP=E:/zx/cache/tmp

top=${1:-E:/zx/cache/tmp/deblock}
src=third_party/core_jpeg/src_v

build_jpeg() {
    out=$top/jpeg
    mkdir -p "$out"; rm -rf "$out/obj"
    "$VB" --cc -O3 --x-assign fast --x-initial fast --noassert -Wno-fatal -Wno-lint -Wno-style \
        -I"$src" --top-module jpeg_core --Mdir "$out/obj" -GSUPPORT_WRITABLE_DHT=1 "$src"/*.v
    "$CXX" -O2 -std=c++20 -w -D_CRT_SECURE_NO_WARNINGS -I"$out/obj" -I"$VR/include" -I"$VR/include/vltstd" \
        "$out"/obj/*.cpp "$VR/include/verilated.cpp" "$VR/include/verilated_threads.cpp" \
        tb/jpeg/tb_jpeg.cpp -o "$out/tb_jpeg.exe"
    echo "собран $out/tb_jpeg.exe"
}

build_vp() {
    d=$1; t=$2; h=${3:-1}; out=$top/vp$d$t$([ "$h" = 0 ] && echo h0 || true)
    mkdir -p "$out"; rm -rf "$out/obj"
    "$VB" --cc -O3 --x-assign fast --x-initial fast --noassert -Wno-fatal -Wno-lint -Wno-style         -I"$src" -Irtl --top-module vp_tb_top --Mdir "$out/obj" -GDEBLOCK=$d -GDITHER=$t -GDB_HALF=$h         "$src"/*.v rtl/vp_dbk_lp.sv rtl/vp_noise_rom.sv rtl/vp_fbwrite.sv tb/jpeg/vp_tb_top.sv
    "$CXX" -O2 -std=c++20 -w -D_CRT_SECURE_NO_WARNINGS -DDEBLOCK=$d -DDITHER=$t -DDB_HALF=$h -I"$out/obj" -I"$VR/include" -I"$VR/include/vltstd"         "$out"/obj/*.cpp "$VR/include/verilated.cpp" "$VR/include/verilated_threads.cpp"         tb/jpeg/tb_vp.cpp -o "$out/tb_vp.exe"
    echo "собран $out/tb_vp.exe"
}

[ -x "$top/jpeg/tb_jpeg.exe" ] || build_jpeg > "$top/build_jpeg.log" 2>&1 &
for v in "0 0" "1 1" "2 1" "2 0" "2 1 0"; do
    set -- $v
    build_vp $1 $2 $3 > "$top/build_vp$1$2$3.log" 2>&1 &
done
wait
tail -n 2 "$top"/build_*.log
