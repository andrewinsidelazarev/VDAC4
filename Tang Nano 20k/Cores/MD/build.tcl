set_device GW2AR-LV18QN88C8/I7 -device_version C
add_file upstream/src/common/dpram.v
add_file upstream/src/common/dpram32_block.v
add_file upstream/src/common/dpram_block.v
add_file upstream/src/fx68k/fx68k.sv
add_file upstream/src/fx68k/fx68kAlu.sv
add_file upstream/src/fx68k/uaddrPla.sv
add_file upstream/src/fx68k/microrom.mem
add_file upstream/src/fx68k/nanorom.mem
add_file upstream/src/t80/t80.v
add_file upstream/src/t80/t80_alu.v
add_file upstream/src/t80/t80_mcode.v
add_file upstream/src/t80/t80_reg.v
add_file fpga/core/t80s.v
add_file fpga/core/vdp.v
add_file upstream/src/vdp/vdp_common.v
add_file upstream/src/peripherals/fourway.v
add_file upstream/src/peripherals/gen_io.sv
add_file upstream/src/peripherals/multitap.sv
add_file upstream/src/peripherals/teamplayer.sv
add_file upstream/src/hdmi/tmds_channel.sv
add_file fpga/core/system.sv
add_file fpga/md_ym_stub.sv
add_file fpga/md_snd_events.sv
add_file fpga/md_sdram.sv
add_file fpga/nes_osd_font.sv
add_file fpga/nes_osd.sv
add_file fpga/tmds_encoder.sv
add_file fpga/hdmi_tx.sv
add_file fpga/md_video.sv
add_file fpga/next_qspi.sv
add_file fpga/fb_sdspi.sv
add_file fpga/md_link.sv
add_file fpga/cp_spi.sv
add_file fpga/next_cp.sv
add_file fpga/next_evo_reset.sv
add_file fpga/md_pll.v
add_file fpga/gowin_pll_hdmi.v
add_file fpga/md_vdac4.sv
add_file fpga/mdVdac4.cst
add_file fpga/mdVdac4.sdc

set_option -synthesis_tool gowinsynthesis
set_option -output_base_name mdVdac4
set_option -top_module md_vdac4
set_option -include_path {"upstream/src/vdp" "upstream/src/common"}
set_option -verilog_std sysv2017
set_option -rw_check_on_ram 1
# шина SDRAM — регистрами в IOB (md_sdram: dq_in, команды, адрес, данные записи)
set_option -ireg_in_iob 1
set_option -oreg_in_iob 1
set_option -ioreg_in_iob 1
set_option -use_mspi_as_gpio 0
set_option -use_sspi_as_gpio 1
set_option -user_code 0x56443406
set_option -multi_boot 1
set_option -place_option 2
set_option -route_option 1
set_option -gen_text_timing_rpt 1
run all
