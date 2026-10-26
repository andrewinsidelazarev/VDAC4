# Ядро слота 1 flash Tang (0x100000), переход — обратно в слот 0 (multiboot).
# Запуск из fpga/vdac2:  gw_sh scripts/gowin_fb_slot1.tcl  → impl/pnr/ts_fb_slot1.fs
# По умолчанию — ядро FT812 без захвата Evo (core_kind ft); core_kind both — проверочный
# образ ядра 1.11 (то же общее ядро, BOOT читается как слот 1).
set boot_slot 1
if {![info exists core_kind]} { set core_kind ft }
# Общее ядро 1.11 в слоте 1 разводилось только с размещением 0
if {![info exists place_option]} { set place_option [expr {$core_kind eq "both" ? 0 : 2}] }
# Ядро FT812 грузится по первому запуску чипа программой Evo, и та ждёт FT812 недолго (просмотрщик
# WC — сразу «VDAC2 не найден»): MCLK загрузки 41,667 МГц — ~0,2 с вместо ~2,9 с (1.18)
if {![info exists loading_rate] && $core_kind eq "ft"} { set loading_rate "41.667" }
source scripts/gowin_fb.tcl
