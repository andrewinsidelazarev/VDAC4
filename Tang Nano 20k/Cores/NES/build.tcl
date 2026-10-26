set_device GW2AR-LV18QN88C8/I7 -device_version C
add_file fpga/config.v
add_file fpga/core/apu.v
add_file upstream/src/autofire.v
add_file fpga/core/cart.sv
add_file upstream/src/compat.v
add_file upstream/src/dpram.v
add_file upstream/src/EEPROM_24C0x.sv
add_file upstream/src/game_loader.v
add_file upstream/src/hdmi2/audio_clock_regeneration_packet.sv
add_file upstream/src/hdmi2/audio_info_frame.sv
add_file upstream/src/hdmi2/audio_sample_packet.sv
add_file upstream/src/hdmi2/auxiliary_video_information_info_frame.sv
add_file upstream/src/hdmi2/hdmi.sv
add_file upstream/src/hdmi2/packet_assembler.sv
add_file upstream/src/hdmi2/packet_picker.sv
add_file upstream/src/hdmi2/serializer.sv
add_file upstream/src/hdmi2/source_product_description_info_frame.sv
add_file upstream/src/hdmi2/tmds_channel.sv
add_file fpga/core/mappers/generic.sv
add_file upstream/src/mappers/iir_filter.v
add_file upstream/src/mappers/JYCompany.sv
add_file upstream/src/mappers/misc.sv
add_file fpga/core/mappers/MMC1.sv
add_file fpga/core/mappers/MMC2.sv
add_file fpga/core/mappers/MMC3.sv
add_file upstream/src/mappers/MMC5.sv
add_file upstream/src/mappers/Namco.sv
add_file upstream/src/mappers/Sachen.sv
add_file upstream/src/mappers/Sunsoft.sv
add_file fpga/core/nes.v
add_file fpga/nes_video_scale.sv
add_file fpga/nes_osd_font.sv
add_file fpga/nes_osd.sv
add_file fpga/nes2hdmi_vdac4.sv
add_file fpga/core/ppu.v
add_file upstream/src/sdram_nes.v
add_file fpga/core/T65.v
add_file upstream/src/t65/T65_ALU.v
add_file upstream/src/t65/T65_MCode.v
add_file upstream/src/t65/T65_Pack.v
add_file fpga/gowin_pll_nes.v
add_file fpga/gowin_pll_hdmi.v
add_file fpga/next_qspi.sv
add_file fpga/cp_spi.sv
add_file fpga/next_cp.sv
add_file fpga/next_evo_reset.sv
add_file fpga/nes_save_bridge.sv
add_file fpga/fb_sdspi.sv
add_file fpga/nes_link.sv
add_file fpga/nes_vdac4.sv
add_file fpga/nesVdac4.cst
add_file fpga/nesVdac4.sdc

set_option -synthesis_tool gowinsynthesis
set_option -output_base_name nesVdac4
set_option -top_module nes_vdac4
set_option -verilog_std sysv2017
set_option -rw_check_on_ram 1
set_option -use_mspi_as_gpio 0
set_option -use_sspi_as_gpio 1
set_option -user_code 0x56443404
set_option -multi_boot 1
set_option -place_option 2
set_option -route_option 1
set_option -gen_text_timing_rpt 1
run all
