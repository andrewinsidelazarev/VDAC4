# Mega Drive VDAC4 (03.11.2026). MCLK (он же такт контроллера SDRAM), Z80 и такт микросхемы SDRAM — одна
# PLL (CLKOUT, CLKOUTD, CLKOUTP 135°). HDMI, LINK и BL616 — свои домены.
create_clock -name clk_27m -period 37.037 [get_ports {clk_27m}]
create_generated_clock -name mclk -source [get_ports {clk_27m}] -multiply_by 2 [get_pins {pll_sys/rpll_inst/CLKOUT}]
create_generated_clock -name clk_z80 -source [get_pins {pll_sys/rpll_inst/CLKOUT}] -divide_by 2 [get_pins {pll_sys/rpll_inst/CLKOUTD}]
create_generated_clock -name clk_sdp -source [get_ports {clk_27m}] -multiply_by 2 -phase 135 [get_pins {pll_sys/rpll_inst/CLKOUTP}]
create_clock -name hclk5 -period 2.6936 [get_pins {pll_hdmi/rpll_inst/CLKOUT}]
create_generated_clock -name hclk -source [get_pins {pll_hdmi/rpll_inst/CLKOUT}] -divide_by 5 [get_pins {div5/CLKOUT}]
create_clock -name link_sck -period 25.000 [get_ports {link_sck}]
create_clock -name cp_sck -period 33.333 [get_ports {cp_sck}]
set_clock_groups -asynchronous -group [get_clocks {mclk clk_z80 clk_sdp}] -group [get_clocks {hclk hclk5}] -group [get_clocks {link_sck}] -group [get_clocks {cp_sck}]
set_false_path -from [get_ports {link_cs_n link_io[*] cp_cs_n cp_mosi btn[*] m_sel}]
# как в mdtang: Z80 -> 68K, два такта clk_z80
set_multicycle_path 4 -end -setup -from [get_clocks {clk_z80}] -to [get_clocks {mclk}]
set_multicycle_path 3 -end -hold -from [get_clocks {clk_z80}] -to [get_clocks {mclk}]
report_timing -setup -max_paths 40
report_timing -hold -max_paths 20
