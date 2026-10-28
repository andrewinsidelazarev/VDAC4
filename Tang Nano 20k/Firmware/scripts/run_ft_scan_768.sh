#!/bin/sh
# Вывод кадра ядра FT812 в 1366×768 (fb_scanout OUT768, с 1.41) против эталона развёртки:
# tb_ft_scan (Icarus, OUT768 = 1) → scripts/ft_scan_check.py ... 768. Кадры 1024×768 и 640×480 —
# все окна ASPECT × SIZE; случаи идут по четыре одновременно.
#
#   sh scripts/run_ft_scan_768.sh [КАТАЛОГ]   (по умолчанию E:/zx/cache/tmp/fts_768)
#
# Нужны iverilog/vvp (E:/zx/tools/rtl/ucrt64/bin) и Python с Pillow (по желанию — картинки).
set -e
out=${1:-E:/zx/cache/tmp/fts_768}
PY=${PY:-E:/zx/esp/pio-venv/Scripts/python.exe}
export TMP=E:/zx/cache/tmp TEMP=E:/zx/cache/tmp PYTHONIOENCODING=utf-8
here=$(pwd)
mkdir -p "$out"

run_case() {
    d="$out/${1}x${2}_a${3}_s${4}"
    mkdir -p "$d"
    "$PY" scripts/ft_scan_frame.py "$1" "$2" "$d/ft_frame.hex" > /dev/null
    iverilog -g2012 -DSIMULATION -I rtl -s tb_ft_scan \
        -P tb_ft_scan.SRC_W="$1" -P tb_ft_scan.SRC_H="$2" -P tb_ft_scan.ASPECT="$3" -P tb_ft_scan.SIZE="$4" \
        -P tb_ft_scan.OUT768=1 -o "$d/t.vvp" \
        rtl/fb_core.sv rtl/fb_line_ram.sv rtl/fb_scanout.sv rtl/legacy_palette.sv rtl/sdram_burst.sv \
        tb/sdram_model.sv tb/tb_ft_scan.sv
    (cd "$d" && vvp -n t.vvp > sim.txt 2>&1)
    "$PY" "$here/scripts/ft_scan_check.py" "$d" "$3" "$4" "$1" 768 > "$d/check.txt" 2>&1 || true
}

i=0
for case in "1024 768 0 0" "1024 768 0 1" "1024 768 0 2" "1024 768 1 0" "1024 768 1 1" "1024 768 1 2" \
            "640 480 0 0" "640 480 0 1" "640 480 0 2" "640 480 1 0" "640 480 1 1" "640 480 1 2"; do
    set -- $case
    ( cd "$here" && run_case "$1" "$2" "$3" "$4" ) &
    i=$((i + 1))
    [ $((i % 4)) = 0 ] && wait
done
wait

fail=0
for d in "$out"/*x*_a*_s*; do
    name=$(basename "$d")
    sim=$(grep -a "ТЕСТ\|кадр выведен" "$d/sim.txt" | tr '\n' ' ')
    chk=$(grep -a "точек расходится" "$d/check.txt" || echo "сверки нет")
    echo "$name: $sim| $chk"
    grep -aq "ТЕСТ ПРОЙДЕН" "$d/sim.txt" || fail=1
    grep -aq "точек расходится 0" "$d/check.txt" || fail=1
done
[ "$fail" = 0 ] && echo "ВСЕ ОКНА 1366×768 ВЕРНЫ" || { echo "ЕСТЬ РАСХОЖДЕНИЯ"; exit 1; }
