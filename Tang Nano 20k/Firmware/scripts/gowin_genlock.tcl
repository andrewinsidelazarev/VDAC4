# Синтез и раскладка генлок-прошивки ts_hdmi_top в Gowin (gw_sh). Запуск из fpga/vdac2:
#   gw_sh scripts/gowin_genlock.tcl
set_device GW2AR-LV18QN88C8/I7 -name GW2AR-18C -device_version C
foreach f {ts_hdmi_top ts_clocking ts_video_chain ts_mux_capture ts_line_buffer
           ts_video_timing legacy_palette tmds_encoder tmds_serializer_gowin} {
    add_file rtl/$f.sv
}
add_file constraints/ts_hdmi_top.cst
add_file constraints/ts_hdmi_top.sdc
set_option -output_base_name ts_hdmi_top
set_option -top_module ts_hdmi_top
set_option -verilog_std sysv2017
set_option -synthesis_tool gowinsynthesis
set_option -use_sspi_as_gpio 1
set_option -use_mspi_as_gpio 1
set_option -use_ready_as_gpio 1
set_option -use_done_as_gpio 1
set_option -gen_text_timing_rpt 1
set_option -ioreg_in_iob 1
run all
