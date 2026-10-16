#!/bin/sh
# Проверка кадрового буфера (ts_fb_core): растр Evo → автоопределение → SDRAM → 576p50 → TMDS.
#   sh scripts/run_fb_sim.sh          — сценарии в уменьшенном растре (~2 мин)
#   sh scripts/run_fb_sim.sh full     — плюс настоящие растры с картинкой (десятки минут)
#   sh scripts/run_fb_sim.sh osd      — только кадр 576p с экранным меню → sim/osd/*.ppm
#   sh scripts/run_fb_sim.sh hd [A [S]] — только настоящий растр Evo → 1280×720p50; A: 0 — 4:3,
#                                       1 — 16:9; S: 0 — 100 %, 1 — 95 %, 2 — 90 % высоты (~25 мин)
#   sh scripts/run_fb_sim.sh flash    — только fb_flash: настройки во flash Tang и мост (секунды)
# Симулятор: Icarus Verilog 13 (IVERILOG/VVP или в PATH). Установка без прав администратора:
#   py -3 tools/bootstrap_a3_rtl_tools.py --directory E:/zx/tools/rtl --package iverilog --env ucrt64
set -e
cd "$(dirname "$0")/.."
IV=${IVERILOG:-iverilog}
VVP=${VVP:-vvp}
SRC="rtl/ts_fb_core.sv rtl/ts_mux_capture.sv rtl/raster_detect.sv rtl/active_bbox.sv rtl/fb_capture.sv
     rtl/async_fifo.sv rtl/sdram_burst.sv rtl/fb_core.sv rtl/fb_line_ram.sv rtl/fb_scanout.sv
     rtl/fb_pattern.sv rtl/fb_cdc.sv rtl/fb_ui.sv rtl/fb_osd.sv rtl/fb_osd_rom.sv rtl/fb_link.sv
     rtl/fb_flash.sv rtl/legacy_palette.sv rtl/tmds_encoder.sv rtl/hdmi_tx.sv tb/sdram_model.sv
     tb/spi_flash_model.sv tb/tb_ts_fb.sv"

run() {
    name=$1; shift
    echo "=== $name"
    "$IV" -g2012 -DSIMULATION -I rtl -s tb_ts_fb "$@" -o tb_ts_fb.vvp $SRC
    "$VVP" -n tb_ts_fb.vvp | grep -v '^tb/'
}

osd() {
    mkdir -p sim/osd
    echo "=== 576p: испытательная таблица без Evo и экранное меню → sim/osd/pat_out_*.ppm"
    "$IV" -g2012 -DSIMULATION -I rtl -s tb_ts_fb -Ptb_ts_fb.MINI=0 -Ptb_ts_fb.PATTERN=1 \
        -Ptb_ts_fb.CAL=0 -Ptb_ts_fb.OSD=1 -Ptb_ts_fb.OUT_FRAMES=5 -o sim/osd/t.vvp $SRC
    (cd sim/osd && "$VVP" -n t.vvp +ppm | grep -v '^tb/')
}

hd() {
    aspect=${1:-0}
    size=${2:-0}
    dir=sim/hd$aspect$size
    mkdir -p $dir
    pct=$([ "$size" = 1 ] && echo 95 || ([ "$size" = 2 ] && echo 90 || echo 100))
    if [ "$aspect" = 1 ]; then
        echo "=== 1280×720p50, 16:9, $pct %: TS-Config Pentagon по центру"
    else
        echo "=== 1280×720p50, 4:3, $pct %: TS-Config Pentagon по центру, поля чёрные"
    fi
    "$IV" -g2012 -DSIMULATION -I rtl -s tb_ts_fb -Ptb_ts_fb.MINI=0 -Ptb_ts_fb.HD=1 -Ptb_ts_fb.RASTER=0 \
        -Ptb_ts_fb.ASPECT=$aspect -Ptb_ts_fb.SIZE=$size -Ptb_ts_fb.OUT_FRAMES=8 -o $dir/t.vvp $SRC
    (cd $dir && "$VVP" -n t.vvp | grep -v '^tb/')
}

flash() {
    mkdir -p sim/flash
    "$IV" -g2012 -DSIMULATION -I rtl -s tb_fb_flash -o sim/flash/t.vvp \
        rtl/fb_flash.sv tb/spi_flash_model.sv tb/tb_fb_flash.sv
    (cd sim/flash && "$VVP" -n t.vvp | grep -v '^tb/')
}

if [ "$1" = "osd" ]; then osd; exit 0; fi
if [ "$1" = "hd" ]; then hd "$2" "$3"; exit 0; fi
if [ "$1" = "flash" ]; then flash; exit 0; fi

flash

run "растр A, вход медленнее выхода: повторы кадров"
run "растр B, положительные синхроимпульсы" -Ptb_ts_fb.RASTER=6
run "незнакомый растр, вход быстрее на 6 %: окно по центру, пропуски" \
    -Ptb_ts_fb.RASTER=7 -Ptb_ts_fb.SRC_H_TOTAL=86 -Ptb_ts_fb.SRC_V_TOTAL=40
run "незнакомый растр, вход быстрее на 11 %" \
    -Ptb_ts_fb.RASTER=7 -Ptb_ts_fb.SRC_H_TOTAL=84 -Ptb_ts_fb.SRC_V_TOTAL=39 -Ptb_ts_fb.OUT_FRAMES=60
run "смена растра A → B на лету" -Ptb_ts_fb.SWITCH_TO=6 -Ptb_ts_fb.SWITCH_AT=15
run "смена растра B → A на лету" -Ptb_ts_fb.RASTER=6 -Ptb_ts_fb.SWITCH_TO=5 -Ptb_ts_fb.SWITCH_AT=15
run "PCLK пропадает посреди кадра" -Ptb_ts_fb.STOP_AFTER=15
run "картинка сдвинута на +3/−2: окно подстраивается по границам" -Ptb_ts_fb.SHIFT_X=3 -Ptb_ts_fb.SHIFT_Y=-2
run "растр B, картинка сдвинута на −5/+1" -Ptb_ts_fb.RASTER=6 -Ptb_ts_fb.SHIFT_X=-5 -Ptb_ts_fb.SHIFT_Y=1
run "Evo нет: таблица через SDRAM, калибровка выборки чтения" -Ptb_ts_fb.PATTERN=1
run "Evo нет: таблица, выборка 0 без калибровки" -Ptb_ts_fb.PATTERN=1 -Ptb_ts_fb.CAL=0 -Ptb_ts_fb.RD_FIXED=0
run "картинка окном в кадре вывода (чёрное поле вокруг, как в 720p)" -Ptb_ts_fb.BORDER=8
run "окно в кадре вывода: таблица и калибровка" -Ptb_ts_fb.BORDER=8 -Ptb_ts_fb.PATTERN=1

# Контроль модели: прежняя выборка (фронт E3, точка 2) с паспортными tAC/tOH должна давать ошибки
echo "=== контроль: прежняя точка выборки 2 без калибровки должна НЕ пройти"
"$IV" -g2012 -DSIMULATION -I rtl -s tb_ts_fb -Ptb_ts_fb.PATTERN=1 -Ptb_ts_fb.CAL=0 -Ptb_ts_fb.RD_FIXED=2 \
    -o tb_ts_fb.vvp $SRC
if "$VVP" -n tb_ts_fb.vvp | grep -q "ТЕСТ НЕ ПРОЙДЕН"; then
    echo "ожидаемо не пройден: модель ловит неверную точку выборки"
else
    echo "ОШИБКА: неверная точка выборки не обнаружена"; exit 1
fi

if [ "$1" = "full" ]; then
    # Картинка: экран ZX в бордюре, выход декодируется из TMDS и выгружается в sim/<сценарий>
    SCR=${SCR:-sim/hib.hex}
    [ -f "$SCR" ] || { echo "нет $SCR: py -3 scripts/scr2hex.py экран.scr $SCR 1 sim/hib_in.png"; exit 1; }
    for r in 0 1 2 3 4; do
        mkdir -p sim/r$r
        echo "=== настоящий растр $r (0 TS Pentagon, 1 TS PENT_312, 2 Base Pentagon, 3 Base 48k, 4 Base 128k)"
        "$IV" -g2012 -DSIMULATION -I rtl -s tb_ts_fb -Ptb_ts_fb.MINI=0 -Ptb_ts_fb.RASTER=$r \
            -Ptb_ts_fb.OUT_FRAMES=8 -o sim/r$r/t.vvp $SRC
        (cd sim/r$r && "$VVP" -n t.vvp +img=../$(basename "$SCR") +ppm | grep -v '^tb/')
    done
    # Полноэкранный TS-Config 360×288 со сдвигом: окно должно встать по краю картинки
    py -3 scripts/img2hex.py --test360 sim/t360.hex sim/t360_in.png
    mkdir -p sim/t360
    echo "=== 360×288 без бордюра, картинка сдвинута на +3/−2"
    "$IV" -g2012 -DSIMULATION -I rtl -s tb_ts_fb -Ptb_ts_fb.MINI=0 -Ptb_ts_fb.RASTER=0 \
        -Ptb_ts_fb.SHIFT_X=3 -Ptb_ts_fb.SHIFT_Y=-2 -Ptb_ts_fb.OUT_FRAMES=11 -o sim/t360/t.vvp $SRC
    (cd sim/t360 && "$VVP" -n t.vvp +img=../t360.hex +ppm | grep -v '^tb/')
    osd
    hd
fi
