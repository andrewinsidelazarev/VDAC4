// SPDX-License-Identifier: GPL-3.0-or-later
// События звука для ESP (03.11.2026): каждая запись в порт YM2612, каждая запись в PSG и сброс YM —
// одно 32-битное слово с меткой времени в тактах синтеза YM (1,278 МГц, 20 бит — 0,82 с по кругу).
// ESP повторяет записи в своём эмуляторе в те же моменты, отсюда звук без потоков отсчётов по LINK.
//   [31:30] вид: 0 — порт YM, 1 — PSG, 2 — сброс YM (data[0] = 1 — сброс снят)
//   [29:28] порт YM (A1, A0)
//   [27:8]  метка времени
//   [7:0]   байт
module md_snd_events(
    input             clk,
    input             synth,      // такт синтеза YM (md_ym_stub)
    input             ym_rst,     // сброс YM (уровень)
    input             ym_wr,      // запись в порт YM, один такт
    input       [1:0] ym_port,
    input       [7:0] ym_data,
    input             psg_wr_n,   // запись в PSG (уровень на весь цикл шины)
    input       [7:0] psg_data,
    output reg        push=0,
    output reg [31:0] word=0,
    output reg [19:0] stamp=0     // текущее время YM: LINK отдаёт его ESP (звук идёт и без событий)
);

always @(posedge clk) if (synth) stamp<=stamp+20'd1;

// Записи YM и PSG идут с разных шин и могут совпасть в одном такте: PSG ждёт в защёлке такт.
reg psg_d=1, rst_d=1, psg_wait=0, rst_wait=0;
reg [7:0] psg_byte=0;
always @(posedge clk) begin
    psg_d<=psg_wr_n; rst_d<=ym_rst;
    if (psg_d && !psg_wr_n) begin psg_wait<=1; psg_byte<=psg_data; end
    if (rst_d!=ym_rst) rst_wait<=1;
    push<=0;
    if (ym_wr) begin
        push<=1; word<={2'd0,ym_port,stamp,ym_data};
    end else if (psg_wait) begin
        push<=1; word<={2'd1,2'd0,stamp,psg_byte}; psg_wait<=0;
    end else if (rst_wait) begin
        push<=1; word<={2'd2,2'd0,stamp,7'd0,!ym_rst}; rst_wait<=0;
    end
end

endmodule
