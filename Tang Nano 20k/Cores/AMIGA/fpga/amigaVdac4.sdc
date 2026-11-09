create_clock -name clk_osc -period 37.037037 [get_ports {clk_27m}]
create_generated_clock -name clk85 -source [get_ports {clk_27m}] -multiply_by 19 -divide_by 6 [get_pins {amigaclks/sysclk_inst/CLKOUT}]
// CLKOUTP 270 градусов только на вывод такта SDRAM. CLKOUTD3 растёт на спаде CLKOUT,
// то есть вместе с рабочим фронтом контроллера SDRAM (спад clk85).
create_generated_clock -name clk_sdram -source [get_pins {amigaclks/sysclk_inst/CLKOUT}] -multiply_by 1 -phase 270 [get_pins {amigaclks/sysclk_inst/CLKOUTP}]
create_generated_clock -name clk28 -source [get_pins {amigaclks/sysclk_inst/CLKOUT}] -edges {2 5 8} [get_pins {amigaclks/sysclk_inst/CLKOUTD3}]
// 8 ноября 2026 (0.13.0): вывод 576p50 / 480p60 — такт TMDS 135 МГц (27 × 5), точка 27 МГц (÷5).
create_generated_clock -name clk_hdmi -source [get_ports {clk_27m}] -multiply_by 5 [get_pins {amigaclks/tmdsclk_inst/CLKOUT}]
create_generated_clock -name clk_pixel -source [get_pins {amigaclks/tmdsclk_inst/CLKOUT}] -divide_by 5 [get_pins {amigaclks/clkdiv_inst/CLKOUT}]
// every multi cycle setup exception needs its hold counterpart, otherwise the
// hold analysis still assumes a single cycle relationship between the two
// domains and reports thousands of meaningless violations
set_multicycle_path -from [get_clocks {clk28}] -to [get_clocks {clk85}] 3
set_multicycle_path -from [get_clocks {clk28}] -to [get_clocks {clk85}] -hold 2


// the TG68K advances at most every second clk28 cycle (CPU_SLOW14 in
// cpu_wrapper.v), so all its internal register to register paths are
// true two cycle paths
set_multicycle_path -from [get_regs {*cpu_inst_p/*}] -to [get_regs {*cpu_inst_p/*}] -setup -end 2
set_multicycle_path -from [get_regs {*cpu_inst_p/*}] -to [get_regs {*cpu_inst_p/*}] -hold -end 1
// the TG68K register file is distributed LUT ram, not covered by get_regs,
// so additionally relax paths ending at any pin inside the cpu core
set_multicycle_path -from [get_regs {*cpu_inst_p/*}] -to [get_pins {*cpu_inst_p/*/DI* *cpu_inst_p/*/AD* *cpu_inst_p/*/WRE}] -setup -end 2
set_multicycle_path -from [get_regs {*cpu_inst_p/*}] -to [get_pins {*cpu_inst_p/*/DI* *cpu_inst_p/*/AD* *cpu_inst_p/*/WRE}] -hold -end 1
// The disk length register reaches the blitter through the dma priority logic.
// Both live in the chipset, which advances on the 7MHz (and slower) clock
// enables, so these are multi cycle paths on the 28MHz clock.
set_multicycle_path -from [get_regs {*PAULA1/pf1/dsklen*}] -to [get_regs {*AGNUS1/bl1/*}] -setup -end 4
set_multicycle_path -from [get_regs {*PAULA1/pf1/dsklen*}] -to [get_regs {*AGNUS1/bl1/*}] -hold -end 3
// 9 ноября 2026 (0.13.5): режим совместимости compat_mode (amiga_vdac4.sv) защёлкивается только пока
// nanomig_reset держит CPU в сбросе (ESP пишет 0x6B до загрузки ROM и сброса) и весь сеанс постоянен.
// Через вход CPU (cpucfg) он входит в декодер TG68K (SR_Read/VBR_Stackframe = 2), а также в адрес 24 бита,
// fastram/turbo. К выходу CPU из сброса значение стоит сотни тактов — пути от него ложные. Сборка
// 20261108-232643Z показала их -5,06 нс (в 0.13.3 проходили по везению раскладки).
set_false_path -from [get_regs {compat_mode*}]
create_clock -name link_sck -period 25 [get_ports {link_sck}]
create_clock -name cp_sck -period 33.333 [get_ports {cp_sck}]
set_clock_groups -asynchronous -group [get_clocks {clk85 clk28 clk_osc}] -group [get_clocks {clk_hdmi clk_pixel}] -group [get_clocks {link_sck}] -group [get_clocks {cp_sck}]
set_false_path -from [get_ports {link_cs_n link_io[*] cp_cs_n cp_mosi btn[*] m_sel}]
// 8 ноября 2026 (0.12.18), по образцу Next 0.18: полутактовые пути clk85 (фронт -> спад и обратно,
// 5,848 нс), по которым каждое слово SDRAM идёт к потребителю на следующем же фронте, — с запасом
// не меньше 1,3 нс при любой раскладке. В Next запас 0,03 нс на таком пути давал ошибки чтения при
// чистом STA. Здесь: dq_q (вход DQ, фронт) -> dout чипсета/CPU, video_dout и dq_r (спад); слово,
// адрес и разрешение записи кэша вывода (спад DMA) -> wr_* (фронт); адрес чтения буфера захвата
// (спад DMA и контроллера) -> read_address_q (фронт). Пути от регистров спада к выводу DQ (dq_out)
// сюда не входят: значение берётся через полтора такта (DQ включается на такт позже), STA их
// считает строже, чем есть. Строк report_timing здесь нет: они заменяют стандартные таблицы отчёта,
// а validation.py читает именно их (8.11: сборка 114421Z — запас dq_q 0,909 от 4,5, wr_* 1,569).
set_max_delay 4.5 -from [get_pins {sdram/dq_q_*/Q}]
set_max_delay 4.5 -to [get_pins {video/cache/wr_*/D}]
set_max_delay 4.5 -to [get_pins {video/capture/read_address_q_*/D}]
