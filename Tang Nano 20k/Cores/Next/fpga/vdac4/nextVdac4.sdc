# ZX Next на VDAC4: nextNano.sdc NextNano, такт SPI компаньона заменён на LINK и SPI BL616.
#============================================================================
# ZX Spectrum Next / Tang Nano 20K
# Conservative timing baseline for SDRAM controller V89
# Gowin-compatible edition: every SDC command is on one physical line.
#
# Goals:
#   * keep clk_28 and clk_140 as RELATED clocks;
#   * do not hide real clk_28 <-> clk_140 paths with global exceptions;
#   * remove broad T80/DMA multicycle exceptions until proven necessary;
#   * false-path only the first stage of intentional synchronizers;
#   * keep Port B -> Layer 2 fully timed.
#
# IMPORTANT:
#   1. Verify Clock Summary: CLKOUT = 112.5 MHz, CLKOUTD = 28.125 MHz.
#   2. Verify every get_pins pattern matches at least one object.
#   3. Do not add global clk_28 <-> clk_140 multicycle paths.
#============================================================================

#----------------------------------------------------------------------------
# 1. Clocks
#----------------------------------------------------------------------------
create_clock -name clk_27m -period 37.037 [get_ports {clk_27m}]
create_generated_clock -name clk_140 -source [get_ports {clk_27m}] -master_clock clk_27m -multiply_by 50 -divide_by 12 [get_pins {pll_inst/rpll_inst/CLKOUT}]
create_generated_clock -name clk_140p -source [get_ports {clk_27m}] -master_clock clk_27m -multiply_by 50 -divide_by 12 [get_pins {pll_inst/rpll_inst/CLKOUTP}]
create_generated_clock -name clk_28 -source [get_pins {pll_inst/rpll_inst/CLKOUTP}] -master_clock clk_140p -divide_by 4 [get_pins {pll_inst/rpll_inst/CLKOUTD}]

create_clock -name clk_hdmi_5x -period 7.111 [get_pins {hdmi_rpll/hdmi_pll/CLKOUT}]
create_clock -name clk_hdmi_pixel -period 35.556 [get_pins {hdmi_rpll/clkdiv_pixel/CLKOUT}]
# VDAC4: LINK от ESP (до 40 МГц) и SPI BL616 (26,67 МГц) — выводы, как у ядер VDAC4
create_clock -name link_sck -period 25.000 -waveform {0 12.500} [get_ports {link_sck}]
create_clock -name cp_sck -period 33.333 -waveform {0 16.667} [get_ports {cp_sck}]

#----------------------------------------------------------------------------
# 2. Asynchronous clock groups
# clk_28 and clk_140 are intentionally NOT asynchronous.
#----------------------------------------------------------------------------
set_clock_groups -asynchronous -group [get_clocks {clk_28 clk_140}] -group [get_clocks {link_sck}] -group [get_clocks {cp_sck}] -group [get_clocks {clk_hdmi_5x clk_hdmi_pixel}]
set_false_path -from [get_ports {link_cs_n link_io[*] cp_cs_n cp_mosi btn[*]}]


#----------------------------------------------------------------------------
# 3. Intentional synchronizers in SDRAM V89
# Only first receiving stages are false-pathed. Second stages remain timed.
# Verify synthesized names because Gowin may add _s0/_s1 suffixes.
#----------------------------------------------------------------------------
set_false_path -to [get_pins {u_sdram/a_req_toggle_sync1*/D}]
set_false_path -to [get_pins {u_sdram/a_epoch_sync1*/D}]
# VDAC4: первые ступени синхронизаторов уровня «цикл Z80 идёт» (ASYNC_REG meta -> sync) — по
# правилу выше. Z80 сблокирован RAM_A_WAIT: запрос идёт переключателем a_req_toggle, данные —
# только по ответу; уровень лишь выбирает момент регенерации и начала выборки, поздний или
# промежуточный отсчёт меняет задержку, но не данные. У автора путь Z80 -> meta укладывался с
# запасом больше 3,2 нс, при размещении VDAC4 он 12 нс при 8,9 нс отношения clk_28 -> clk_140
# (ядра Next 0.1 и 0.2 без этой строки: два нарушенных конца, -3,2…-5,7 нс).
set_false_path -to [get_pins {u_sdram/a_req_level_meta_112*/D}]
set_false_path -to [get_pins {u_sdram/a_cycle_meta_112*/D}]



# set_false_path -to [get_pins {u_sdram/port_reset_meta_112*/D}]
# set_false_path -to [get_pins {u_sdram/sdram_ready_meta_28*/D}]


#----------------------------------------------------------------------------
# 4. Port A bundled-data transfer: diagnostic baseline
# No bundled-data multicycle is enabled in this first diagnostic build.
# Do not apply a clock-wide clk_28 -> clk_140 exception.
#
# Endpoint-specific template, intentionally disabled until exact endpoints and
# capture relation are confirmed from the report:
# set_multicycle_path -setup 2 -end -from [get_pins {u_sdram/a_req_addr_28*/Q}] -to [get_pins {u_sdram/req_addr_r*/D}]
# set_false_path -hold -from [get_pins {u_sdram/a_req_addr_28*/Q}] -to [get_pins {u_sdram/req_addr_r*/D}]
#----------------------------------------------------------------------------

#----------------------------------------------------------------------------
# 5. Port B / Layer 2
# No false path, multicycle or relaxed max delay. This is a real pixel deadline.
#----------------------------------------------------------------------------

#----------------------------------------------------------------------------
# 6. T80 / DMA
# Broad historical multicycle exceptions are intentionally absent.
#----------------------------------------------------------------------------

#----------------------------------------------------------------------------
# 7. Optional placement goals -- disabled for the first diagnostic build
#----------------------------------------------------------------------------
# set_max_delay 16.0 -from [get_pins {zxnext_top/zxnext/cpu_mod/z80n/BusAck_s*/Q}] -to [get_pins {zxnext_top/zxnext/cpu_mod/MReq_Inhibit_s*/D}]
# set_max_delay 16.0 -from [get_pins {zxnext_top/zxnext/cpu_mod/DI_Reg_*/Q}] -to [get_pins {zxnext_top/zxnext/dma_mod/*/D zxnext_top/zxnext/dma_mod/*/CE}]
# set_max_delay 16.0 -from [get_pins {zxnext_top/zxnext/dma_mod/dma_rd_s*/Q zxnext_top/zxnext/cpu_mod/RD_s*/Q}] -to [get_pins {zxnext_top/zxnext/cpu_mod/z80n/TmpAddr_*/D zxnext_top/zxnext/cpu_mod/z80n/IR_*/D}]
# set_max_delay 16.0 -from [get_pins {zxnext_top/zxnext/timing_mod/whc_*/Q}] -to [get_pins {zxnext_top/zxnext/sprite_mod/linebuf*/*/WRE}]

#----------------------------------------------------------------------------
# 8. Reports
#----------------------------------------------------------------------------
report_timing -setup -max_paths 350
report_timing -hold -max_paths 350
report_timing -setup -from [get_clocks {clk_28}] -to [get_clocks {clk_140}] -max_paths 100 -max_common_paths 3
report_timing -hold -from [get_clocks {clk_28}] -to [get_clocks {clk_140}] -max_paths 100
report_timing -setup -from [get_clocks {clk_140}] -to [get_clocks {clk_28}] -max_paths 100 -max_common_paths 3
report_timing -hold -from [get_clocks {clk_140}] -to [get_clocks {clk_28}] -max_paths 100
report_timing -setup -from [get_clocks {clk_28}] -to [get_clocks {clk_28}] -max_paths 150 -max_common_paths 3
report_timing -hold -from [get_clocks {clk_28}] -to [get_clocks {clk_28}] -max_paths 100
report_timing -setup -to [get_pins {zxnext_top/zxnext/cpu_mod/DI_Reg_*/D zxnext_top/zxnext/dma_mod/dma_d_n_s_*/D zxnext_top/zxnext/dma_mod/dma_wr_s_s*/D zxnext_top/zxnext/dma_mod/dma_rd_s_s*/D zxnext_top/zxnext/dma_mod/wait_n_s_s*/D}] -max_paths 100
report_timing -hold -to [get_pins {zxnext_top/zxnext/cpu_mod/DI_Reg_*/D zxnext_top/zxnext/dma_mod/dma_d_n_s_*/D zxnext_top/zxnext/dma_mod/dma_wr_s_s*/D zxnext_top/zxnext/dma_mod/dma_rd_s_s*/D zxnext_top/zxnext/dma_mod/wait_n_s_s*/D}] -max_paths 100
report_timing -setup -from [get_pins {zxnext_top/zxnext/cpu_mod/MREQ_s*/Q zxnext_top/zxnext/cpu_mod/MReq_Inhibit_s*/Q zxnext_top/zxnext/cpu_mod/Req_Inhibit_s*/Q zxnext_top/zxnext/cpu_mod/IORQ_t1_s*/Q zxnext_top/zxnext/cpu_mod/IORQ_t2_s*/Q}] -to [get_pins {zxnext_top/zxnext/cpu_mod/z80n/*/D zxnext_top/zxnext/cpu_mod/z80n/*/CE}] -max_paths 250
report_timing -hold -from [get_pins {zxnext_top/zxnext/cpu_mod/MREQ_s*/Q zxnext_top/zxnext/cpu_mod/MReq_Inhibit_s*/Q zxnext_top/zxnext/cpu_mod/Req_Inhibit_s*/Q zxnext_top/zxnext/cpu_mod/IORQ_t1_s*/Q zxnext_top/zxnext/cpu_mod/IORQ_t2_s*/Q}] -to [get_pins {zxnext_top/zxnext/cpu_mod/z80n/*/D zxnext_top/zxnext/cpu_mod/z80n/*/CE}] -max_paths 250
report_timing -setup -from [get_clocks {clk_140}] -to [get_clocks {clk_140}] -max_paths 100
report_timing -hold -from [get_clocks {clk_140}] -to [get_clocks {clk_140}] -max_paths 100
report_timing -setup -from [get_pins {u_sdram/RAM_B_DO*/Q}] -to [get_pins {zxnext_top/zxnext/layer2_mod/*/D}] -max_paths 100
report_timing -hold -from [get_pins {u_sdram/RAM_B_DO*/Q}] -to [get_pins {zxnext_top/zxnext/layer2_mod/*/D}] -max_paths 100

# Gowin versions differ in support for the commands below. Uncomment only if
# your version accepts them.
# report_exceptions -setup
# report_exceptions -hold
# report_min_pulse_width -nworst 20 -detail

# 0.18: путь данных в CPU и DMA (DI_Reg, dma_d_n_s защёлкиваются по спаду clk_28 — полтакта, 17,8 нс)
# с запасом не меньше 1,3 нс при любой раскладке. Этим путём процессор берёт каждый байт памяти и
# портов; у 0.17 запас был 0,031 нс (cpu_speed_r -> dma_d_n_s_2), у 0.16 — 0,305 нс, и сборка 0.17
# на плате сбоила (зависание после TBBlue, ошибки чтения файлов). Заменяет предел 17,3 нс от BusAck (0.10).
set_max_delay 16.5 -to [get_pins {zxnext_top/zxnext/cpu_mod/DI_Reg_*/D zxnext_top/zxnext/dma_mod/dma_d_n_s_*/D}]
set_max_delay 16.5 -from [get_pins {zxnext_top/zxnext/cpu_mod/DI_Reg_*/Q zxnext_top/zxnext/dma_mod/dma_d_n_s_*/Q}]
set_max_delay 16.5 -to [get_pins {zxnext_top/zxnext/dma_mod/dma_wr_s_s*/D zxnext_top/zxnext/dma_mod/dma_rd_s_s*/D zxnext_top/zxnext/dma_mod/wait_n_s_s*/D}]
