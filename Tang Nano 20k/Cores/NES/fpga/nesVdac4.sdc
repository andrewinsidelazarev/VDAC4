# NESTang's native clock ratio, plus VDAC4 LINK and CP clocks.
create_clock -name clk_27m -period 37.037 [get_ports {clk_27m}]
create_generated_clock -name fclk -source [get_ports {clk_27m}] -multiply_by 12 -divide_by 5 [get_pins {pll_nes/rpll_inst/CLKOUT}]
create_generated_clock -name clk -source [get_pins {pll_nes/rpll_inst/CLKOUT}] -divide_by 3 [get_pins {pll_nes/rpll_inst/CLKOUTD3}]
create_clock -name hclk5 -period 2.6936 [get_pins {pll_hdmi/rpll_inst/CLKOUT}]
create_generated_clock -name hclk -source [get_pins {pll_hdmi/rpll_inst/CLKOUT}] -divide_by 5 [get_pins {div5/CLKOUT}]
create_clock -name link_sck -period 25.000 [get_ports {link_sck}]
create_clock -name cp_sck -period 33.333 [get_ports {cp_sck}]
set_clock_groups -asynchronous -group [get_clocks {clk fclk}] -group [get_clocks {hclk hclk5}] -group [get_clocks {link_sck}] -group [get_clocks {cp_sck}]
set_false_path -from [get_ports {link_cs_n link_io[*] cp_cs_n cp_mosi btn[*] m_sel}]
# Upstream NES SDRAM phase schedule: requests live for three fclk cycles,
# and return data is captured two fclk cycles after the read.
set_multicycle_path 3 -end -setup -from [get_clocks {clk}] -to [get_clocks {fclk}]
set_multicycle_path 2 -end -hold -from [get_clocks {clk}] -to [get_clocks {fclk}]
set_multicycle_path 2 -start -setup -from [get_clocks {fclk}] -to [get_clocks {clk}]
set_multicycle_path 1 -start -hold -from [get_clocks {fclk}] -to [get_clocks {clk}]
report_timing -setup -max_paths 30
report_timing -hold -max_paths 30
