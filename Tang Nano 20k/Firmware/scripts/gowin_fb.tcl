# Синтез и раскладка ts_fb_hdmi_top в Gowin (gw_sh). Запуск из fpga/vdac2:
#   gw_sh scripts/gowin_fb.tcl          — слот 0: impl/pnr/ts_fb_hdmi_top.fs
#   gw_sh scripts/gowin_fb_slot1.tcl    — слот 1 (ядро FT812): impl/pnr/ts_fb_slot1.fs
#
# Multiboot (UG290 7.5.4): при включении ПЛИС всегда грузит битстрим с адреса 0 flash Tang;
# импульс RECONFIG_N (регистр BOOT) грузит битстрим с адреса перехода из заголовка текущего.
# Слоты — по 1 МБ: слот 0 переходит в слот 1 (0x100000), слот 1 — обратно в слот 0.
# Переменные до source:
#   boot_slot    — 0 или 1;
#   core_kind    — both (общее ядро, до 1.11), evo (TS-Config без FT812), ft (FT812 без Evo);
#   place_option — размещение Gowin (0…2), по умолчанию 2;
#   route_option — разводка Gowin (0…2), по умолчанию 2;
#   loading_rate — частота MCLK загрузки, МГц: «25.000», «41.667», «62.500»; пусто — по
#                  умолчанию Gowin (2,5 МГц, ~2,9 с). С 1.22 оба слота — 41,667 МГц (~0,2 с):
#                  слот 1 так грузится на плате с 1.18; слот 0 — ещё и возврат из ядра FT812.
if {![info exists boot_slot]} { set boot_slot 0 }
if {![info exists core_kind]} { set core_kind both }
if {![info exists loading_rate]} { set loading_rate "41.667" }
# Размещение: 2 по умолчанию; ядро на пределе, и при неразведённых цепях помогает 0
if {![info exists place_option]} { set place_option 2 }
if {![info exists route_option]} { set route_option 2 }
switch $core_kind {
    both { set has_evo 1; set has_ft 1 }
    evo  { set has_evo 1; set has_ft 0 }
    ft   { set has_evo 0; set has_ft 1 }
    default { error "core_kind: both, evo или ft" }
}

set_device GW2AR-LV18QN88C8/I7 -name GW2AR-18C -device_version C
foreach f {ts_fb_hdmi_top fb_clocking ts_clocking ts_fb_core ts_mux_capture raster_detect active_bbox fb_capture
           async_fifo sdram_burst sd_bridge fb_core fb_avg_rom fb_line_ram fb_scanout fb_pattern fb_cdc fb_ui fb_ovl fb_link fb_flash fb_sdspi
           ft_ram ft_ramg ft_spi ft_core ft_port_arb cp_spi cp_link ft_mul9 ft_rd_walk ft_rd_exec ft_rd_pix ft_render ws_dark
           legacy_palette tmds_encoder hdmi_tx tmds_serializer_gowin} {
    if {$f eq "ts_fb_hdmi_top" && ($boot_slot != 0 || $core_kind ne "both")} {
        # Копия верхнего модуля с другими значениями параметров (параметров верхнего модуля
        # gw_sh не задаёт); имена и иерархия те же — .cst и .sdc подходят как есть
        set in [open rtl/ts_fb_hdmi_top.sv r]
        fconfigure $in -encoding utf-8
        set text [read $in]
        close $in
        set next [expr {$boot_slot == 0 ? 1 : 0}]
        set n 0
        incr n [regsub {parameter logic \[7:0\] BOOT_SLOT = 8'd0} $text "parameter logic \[7:0\] BOOT_SLOT = 8'd$boot_slot" text]
        incr n [regsub {parameter logic \[7:0\] BOOT_NEXT = 8'd1} $text "parameter logic \[7:0\] BOOT_NEXT = 8'd$next" text]
        incr n [regsub {parameter bit         HAS_EVO   = 1'b1} $text "parameter bit         HAS_EVO   = 1'b$has_evo" text]
        incr n [regsub {parameter bit         HAS_FT    = 1'b1} $text "parameter bit         HAS_FT    = 1'b$has_ft" text]
        if {$n != 4} { error "ts_fb_hdmi_top.sv: параметры BOOT_*/HAS_* не найдены ($n из 4)" }
        set dir impl/top_slot${boot_slot}_$core_kind
        file mkdir $dir
        set out [open $dir/ts_fb_hdmi_top.sv w]
        fconfigure $out -encoding utf-8
        puts -nonewline $out $text
        close $out
        add_file $dir/ts_fb_hdmi_top.sv
    } else {
        add_file rtl/$f.sv
    }
}
add_file constraints/ts_fb_hdmi_top.cst
if {$has_evo} {
    add_file constraints/ts_fb_hdmi_top.sdc
} else {
    # Ядро FT812: захвата Evo нет — PLL захвата синтез выбрасывает, такта clk_cap нет
    set in [open constraints/ts_fb_hdmi_top.sdc r]
    fconfigure $in -encoding utf-8
    set sdc ""
    foreach line [split [read $in] "\n"] {
        if {[string match "*-name clk_cap *" $line]} { continue }
        set line [string map {{-group [get_clocks {clk_cap}] } {}} $line]
        # SDRAM — на CLKOUTD3 = 2 × clk_mem (ts_fb_core SD_FAST): тот же PLL, домен с clk_mem общий
        set line [string map {{-group [get_clocks {clk_mem}]} {-group [get_clocks {clk_mem clk_sd}]}} $line]
        append sdc $line "\n"
        if {[string match "*-name clk_mem *" $line]} {
            append sdc "create_generated_clock -name clk_sd -source \[get_pins {pll_xtal/pll/CLKOUT}\] -divide_by 3 \[get_pins {pll_xtal/pll/CLKOUTD3}\]\n"
        }
    }
    close $in
    # худшие пути по доменам отдельно (своя команда в SDC заменяет общий отчёт Gowin, а в
    # общем списке были бы одни пути clk_mem)
    foreach ck {clk_mem clk_sd pixel_clk} {
        append sdc "report_timing -setup -max_paths 20 -max_common_paths 1 -from_clock \[get_clocks {$ck}\] -to_clock \[get_clocks {$ck}\]\n"
    }
    file mkdir impl/sdc_ft
    set out [open impl/sdc_ft/ts_fb_hdmi_top.sdc w]
    fconfigure $out -encoding utf-8
    puts -nonewline $out $sdc
    close $out
    add_file impl/sdc_ft/ts_fb_hdmi_top.sdc
}
if {$boot_slot == 0} {
    set_option -output_base_name ts_fb_hdmi_top
    set_option -multiboot_spi_flash_address 0x00100000
} else {
    set_option -output_base_name ts_fb_slot$boot_slot
    set_option -multiboot_spi_flash_address 0x00000000
}
set_option -multi_boot 1
if {$loading_rate ne ""} { set_option -loading_rate $loading_rate }
set_option -top_module ts_fb_hdmi_top
set_option -verilog_std sysv2017
set_option -include_path {rtl}
set_option -synthesis_tool gowinsynthesis
set_option -use_sspi_as_gpio 1
set_option -use_mspi_as_gpio 1
set_option -use_ready_as_gpio 1
set_option -use_done_as_gpio 1
# RECONFIG_N (FPGA9) ведёт само ядро: регистр BOOT тянет его к 0
set_option -use_reconfign_as_gpio 1
set_option -gen_text_timing_rpt 1
set_option -ioreg_in_iob 1
# Общее ядро на пределе по площади (CLS 97 %): размещение 2 и трассировка 2 (с упором на
# разводимость); с 1/1 ядро 1.10 не разводится (91 цепь), с 2/2 — разведено (2026-10-18)
set_option -place_option $place_option
set_option -route_option $route_option
run all
