// SPDX-License-Identifier: Apache-2.0
//
// Тактирование прошивки: два rPLL и делитель CLKDIV, все — от входного PCLK 28 МГц.
//
// Расчёт коэффициентов (GW2AR-18C, rPLL: CLKOUT = CLKIN × FBDIV / IDIV, VCO = CLKOUT × ODIV,
// VCO должен лежать в 400…1200 МГц, частота сравнения CLKIN/IDIV — не ниже единиц МГц):
//
//   Последовательный клок TMDS: 28 × 5 / 1 = 140 МГц, ODIV = 8 → VCO = 1120 МГц.
//     Пиксельный клок получается из него делителем CLKDIV на 5 = 28 МГц и остаётся
//     фазово связанным с последовательным — это требование OSER10.
//
//   Клок приёма: 28 × 2 / 1 = 56 МГц, ODIV = 16 → VCO = 896 МГц.
//     Ровно два такта на пиксель: по одному на каждую фазу мультиплексора.
//
// Частота сравнения в обоих случаях равна 28 МГц (IDIV = 1) — это внутри допустимого.
// Значения FBDIV_SEL/IDIV_SEL/ODIV_SEL задаются на единицу меньше самих делителей.
//
// В симуляции примитивы Gowin недоступны, поэтому под `ifdef SIMULATION (ключ -DSIMULATION) собраны
// поведенческие заглушки: они не моделируют захват PLL, а просто выдают нужные
// частоты от идеального входа — этого достаточно для проверки тракта данных.

// ---------------------------------------------------------------------------
// PLL последовательного клока TMDS: 28 МГц → 140 МГц
// ---------------------------------------------------------------------------
module ts_pll_tmds (
    input  wire clkin,
    input  wire reset,
    output wire clkout,
    output wire lock
);
`ifndef SIMULATION
    wire gnd = 1'b0;
    wire unused_p, unused_d, unused_d3;
    rPLL pll (
        .CLKOUT(clkout), .LOCK(lock), .CLKOUTP(unused_p),
        .CLKOUTD(unused_d), .CLKOUTD3(unused_d3),
        .RESET(reset), .RESET_P(gnd), .CLKIN(clkin), .CLKFB(gnd),
        .FBDSEL(6'b0), .IDSEL(6'b0), .ODSEL(6'b0), .PSDA(4'b0),
        .DUTYDA(4'b0), .FDLY(4'b0)
    );
    defparam pll.FCLKIN = "28";
    defparam pll.IDIV_SEL = 0;      // IDIV = 1
    defparam pll.FBDIV_SEL = 4;     // FBDIV = 5  → 140 МГц
    defparam pll.ODIV_SEL = 8;      // VCO = 1120 МГц
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
    // Заглушка: умножение на 5 выражается генерацией клока с периодом /5.
    reg clk_q = 1'b0;
    realtime last_edge = 0, period = 0;
    always @(posedge clkin) begin
        if (last_edge != 0) period = $realtime - last_edge;
        last_edge = $realtime;
    end
    always begin
        if (period == 0) #1 clk_q = 1'b0;
        else begin
            #(period / 10.0) clk_q = ~clk_q;
        end
    end
    assign clkout = clk_q;
    assign lock = (period != 0);
`endif
endmodule

// ---------------------------------------------------------------------------
// PLL домена приёма: 28 МГц → 56 МГц
//
// Клок приёма берётся с выхода CLKOUTP, фаза которого задаётся на лету: phase × 1/16
// периода 56 МГц (≈1,12 нс на шаг). Этим момент выборки мультиплексоров сдвигается
// относительно смены пикселя Evo (пункт MUX CLOCK меню): если выборка попадает на
// смену пикселя, красная или голубая кайма на перепадах есть при любой MUX PHASE.
// SEL тактируется тем же клоком, поэтому связь «SEL → выборка» от фазы не зависит.
// Скважность CLKOUTP задаёт абсолютное положение спада (DUTYDA, в 1/16 периода):
// для 50 % спад ставится на полпериода после фронта.
// ---------------------------------------------------------------------------
module ts_pll_capture (
    input  wire       clkin,
    input  wire       reset,
    input  wire [3:0] phase,
    output wire       clkout,
    output wire       lock
);
`ifndef SIMULATION
    wire gnd = 1'b0;
    wire unused_o, unused_d, unused_d3;
    wire [3:0] duty = phase + 4'd8;
    rPLL pll (
        .CLKOUT(unused_o), .LOCK(lock), .CLKOUTP(clkout),
        .CLKOUTD(unused_d), .CLKOUTD3(unused_d3),
        .RESET(reset), .RESET_P(gnd), .CLKIN(clkin), .CLKFB(gnd),
        .FBDSEL(6'b0), .IDSEL(6'b0), .ODSEL(6'b0), .PSDA(phase),
        .DUTYDA(duty), .FDLY(4'b1111)
    );
    defparam pll.FCLKIN = "28";
    defparam pll.IDIV_SEL = 0;      // IDIV = 1
    defparam pll.FBDIV_SEL = 1;     // FBDIV = 2 → 56 МГц
    defparam pll.ODIV_SEL = 16;     // VCO = 896 МГц
    defparam pll.DYN_IDIV_SEL = "false";
    defparam pll.DYN_FBDIV_SEL = "false";
    defparam pll.DYN_ODIV_SEL = "false";
    defparam pll.PSDA_SEL = "0000";
    defparam pll.DYN_DA_EN = "true";
    defparam pll.DUTYDA_SEL = "1000";
    defparam pll.CLKFB_SEL = "internal";
    defparam pll.CLKOUT_BYPASS = "false";
    defparam pll.CLKOUTP_BYPASS = "false";
    defparam pll.CLKOUTD_BYPASS = "false";
    defparam pll.DEVICE = "GW2AR-18C";
`else
    reg clk_q = 1'b0;
    realtime last_edge = 0, period = 0;
    always @(posedge clkin) begin
        if (last_edge != 0) period = $realtime - last_edge;
        last_edge = $realtime;
    end
    always begin
        if (period == 0) #1 clk_q = 1'b0;
        else #(period / 4.0) clk_q = ~clk_q;
    end
    assign clkout = clk_q;
    assign lock = (period != 0);
`endif
endmodule

// ---------------------------------------------------------------------------
// Делитель последовательного клока на 5: 140 МГц → 28 МГц пиксельных.
// ---------------------------------------------------------------------------
module ts_clkdiv5 (
    input  wire hclkin,
    input  wire resetn,
    output wire clkout
);
`ifndef SIMULATION
    CLKDIV divider (
        .CLKOUT(clkout), .HCLKIN(hclkin), .RESETN(resetn), .CALIB(1'b0)
    );
    defparam divider.DIV_MODE = "5";
    defparam divider.GSREN = "false";
`else
    reg [2:0] cnt = 0;
    reg q = 0;
    always @(posedge hclkin or negedge resetn) begin
        if (!resetn) begin
            cnt <= 0; q <= 0;
        end else if (cnt == 3'd4) begin
            cnt <= 0; q <= ~q;
        end else begin
            cnt <= cnt + 3'd1;
            if (cnt == 3'd1) q <= ~q;   // скважность 2/5 и 3/5, фронт раз в пять тактов
        end
    end
    assign clkout = q;
`endif
endmodule
