#!/bin/sh
# Вывод холстов ядра видео (fb_scanout с FT_W512) против эталона развёртки: tb_ft_scan (Icarus) →
# scripts/ft_scan_check.py. С 1.39 все холсты — 4:3 (512×384, 640×480, 1024×768) и 16:9 (512×288,
# 640×360, 1024×576) — окном по ASPECT и SIZE слота 0, растянуты на него (как кадр FT812).
#
#   sh scripts/run_ft_scan_video.sh [КАТАЛОГ]   (по умолчанию E:/zx/cache/tmp/fts_video)
#
# Нужны iverilog/vvp (E:/zx/tools/rtl/ucrt64/bin) и Python с Pillow (по желанию — картинки).
set -e
out=${1:-E:/zx/cache/tmp/fts_video}
PY=${PY:-E:/zx/esp/pio-venv/Scripts/python.exe}
here=$(pwd)
mkdir -p "$out"
fail=0
for case in "512 288 0 0" "512 288 1 1" "512 288 0 2" "640 360 0 0" "640 360 1 1" "640 360 0 2" \
            "1024 576 0 0" "1024 576 0 1" "1024 576 1 2" "512 384 0 0" "512 384 1 1" "512 384 0 2"; do
    set -- $case
    d="$out/${1}x${2}_a${3}_s${4}"
    mkdir -p "$d"
    "$PY" scripts/ft_scan_frame.py "$1" "$2" "$d/ft_frame.hex"
    iverilog -g2012 -DSIMULATION -I rtl -s tb_ft_scan \
        -P tb_ft_scan.SRC_W="$1" -P tb_ft_scan.SRC_H="$2" -P tb_ft_scan.ASPECT="$3" -P tb_ft_scan.SIZE="$4" \
        -P tb_ft_scan.FT_W512=1 -o "$d/t.vvp" \
        rtl/fb_core.sv rtl/fb_line_ram.sv rtl/fb_scanout.sv rtl/legacy_palette.sv rtl/sdram_burst.sv \
        tb/sdram_model.sv tb/tb_ft_scan.sv
    (cd "$d" && vvp -n t.vvp | grep -a "ТЕСТ\|кадр выведен")
    if ! "$PY" "$here/scripts/ft_scan_check.py" "$d" "$3" "$4" "$1" video; then fail=1; fi
done
[ "$fail" = 0 ] && echo "ВСЕ ХОЛСТЫ ВЕРНЫ" || { echo "ЕСТЬ РАСХОЖДЕНИЯ"; exit 1; }
