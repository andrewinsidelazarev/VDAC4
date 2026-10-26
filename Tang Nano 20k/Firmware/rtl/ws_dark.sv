// SPDX-License-Identifier: Apache-2.0
//
// RGB-светодиод WS2812 модуля Tang гасится: его DIN — на том же выводе FPGA 79, что и MISO
// FT812 (так разведён модуль, docs/a2-netlist-audit.md, замечание 6). Биты MISO при обмене
// Evo с FT812 светодиод принимает за цвет и держит последний, пока не придёт другой, — и после
// ухода в ядро TS-Config (там MISO — 0) светил им дальше.
//
// Здесь: FT_CS_N поднят T_RST (сброс WS2812, ≥ 280 мкс) — на вывод один раз «чёрный» (24
// нулевых бита: 0,32 мкс высокий, 0,9 мкс низкий), дальше — низкий; следующий — только после
// нового выбора FT812. Вывод на шину Evo выходит через U8 только при выбранном FT812 или ESP,
// так что при поднятом FT_CS эти импульсы Evo не видит; опущен FT_CS — на выводе сразу MISO
// (выбор — по самому выводу, без синхронизатора). После загрузки ядра — тоже «чёрный»: ядро
// TS-Config гасит светодиод при каждом возврате из ядра FT812.
module ws_dark #(
    parameter int CLK_KHZ = 61_875
) (
    input  wire clk,
    input  wire rst_n,
    input  wire cs_n,                       // FT_CS_N с вывода
    input  wire miso,                       // MISO FT812 (идёт на вывод при опущенном FT_CS_N)
    output wire pin                         // FPGA 79
);
    localparam int T0H  = CLK_KHZ * 32 / 100_000;   // 0,32 мкс
    localparam int T0L  = CLK_KHZ * 90 / 100_000;   // 0,90 мкс
    localparam int T_RST = CLK_KHZ * 320 / 1000;    // 320 мкс
    localparam int TW   = $clog2(T0L + 1);
    localparam int IW   = $clog2(T_RST + 1);

    reg  [1:0]    cs_s;
    reg  [IW-1:0] idle;                     // тактов с поднятым FT_CS_N
    reg           sent;                     // «чёрный» отдан после последнего выбора FT812
    reg           run, hi;
    reg  [4:0]    nbit;
    reg  [TW-1:0] t;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            cs_s <= 2'b11; idle <= '0; sent <= 1'b0; run <= 1'b0; hi <= 1'b0; nbit <= '0; t <= '0;
        end else begin
            cs_s <= {cs_s[0], cs_n};
            if (!cs_s[1]) begin
                // FT812 выбран: обмен идёт, кадр WS2812 — заново после него
                idle <= '0; sent <= 1'b0; run <= 1'b0; hi <= 1'b0;
            end else if (run) begin
                if (t != '0) t <= t - 1'b1;
                else if (hi) begin
                    hi <= 1'b0;
                    t  <= TW'(T0L - 1);
                end else if (nbit == 5'd23) begin
                    run  <= 1'b0;
                    sent <= 1'b1;
                end else begin
                    nbit <= nbit + 5'd1;
                    hi   <= 1'b1;
                    t    <= TW'(T0H - 1);
                end
            end else if (!sent) begin
                if (idle == IW'(T_RST)) begin
                    run  <= 1'b1;
                    nbit <= '0;
                    hi   <= 1'b1;
                    t    <= TW'(T0H - 1);
                end else idle <= idle + 1'b1;
            end
        end
    end

    assign pin = cs_n ? hi : miso;
endmodule
