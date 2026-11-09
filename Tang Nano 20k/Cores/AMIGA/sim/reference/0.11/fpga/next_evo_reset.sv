// SPDX-License-Identifier: Apache-2.0
//
// Сброс Evo для ядра из кэша BL616 (ZX Next на VDAC4): пока ядро работает, плагин WC VDAC4 Cores
// держит Evo в режиме FT812 (VCONFIG #04, M_SEL = 1 — видеошина Evo молчит), а сброс Evo снимает
// M_SEL (TS-Config при сбросе гасит vdac2_msel). Взвод — M_SEL = 1 дольше 2^ARM_BIT тактов, запрос —
// M_SEL = 0 дольше 2^FIRE_BIT тактов после взвода; запрос держится до перезагрузки ПЛИС (его, как
// BOOT от ESP, исполняет BL616: JTAG Reconfig, слот 0). До взвода (ядро грузится, M_SEL ещё 0)
// запроса нет. При 28,125 МГц: ARM_BIT 18 — ~9 мс, FIRE_BIT 21 — ~75 мс.
module next_evo_reset #(
    parameter int ARM_BIT  = 18,
    parameter int FIRE_BIT = 21
) (
    input  wire clk,
    input  wire m_sel,          // FPGA73, асинхронно к clk
    output reg  req = 1'b0
);
    reg [2:0]        s = 3'b000;
    reg [FIRE_BIT:0] cnt = '0;  // тактов с последней смены уровня (стоит на 2^FIRE_BIT)
    reg              armed = 1'b0;

    always @(posedge clk) begin
        s <= {s[1:0], m_sel};
        if (s[2] != s[1])
            cnt <= '0;
        else if (!cnt[FIRE_BIT])
            cnt <= cnt + 1'b1;
        if (s[2] && s[1] && cnt[ARM_BIT])
            armed <= 1'b1;
        if (!s[2] && !s[1] && cnt[FIRE_BIT] && armed)
            req <= 1'b1;
    end
endmodule
