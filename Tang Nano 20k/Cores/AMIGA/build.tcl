set_device GW2AR-LV18QN88C8/I7 -device_version C
add_file fpga/amiga_vdac4.sv
add_file upstream/src/minimig-aga/amiga_clk.v
add_file upstream/src/minimig-aga/cpu_cache_new.v
add_file upstream/src/minimig-aga/dpram.v
add_file upstream/src/minimig-aga/dpram_be_1024x16.v
add_file upstream/src/minimig-aga/minimig.v
add_file upstream/src/minimig-aga/ciaa.v
add_file upstream/src/minimig-aga/ciab.v
add_file upstream/src/minimig-aga/cia_int.v
add_file upstream/src/minimig-aga/cia_timera.v
add_file upstream/src/minimig-aga/cia_timerb.v
add_file upstream/src/minimig-aga/cia_timerd.v
add_file upstream/src/minimig-aga/paula.v
add_file upstream/src/minimig-aga/paula_uart.v
add_file upstream/src/minimig-aga/paula_audio_channel.v
add_file upstream/src/minimig-aga/paula_audio_mixer.v
add_file upstream/src/minimig-aga/paula_audio.v
add_file upstream/src/minimig-aga/paula_audio_volume.v
add_file upstream/src/minimig-aga/paula_floppy_fifo.v
add_file upstream/src/minimig-aga/paula_floppy.v
add_file upstream/src/minimig-aga/paula_intcontroller.v
add_file upstream/src/minimig-aga/agnus.v
add_file upstream/src/minimig-aga/agnus_audiodma.v
add_file upstream/src/minimig-aga/agnus_blitter_adrgen.v
add_file upstream/src/minimig-aga/agnus_blitter_minterm.v
add_file upstream/src/minimig-aga/agnus_diskdma.v
add_file upstream/src/minimig-aga/agnus_beamcounter.v
add_file upstream/src/minimig-aga/agnus_blitter_barrelshifter.v
add_file upstream/src/minimig-aga/agnus_blitter.v
add_file upstream/src/minimig-aga/agnus_refresh.v
add_file upstream/src/minimig-aga/agnus_bitplanedma.v
add_file upstream/src/minimig-aga/agnus_blitter_fill.v
add_file upstream/src/minimig-aga/agnus_copper.v
add_file upstream/src/minimig-aga/agnus_spritedma.v
add_file upstream/src/minimig-aga/denise.v
add_file upstream/src/minimig-aga/denise_bitplane_shifter.v
add_file upstream/src/minimig-aga/bitplane_ram.v
add_file upstream/src/minimig-aga/sprite_ram.v
add_file upstream/src/minimig-aga/denise_collision.v
add_file upstream/src/minimig-aga/denise_colortable.v
add_file upstream/src/minimig-aga/denise_playfields.v
add_file upstream/src/minimig-aga/denise_sprites_shifter.v
add_file upstream/src/minimig-aga/denise_bitplanes.v
add_file upstream/src/minimig-aga/denise_hamgenerator.v
add_file upstream/src/minimig-aga/denise_spritepriority.v
add_file upstream/src/minimig-aga/denise_sprites.v
add_file upstream/src/minimig-aga/denise_colortable_ram_mf.v
add_file upstream/src/minimig-aga/gary.v
add_file upstream/src/minimig-aga/gayle.v
add_file upstream/src/minimig-aga/ide.v
add_file upstream/src/minimig-aga/minimig_m68k_bridge.v
add_file upstream/src/minimig-aga/minimig_bankmapper.v
add_file upstream/src/minimig-aga/minimig_sram_bridge.v
add_file upstream/src/minimig-aga/minimig_syscontrol.v
add_file upstream/src/minimig-aga/userio.v
add_file upstream/src/hdmi/hdmi.sv
add_file upstream/src/hdmi/tmds_channel.sv
add_file upstream/src/misc/video_analyzer.v
add_file upstream/src/misc/amiga_keymap.v
add_file upstream/src/tang/nano20k/gowin_dpb/sector_dpram.v
add_file upstream/src/tang/nano20k/gowin_dpb/ide_dpram.v
add_file upstream/src/misc/rst_sync.v
add_file upstream/src/tg68k/TG68K_Pack.vhd
add_file upstream/src/tg68k/TG68K.vhd
add_file upstream/src/tg68k/TG68K_ALU.vhd
add_file upstream/src/tg68k/TG68KdotC_Kernel.vhd
add_file fpga/amigaclks.v
add_file fpga/nanomig.v
add_file fpga/cpu_wrapper.v
add_file fpga/amiga_debug.sv
add_file fpga/amiga_disk.sv
add_file fpga/sdram.sv
add_file fpga/Amber.v
add_file fpga/hid.v
add_file fpga/serializer.sv
add_file fpga/next_qspi.sv
add_file fpga/cp_spi.sv
add_file fpga/next_cp.sv
add_file fpga/next_evo_reset.sv
add_file fpga/fb_sdspi.sv
add_file fpga/amiga_link.sv
add_file fpga/amigaVdac4.cst
add_file fpga/amigaVdac4.sdc
set_option -synthesis_tool gowinsynthesis
set_option -top_module amiga_vdac4
set_option -output_base_name amigaVdac4
set_option -verilog_std sysv2017
set_option -use_mspi_as_gpio 0
set_option -use_sspi_as_gpio 1
set_option -use_jtag_as_gpio 0
set_option -user_code 0x56443405
set_option -multi_boot 1
set_option -timing_driven 1
set_option -place_option 0
set_option -route_option 2
set_option -gen_text_timing_rpt 1
run all
