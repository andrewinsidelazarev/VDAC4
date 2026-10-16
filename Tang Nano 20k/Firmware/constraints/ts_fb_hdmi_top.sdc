# Временные ограничения прошивки с кадровым буфером (ts_fb_hdmi_top).
# Gowin не принимает перенос строки через обратную косую черту: каждая команда — одной строкой.
#
# Два первичных источника, не связанных между собой:
#   clk27   — кварц Tang: TMDS 371,25 МГц, SDRAM 61,875 МГц, пиксели 74,25 МГц (720p50);
#   pclk_in — PCLK от ZX Evolution, ~28 МГц: захват 56 МГц.

create_clock -name clk27   -period 37.037 -waveform {0 18.518} [get_ports {clk27}]
create_clock -name pclk_in -period 35.714 -waveform {0 17.857} [get_ports {pclk_in}]
# LINK_SCK от ESP: до 40 МГц
create_clock -name link_sck -period 25.000 -waveform {0 12.500} [get_ports {link_sck}]

# Выход 720p50 (HD = 1): TMDS 27 × 55 / 4 = 371,25 МГц, SDRAM ÷6 = 61,875 МГц, пиксели ÷5 = 74,25 МГц.
# Для 576p50 (HD = 0): -multiply_by 5 без -divide_by и SDRAM -divide_by 2.
create_generated_clock -name clk_serial -source [get_ports {clk27}] -multiply_by 55 -divide_by 4 [get_pins {pll_xtal/pll/CLKOUT}]
create_generated_clock -name clk_mem -source [get_pins {pll_xtal/pll/CLKOUT}] -divide_by 6 [get_pins {pll_xtal/pll/CLKOUTD}]
create_generated_clock -name pixel_clk -source [get_pins {pll_xtal/pll/CLKOUT}] -divide_by 5 [get_pins {clkdiv/divider/CLKOUT}]
create_generated_clock -name clk_cap -source [get_ports {pclk_in}] -multiply_by 2 [get_pins {pll_capture/pll/CLKOUTP}]

# Домены связаны только через асинхронный FIFO, двухклоковый строчный буфер и
# синхронизированные переключатели — статический анализ между ними не нужен.
set_clock_groups -asynchronous -group [get_clocks {clk_cap}] -group [get_clocks {clk_mem}] -group [get_clocks {pixel_clk clk_serial}] -group [get_clocks {link_sck}]

set_false_path -from [get_ports {mux_d[*] hsync_in vsync_in m_sel btn[*] link_cs_n link_io[*] flash_miso}]
