// SPDX-License-Identifier: Apache-2.0
//
// ПЗУ смешения двух кадров (FRAME BLEND, fb_core): N независимых портов чтения таблицы
// fb_avg_q (rtl/fb_avg_tab.svh, scripts/gen_avg_tab.py), выход — через такт после адреса.
// fb_core смешивает слово строки (две точки × три канала) за такт — шесть портов.
// Каждый порт — своё ПЗУ 4096 × 5 бит в BSRAM.
//
// В симуляции таблица — массив, заполненный один раз: case на 4096 значений в каждом такте
// у Icarus — линейный перебор, и стенд шёл в десятки раз медленнее.
//
module fb_avg_rom #(
    parameter int N = 6
) (
    input  wire            clk,
    input  wire [N*12-1:0] addr,         // порт k: [k*12 +: 12] = {канал кадра 1, канал кадра 2}
    output wire [N*5-1:0]  q
);
    `include "fb_avg_tab.svh"

`ifdef SIMULATION
    reg [4:0] tab [0:4095];
    initial for (int i = 0; i < 4096; i = i + 1) tab[i] = fb_avg_q(12'(i));
`endif

    genvar k;
    for (k = 0; k < N; k = k + 1) begin : g_port
        (* syn_romstyle = "block_rom" *) reg [4:0] q_k;
`ifdef SIMULATION
        always_ff @(posedge clk) q_k <= tab[addr[k*12 +: 12]];
`else
        always_ff @(posedge clk) q_k <= fb_avg_q(addr[k*12 +: 12]);
`endif
        assign q[k*5 +: 5] = q_k;
    end
endmodule
