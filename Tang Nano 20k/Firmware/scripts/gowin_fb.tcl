# Синтез и раскладка ts_fb_hdmi_top в Gowin (gw_sh). Запуск из fpga/vdac2:
#   gw_sh scripts/gowin_fb.tcl
set_device GW2AR-LV18QN88C8/I7 -name GW2AR-18C -device_version C
foreach f {ts_fb_hdmi_top fb_clocking ts_clocking ts_fb_core ts_mux_capture raster_detect active_bbox fb_capture
           async_fifo sdram_burst fb_core fb_line_ram fb_scanout fb_pattern fb_cdc fb_ui fb_osd fb_osd_rom fb_link fb_flash
           legacy_palette tmds_encoder hdmi_tx tmds_serializer_gowin} {
    add_file rtl/$f.sv
}
add_file constraints/ts_fb_hdmi_top.cst
add_file constraints/ts_fb_hdmi_top.sdc
set_option -output_base_name ts_fb_hdmi_top
set_option -top_module ts_fb_hdmi_top
set_option -verilog_std sysv2017
set_option -include_path {rtl}
set_option -synthesis_tool gowinsynthesis
set_option -use_sspi_as_gpio 1
set_option -use_mspi_as_gpio 1
set_option -use_ready_as_gpio 1
set_option -use_done_as_gpio 1
set_option -gen_text_timing_rpt 1
set_option -ioreg_in_iob 1
run all
