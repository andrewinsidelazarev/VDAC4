#!/bin/sh
# Проверка кадрового буфера (ts_fb_core): растр Evo → автоопределение → SDRAM → 576p50 → TMDS.
#   sh scripts/run_fb_sim.sh          — сценарии в уменьшенном растре (~2 мин)
#   sh scripts/run_fb_sim.sh full     — плюс настоящие растры с картинкой (десятки минут)
#   sh scripts/run_fb_sim.sh osd      — только кадр 576p с испытательной таблицей → sim/osd/*.ppm
#                                       (меню рисует ESP в оверлей; оверлеи — в сценариях таблицы)
#   sh scripts/run_fb_sim.sh hd [A [S [avg]]] — только настоящий растр Evo → 1280×720p50; A: 0 — 4:3,
#                                       1 — 16:9; S: 0 — 100 %, 1 — 95 %, 2 — 90 % высоты (~25 мин);
#                                       avg — с FRAME BLEND (ядро без FT812)
#   sh scripts/run_fb_sim.sh flash    — только fb_flash: настройки во flash Tang и мост (секунды)
#   sh scripts/run_fb_sim.sh sd       — только мост к microSD (fb_sdspi) с моделью карты (секунды)
#   sh scripts/run_fb_sim.sh ft       — только FT812: обмен заставки Weather 2 с bt8xxemu (~30 с;
#                                       нужен sim/ft/weather2_spi.txt — запись обмена, см. tb_ft_weather2)
#                                       и RAM_G в SDRAM против эталона (~20 с), смена листа
#   sh scripts/run_fb_sim.sh cp       — только связь с BL616 (сопроцессор): SPI 26,67 / 30 / 13,3 МГц
#   sh scripts/run_fb_sim.sh ovl      — только оверлеи: холст меню окном LINK → SDRAM (секунды)
# Симулятор: Icarus Verilog 13 (IVERILOG/VVP или в PATH). Установка без прав администратора:
#   py -3 tools/bootstrap_a3_rtl_tools.py --directory E:/zx/tools/rtl --package iverilog --env ucrt64
set -e
cd "$(dirname "$0")/.."
IV=${IVERILOG:-iverilog}
VVP=${VVP:-vvp}
SRC="rtl/ts_fb_core.sv rtl/ts_mux_capture.sv rtl/raster_detect.sv rtl/active_bbox.sv rtl/fb_capture.sv
     rtl/async_fifo.sv rtl/sdram_burst.sv rtl/fb_core.sv rtl/fb_avg_rom.sv rtl/fb_line_ram.sv rtl/fb_scanout.sv
     rtl/fb_pattern.sv rtl/fb_cdc.sv rtl/fb_ui.sv rtl/fb_ovl.sv rtl/fb_link.sv
     rtl/fb_flash.sv rtl/fb_sdspi.sv rtl/sd_bridge.sv rtl/ft_ram.sv rtl/ft_ramg.sv rtl/ft_spi.sv rtl/ft_core.sv rtl/ft_port_arb.sv rtl/cp_spi.sv rtl/cp_link.sv rtl/ft_rd_walk.sv rtl/ws_dark.sv
     rtl/ft_mul9.sv rtl/ft_rd_exec.sv rtl/ft_rd_pix.sv rtl/ft_render.sv rtl/legacy_palette.sv rtl/tmds_encoder.sv rtl/hdmi_tx.sv tb/sdram_model.sv
     tb/spi_flash_model.sv tb/sd_card_model.sv tb/tb_ts_fb.sv"

run() {
    name=$1; shift
    echo "=== $name"
    "$IV" -g2012 -DSIMULATION -I rtl -s tb_ts_fb "$@" -o tb_ts_fb.vvp $SRC
    "$VVP" -n tb_ts_fb.vvp | grep -v '^tb/'
}

osd() {
    mkdir -p sim/osd
    echo "=== 576p: испытательная таблица без Evo → sim/osd/pat_out_*.ppm"
    "$IV" -g2012 -DSIMULATION -I rtl -s tb_ts_fb -Ptb_ts_fb.MINI=0 -Ptb_ts_fb.PATTERN=1 \
        -Ptb_ts_fb.CAL=0 -Ptb_ts_fb.OUT_FRAMES=5 -o sim/osd/t.vvp $SRC
    (cd sim/osd && "$VVP" -n t.vvp +ppm | grep -v '^tb/')
}

hd() {
    aspect=${1:-0}
    size=${2:-0}
    dir=sim/hd$aspect$size
    extra=""
    if [ "$3" = "avg" ]; then
        # FRAME BLEND: ядро без FT812, строка пары читается дважды — запас по времени строки
        dir=${dir}avg
        extra="-Ptb_ts_fb.HAS_FT=0 -Ptb_ts_fb.AVG_TEST=1"
        echo "=== (FRAME BLEND, ядро TS-Config без FT812)"
    fi
    mkdir -p $dir
    pct=$([ "$size" = 1 ] && echo 95 || ([ "$size" = 2 ] && echo 90 || echo 100))
    if [ "$aspect" = 1 ]; then
        echo "=== 1280×720p50, 16:9, $pct %: TS-Config Pentagon по центру"
    else
        echo "=== 1280×720p50, 4:3, $pct %: TS-Config Pentagon по центру, поля чёрные"
    fi
    "$IV" -g2012 -DSIMULATION -I rtl -s tb_ts_fb -Ptb_ts_fb.MINI=0 -Ptb_ts_fb.HD=1 -Ptb_ts_fb.RASTER=0 \
        -Ptb_ts_fb.ASPECT=$aspect -Ptb_ts_fb.SIZE=$size -Ptb_ts_fb.OUT_FRAMES=8 $extra -o $dir/t.vvp $SRC
    (cd $dir && "$VVP" -n t.vvp | grep -v '^tb/')
}

flash() {
    mkdir -p sim/flash
    "$IV" -g2012 -DSIMULATION -I rtl -s tb_fb_flash -o sim/flash/t.vvp \
        rtl/fb_flash.sv tb/spi_flash_model.sv tb/tb_fb_flash.sv
    (cd sim/flash && "$VVP" -n t.vvp | grep -v '^tb/')
}

sd() {
    mkdir -p sim/sdspi
    echo "=== мост к microSD: инициализация, чтение и запись блока, маркеры, таймауты"
    "$IV" -g2012 -DSIMULATION -s tb_fb_sdspi -o sim/sdspi/t.vvp rtl/fb_sdspi.sv tb/sd_card_model.sv tb/tb_fb_sdspi.sv
    (cd sim/sdspi && "$VVP" -n t.vvp | grep -v '^tb/')
}

if [ "$1" = "osd" ]; then osd; exit 0; fi
if [ "$1" = "sd" ]; then sd; exit 0; fi
if [ "$1" = "hd" ]; then hd "$2" "$3" "$4"; exit 0; fi
ft() {
    [ -f sim/ft/weather2_spi.txt ] || { echo "нет sim/ft/weather2_spi.txt"; exit 1; }
    echo "=== FT812: обмен Weather 2 против bt8xxemu"
    FT_SRC="rtl/ft_ram.sv rtl/ft_ramg.sv rtl/ft_spi.sv rtl/ft_core.sv rtl/sdram_burst.sv tb/sdram_model.sv"
    "$IV" -g2012 -DSIMULATION -s tb_ft_weather2 -o sim/ft/t.vvp $FT_SRC tb/tb_ft_weather2.sv
    (cd sim/ft && "$VVP" -n t.vvp +spi=weather2_spi.txt | grep -v '^tb/')
    mkdir -p sim/ftg
    echo "=== FT812: RAM_G в SDRAM — SPI и порт слов против эталона"
    "$IV" -g2012 -DSIMULATION -s tb_ft_ramg -o sim/ftg/t.vvp $FT_SRC tb/tb_ft_ramg.sv
    (cd sim/ftg && "$VVP" -n t.vvp | grep -v '^tb/')
    echo "=== FT812: RAM_G под нагрузкой SDRAM (второй клиент пачками), мелкие записи на 28 МГц"
    "$IV" -g2012 -DSIMULATION -s tb_ft_ramg -Ptb_ft_ramg.HOG=1 -o sim/ftg/hog.vvp $FT_SRC tb/tb_ft_ramg.sv
    (cd sim/ftg && "$VVP" -n hog.vvp | grep -v '^tb/')
    echo "=== FT812: смена листа при занятом растеризаторе (swap_hold)"
    "$IV" -g2012 -DSIMULATION -s tb_ft_swap -o sim/ftg/swap.vvp $FT_SRC tb/tb_ft_swap.sv
    (cd sim/ftg && "$VVP" -n swap.vvp | grep -v '^tb/')
    echo "=== FT812: то же ядром слота 1 (START_ACTIVE): REG_ID без ACTIVE"
    "$IV" -g2012 -DSIMULATION -s tb_ft_swap -Ptb_ft_swap.START_ACTIVE=1 -o sim/ftg/swap1.vvp $FT_SRC tb/tb_ft_swap.sv
    (cd sim/ftg && "$VVP" -n swap1.vvp | grep -v '^tb/')
}

ovl() {
    mkdir -p sim/ovl
    echo "=== оверлеи: холст меню 336×136 окном LINK → SDRAM, слово в слово"
    for gp in "22 400" "22 0" "8 30"; do
        set -- $gp
        "$IV" -g2012 -DSIMULATION -s tb_fb_ovl -Ptb_fb_ovl.GAP=$1 -Ptb_fb_ovl.PAUSE=$2 -o sim/ovl/t.vvp \
            rtl/fb_ovl.sv rtl/ft_mul9.sv rtl/async_fifo.sv rtl/sdram_burst.sv tb/sdram_model.sv tb/tb_fb_ovl.sv
        echo "--- слово раз в $1 тактов, пауза между посылками $2"
        (cd sim/ovl && "$VVP" -n t.vvp | grep -v '^tb/')
    done
}

cp() {
    mkdir -p sim/cp
    echo "=== связь с BL616: SPI → очередь → ft_core, буфер чтения, STATUS, IRQ"
    "$IV" -g2012 -DSIMULATION -s tb_cp_link -o sim/cp/t.vvp rtl/ft_ram.sv rtl/ft_ramg.sv         rtl/ft_spi.sv rtl/ft_core.sv rtl/ft_port_arb.sv rtl/cp_spi.sv rtl/cp_link.sv         rtl/sdram_burst.sv tb/sdram_model.sv tb/tb_cp_link.sv
    for ns in 37.5 33.33 75; do
        echo "--- SCK $ns нс"
        (cd sim/cp && "$VVP" -n t.vvp +sck_ns=$ns | grep -v '^tb/')
    done
}

ws() {
    mkdir -p sim/ws
    echo "=== светодиод WS2812 модуля Tang (вывод MISO): чёрный при шуме чтений Evo (R-Type), MISO при выборе"
    for r in 280000.0 50000.0; do               # порог сброса WS2812B и WS2812
        "$IV" -g2012 -s tb_ws_dark -Ptb_ws_dark.RST_NS=$r -o sim/ws/t.vvp rtl/ws_dark.sv tb/tb_ws_dark.sv
        (cd sim/ws && "$VVP" -n t.vvp | grep -v '^tb/')
    done
}

if [ "$1" = "flash" ]; then flash; exit 0; fi
if [ "$1" = "ft" ]; then ft; exit 0; fi
if [ "$1" = "cp" ]; then cp; exit 0; fi
if [ "$1" = "ovl" ]; then ovl; exit 0; fi
if [ "$1" = "ws" ]; then ws; exit 0; fi

flash
sd
ft
cp
ovl
ws

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
# Раздельные ядра multiboot (ts_fb_core HAS_EVO / HAS_FT)
run "ядро TS-Config без FT812: растр A" -Ptb_ts_fb.HAS_FT=0
run "ядро TS-Config без FT812: таблица, LINK; автопереход в слот FT812 по ACTIVE после ARM1, команды хоста — нет" \
    -Ptb_ts_fb.PATTERN=1 -Ptb_ts_fb.HAS_FT=0 -Ptb_ts_fb.SWITCH_TEST=1
run "ядро TS-Config без FT812: автопереход по чтению REG_ID" \
    -Ptb_ts_fb.PATTERN=1 -Ptb_ts_fb.HAS_FT=0 -Ptb_ts_fb.SWITCH_TEST=1 -Ptb_ts_fb.SW_READ=1
run "ядро FT812 без Evo: таблица, LINK, оверлеи; автовозврат после тишины при M_SEL = 0" \
    -Ptb_ts_fb.PATTERN=1 -Ptb_ts_fb.HAS_EVO=0 -Ptb_ts_fb.SWITCH_TEST=1
# FRAME BLEND (1.32, ядро TS-Config без FT812): среднее двух кадров, настройка — из flash Tang
run "FRAME BLEND: среднее пары кадров (A/B через один — ровный цвет)" -Ptb_ts_fb.HAS_FT=0 -Ptb_ts_fb.AVG_TEST=1
run "FRAME BLEND: вход быстрее на 11 % — два кадра за выходной, пара без свободного буфера" \
    -Ptb_ts_fb.HAS_FT=0 -Ptb_ts_fb.AVG_TEST=1 \
    -Ptb_ts_fb.RASTER=7 -Ptb_ts_fb.SRC_H_TOTAL=84 -Ptb_ts_fb.SRC_V_TOTAL=39 -Ptb_ts_fb.OUT_FRAMES=60
run "FRAME BLEND: PCLK пропадает посреди кадра" -Ptb_ts_fb.HAS_FT=0 -Ptb_ts_fb.AVG_TEST=1 -Ptb_ts_fb.STOP_AFTER=15

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
