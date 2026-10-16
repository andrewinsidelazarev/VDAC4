# Временные ограничения базовой видеопрошивки A3.1.
#
# Единственный первичный источник — PCLK от ZX Evolution, номинально 28 МГц
# (docs/tsconfig-tang20k-consolidated-notes.md, 10.1: PCLK = !fclk основной ПЛИС).
# Остальные клоки получены из него двумя PLL и делителем CLKDIV.
#
# Период 35,714 нс = 1 / 28 МГц. Реальная частота кварца Evo не измерялась,
# поэтому при отладке имеет смысл проверить её и при необходимости поправить.

create_clock -name pclk_in -period 35.714 -waveform {0 17.857} [get_ports {pclk_in}]

# Последовательный клок TMDS: 140 МГц (PCLK × 5)
create_generated_clock -name serial_clk -source [get_ports {pclk_in}] -multiply_by 5 [get_pins {pll_tmds/pll/CLKOUT}]

# Пиксельный клок: 28 МГц (140 / 5), фазово связан с последовательным
create_generated_clock -name pixel_clk -source [get_pins {pll_tmds/pll/CLKOUT}] -divide_by 5 [get_pins {clkdiv/divider/CLKOUT}]

# Клок приёма: 56 МГц (PCLK × 2)
create_generated_clock -name clk_cap -source [get_ports {pclk_in}] -multiply_by 2 [get_pins {pll_capture/pll/CLKOUTP}]

# Домены приёма и вывода связаны только через строчный буфер и триггер-переключатель
# кадрового импульса; сквозной статический анализ между ними не требуется.
set_clock_groups -asynchronous -group [get_clocks {clk_cap}] -group [get_clocks {pixel_clk serial_clk}]

# Входные данные мультиплексоров приходят асинхронно относительно clk_cap:
# запас setup/hold на плате не измерен, анализ по этим путям отключён намеренно,
# выборка настраивается параметром SAMPLE_DELAY после проверки на железе.
set_false_path -from [get_ports {mux_d[*] hsync_in vsync_in m_sel}]
