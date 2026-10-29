# Синтез и раскладка ts_fb_hdmi_top в Gowin (gw_sh). Запуск из fpga/vdac2:
#   gw_sh scripts/gowin_fb.tcl          — слот 0: impl/pnr/ts_fb_hdmi_top.fs
#   gw_sh scripts/gowin_fb_slot1.tcl    — слот 1 (ядро FT812): impl/pnr/ts_fb_slot1.fs
#   gw_sh scripts/gowin_fb_video.tcl    — ядро видео (MJPEG FTVIEW4): impl/pnr/ts_fb_video.fs
#
# Multiboot (UG290 7.5.4): при включении ПЛИС всегда грузит битстрим с адреса 0 flash Tang;
# импульс RECONFIG_N (регистр BOOT) грузит битстрим с адреса перехода из заголовка текущего.
# Слоты — по 1 МБ: слот 0 переходит в слот 1 (0x100000), слот 1 — обратно в слот 0.
# Переменные до source:
#   boot_slot    — 0 или 1;
#   core_kind    — both (общее ядро, до 1.11), evo (TS-Config без FT812), ft (FT812 без Evo),
#                  video (ядро видео: декодер MJPEG вместо FT812, без Evo; переход — в слот 0);
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
set has_video 0
switch $core_kind {
    both  { set has_evo 1; set has_ft 1 }
    evo   { set has_evo 1; set has_ft 0 }
    ft    { set has_evo 0; set has_ft 1 }
    video { set has_evo 0; set has_ft 0; set has_video 1 }
    default { error "core_kind: both, evo, ft или video" }
}

set_device GW2AR-LV18QN88C8/I7 -name GW2AR-18C -device_version C
foreach f {ts_fb_hdmi_top fb_clocking ts_clocking ts_fb_core ts_mux_capture raster_detect active_bbox fb_capture
           async_fifo sdram_burst sd_bridge fb_core fb_avg_rom fb_line_ram fb_scanout fb_pattern fb_cdc fb_ui fb_ovl fb_link fb_flash fb_sdspi
           ft_ram ft_ramg ft_spi ft_core ft_port_arb cp_spi cp_link ft_mul9 ft_rd_walk ft_rd_exec ft_rd_pix ft_render ws_dark
           legacy_palette tmds_encoder hdmi_tx tmds_serializer_gowin vp_core vp_avi vp_pace vp_fbwrite vp_dbk_lp vp_noise_rom} {
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
        incr n [regsub {parameter bit         HAS_VIDEO = 1'b0} $text "parameter bit         HAS_VIDEO = 1'b$has_video" text]
        if {$n != 5} { error "ts_fb_hdmi_top.sv: параметры BOOT_*/HAS_* не найдены ($n из 5)" }
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
# декодер JPEG ядра видео (third_party/core_jpeg, Verilog 2001)
if {$has_video} {
    foreach f [lsort [glob third_party/core_jpeg/src_v/*.v]] { add_file $f }
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
        # Ядро FT812 (с 1.38): SDRAM на clk_sd = 2 × clk_mem, оба — от второй PLL (fb_pll_sd, кварц):
        # CLKOUT 27 × 9 / 2 = 121,5 МГц и CLKOUTD ÷2; домен с clk_mem общий, с пикселями — нет
        # (у ядра видео SDRAM на clk_mem первой PLL — clk_sd нет)
        if {!$has_video} {
            set line [string map {{-group [get_clocks {clk_mem}]} {-group [get_clocks {clk_mem clk_sd}]}} $line]
            # вывод 1366×768 (с 1.41): TMDS 27 × 40 / 3 = 360 МГц, пиксели ÷5 = 72 МГц
            set line [string map {{-multiply_by 55 -divide_by 4} {-multiply_by 40 -divide_by 3}} $line]
            if {[string match "*-name clk_mem *" $line]} {
                append sdc "create_generated_clock -name clk_sd -source \[get_ports {clk27}\] -multiply_by 9 -divide_by 2 \[get_pins {g_pll_sd.pll_sd/pll/CLKOUT}\]\n"
                append sdc "create_generated_clock -name clk_mem -source \[get_pins {g_pll_sd.pll_sd/pll/CLKOUT}\] -divide_by 2 \[get_pins {g_pll_sd.pll_sd/pll/CLKOUTD}\]\n"
                continue
            }
        }
        append sdc $line "\n"
    }
    close $in
    # худшие пути по доменам отдельно (своя команда в SDC заменяет общий отчёт Gowin, а в
    # общем списке были бы одни пути clk_mem)
    foreach ck [expr {$has_video ? {clk_mem pixel_clk} : {clk_mem clk_sd pixel_clk}}] {
        append sdc "report_timing -setup -max_paths 20 -max_common_paths 1 -from_clock \[get_clocks {$ck}\] -to_clock \[get_clocks {$ck}\]\n"
    }
    # ядро FT812: пути между clk_sd и clk_mem (у буфера плитки с 1.40 — полтакта clk_mem)
    if {!$has_video} {
        foreach {a b} {clk_sd clk_mem clk_mem clk_sd} {
            append sdc "report_timing -setup -max_paths 20 -max_common_paths 1 -from_clock \[get_clocks {$a}\] -to_clock \[get_clocks {$b}\]\n"
        }
    }
    file mkdir impl/sdc_ft
    set out [open impl/sdc_ft/ts_fb_hdmi_top.sdc w]
    fconfigure $out -encoding utf-8
    puts -nonewline $out $sdc
    close $out
    add_file impl/sdc_ft/ts_fb_hdmi_top.sdc
}
if {$has_video} {
    # ядро видео грузится не из своего слота (BL616 по JTAG или слот 3+), переход — в слот 0
    set_option -output_base_name ts_fb_video
    # метка VDAC4 для BL616 (docs/next-core.md): «VD4» и личность 05 — ядро видео
    set_option -user_code 0x56443405
    set_option -multiboot_spi_flash_address 0x00000000
} elseif {$boot_slot == 0} {
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
