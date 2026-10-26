// SPDX-License-Identifier: Apache-2.0
//
// Тактирование режима кадрового буфера от кварца Tang Nano 20K (27 МГц, вывод 4).
//
// rPLL: CLKOUT = 27 × FBDIV / IDIV — последовательный клок TMDS (VCO = CLKOUT × ODIV);
//       CLKOUTD = CLKOUT / SDIV — SDRAM и кадровый буфер.
// CLKDIV ÷5 от CLKOUT — пиксельный клок, фазово связан с TMDS.
//
//   576p50: 27 × 5 / 1 = 135 МГц (VCO 540, ODIV 4); SDRAM 135 / 2 = 67,5 МГц; пиксели 27 МГц.
//   720p50: 27 × 55 / 4 = 371,25 МГц (VCO 742,5, ODIV 2); SDRAM 371,25 / 6 = 61,875 МГц;
//           пиксели 74,25 МГц.
//
// Вторая PLL (захват, ×2 от PCLK Evo) — ts_pll_capture из ts_clocking.sv.
// Под `ifdef SIMULATION (ключ -DSIMULATION) — поведенческие заглушки для симуляции;
// синтезу макрос не нужен: по умолчанию собираются примитивы Gowin.
//
module fb_pll_xtal #(
    parameter bit HD = 1'b0          // 0 — 576p50, 1 — 720p50
) (
    input  wire clkin,        // 27 МГц
    input  wire reset,
    output wire clk_serial,   // 135 / 371,25 МГц
    output wire clk_mem,      // 67,5 / 61,875 МГц
    output wire clk_sd,       // CLKOUT ÷ 3: 45 / 123,75 МГц — 2 × clk_mem (SDRAM ядра FT812)
    output wire lock
);
    localparam int IDIV_SEL  = HD ? 3  : 0;     // IDIV − 1
    localparam int FBDIV_SEL = HD ? 54 : 4;     // FBDIV − 1
    localparam int ODIV_SEL  = HD ? 2  : 4;     // ODIV
    localparam int SDIV      = HD ? 6  : 2;
`ifndef SIMULATION
    wire gnd = 1'b0;
    wire unused_p;
    rPLL pll (
        .CLKOUT(clk_serial), .LOCK(lock), .CLKOUTP(unused_p),
        .CLKOUTD(clk_mem), .CLKOUTD3(clk_sd),
        .RESET(reset), .RESET_P(gnd), .CLKIN(clkin), .CLKFB(gnd),
        .FBDSEL(6'b0), .IDSEL(6'b0), .ODSEL(6'b0), .PSDA(4'b0),
        .DUTYDA(4'b0), .FDLY(4'b0)
    );
    defparam pll.FCLKIN = "27";
    defparam pll.IDIV_SEL = IDIV_SEL;
    defparam pll.FBDIV_SEL = FBDIV_SEL;
    defparam pll.ODIV_SEL = ODIV_SEL;
    defparam pll.DYN_SDIV_SEL = SDIV;
    defparam pll.CLKOUTD_SRC = "CLKOUT";
    defparam pll.DYN_IDIV_SEL = "false";
    defparam pll.DYN_FBDIV_SEL = "false";
    defparam pll.DYN_ODIV_SEL = "false";
    defparam pll.PSDA_SEL = "0000";
    defparam pll.DYN_DA_EN = "false";
    defparam pll.DUTYDA_SEL = "1000";
    defparam pll.CLKFB_SEL = "internal";
    defparam pll.CLKOUT_BYPASS = "false";
    defparam pll.CLKOUTP_BYPASS = "false";
    defparam pll.CLKOUTD_BYPASS = "false";
    defparam pll.DEVICE = "GW2AR-18C";
`else
    localparam real MULT = (FBDIV_SEL + 1.0) / (IDIV_SEL + 1.0);
    reg s = 1'b0, m = 1'b0, d3 = 1'b0;
    integer half = 0, h3 = 0;
    realtime last_edge = 0, period = 0;
    always @(posedge clkin) begin
        if (last_edge != 0) period = $realtime - last_edge;
        last_edge = $realtime;
    end
    always begin
        if (period == 0) #1 s = 1'b0;
        else begin
            #(period / (2.0 * MULT)) s = ~s;
            half = half + 1;
            if (half == SDIV) begin half = 0; m = ~m; end
            h3 = h3 + 1;
            if (h3 == 3) begin h3 = 0; d3 = ~d3; end
        end
    end
    assign clk_serial = s;
    assign clk_mem = m;
    assign clk_sd = d3;
    assign lock = (period != 0);
`endif
endmodule
