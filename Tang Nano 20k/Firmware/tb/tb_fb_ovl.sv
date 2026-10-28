// SPDX-License-Identifier: Apache-2.0
//
// Оверлеи ESP (fb_ovl): запись холста окном LINK → очередь → пакеты SDRAM по 8 слов, как у
// меню настроек на плате: холст 336×136 (строка — 168 слов, 21 пакет), ESP пишет его
// посылками по 16 слов (последняя в строке — 8) с паузами между посылками; слово посылки — раз
// в ~22 такта пикселей (LINK 26,7 МГц, 4 линии). После записи память SDRAM (модель)
// сверяется с холстом слово в слово; ошибки считаются по номеру слова в пакете.
//
// Тест появился 18.10.2026: на плате меню шло вертикальными полосами — испорчено одно слово в
// каждом пакете (шаг 16 точек холста), а прежний тест оверлеев в tb_ts_fb писал холсты по
// 1…2 пакета на строку.
`timescale 1ns/1ps
module tb_fb_ovl;
    parameter real PCLK_NS = 13.468;   // 74,25 МГц
    parameter real MCLK_NS = 16.1616;  // 61,875 МГц
    parameter int  GAP     = 22;       // тактов пикселей между словами посылки
    parameter int  PAUSE   = 400;      // тактов пикселей между посылками

    localparam logic [20:0] REGION = 21'h108000;
    localparam int W = 336, H = 136, STRIDE = ((W + 15) / 16) * 8;

    reg pclk = 1'b0, mclk = 1'b0;
    always #(PCLK_NS / 2) pclk = ~pclk;
    always #(MCLK_NS / 2) mclk = ~mclk;
    reg rst_n = 1'b0;

    reg         ow_stb = 1'b0;
    reg  [16:0] ow_addr = '0;
    reg  [31:0] ow_data = '0;
    wire        ow_ovf;

    wire        s_req, s_we, s_ack, s_wr_take, s_rd_valid;
    wire [20:0] s_addr;
    wire [2:0]  s_len;
    wire [5:0]  s_wr_idx, s_rd_idx;
    wire [31:0] s_wdata, s_rd_data;
    wire [3:0]  s_wmask;
    wire [7:0]  o_r, o_g, o_b;
    wire        o_de, o_hs, o_vs;

    fb_ovl #(.N(2), .REGION(REGION)) dut (
        .pclk(pclk), .prst_n(rst_n),
        .cfg_on(2'b00), .cfg_x2(2'b00), .cfg_p4(2'b00), .cfg_alpha('0), .cfg_x('0), .cfg_y('0),
        .cfg_w('0), .cfg_h('0), .cfg_base('0),
        .pal_we(1'b0), .pal_k(1'b0), .pal_idx(4'd0), .pal_data(21'd0),
        .ow_stb(ow_stb), .ow_addr(ow_addr), .ow_data(ow_data), .ow_ovf(ow_ovf),
        .in_r(8'd0), .in_g(8'd0), .in_b(8'd0), .in_de(1'b0), .in_hs(1'b0), .in_vs(1'b0),
        .out_r(o_r), .out_g(o_g), .out_b(o_b), .out_de(o_de), .out_hs(o_hs), .out_vs(o_vs),
        .mclk(mclk), .mrst_n(rst_n),
        .s_req(s_req), .s_we(s_we), .s_addr(s_addr), .s_len(s_len), .s_ack(s_ack),
        .s_wr_take(s_wr_take), .s_wr_idx(s_wr_idx), .s_wdata(s_wdata), .s_wmask(s_wmask),
        .s_rd_valid(s_rd_valid), .s_rd_idx(s_rd_idx), .s_rd_data(s_rd_data));

    wire        sd_cs_n, sd_ras_n, sd_cas_n, sd_we_n, sd_dq_oe, init_done;
    wire [10:0] sd_a;
    wire [1:0]  sd_ba;
    wire [3:0]  sd_dqm;
    wire [31:0] sd_dq_out, sd_dq_in;
    wire [1:0]  op_tag;
    sdram_burst #(.TAGW(2), .CLK_KHZ(61_875), .INIT_US(5)) ctrl (
        .clk(mclk), .rst_n(rst_n), .rd_phase(2'd0),
        .req(s_req), .we(s_we), .addr(s_addr), .req_len(s_len), .req_tag(2'b01), .ack(s_ack),
        .op_tag(op_tag), .wr_take(s_wr_take), .wr_idx(s_wr_idx), .wdata(s_wdata), .wmask(s_wmask),
        .rd_valid(s_rd_valid), .rd_idx(s_rd_idx), .rd_data(s_rd_data), .init_done(init_done),
        .sdram_cs_n(sd_cs_n), .sdram_ras_n(sd_ras_n), .sdram_cas_n(sd_cas_n),
        .sdram_we_n(sd_we_n), .sdram_a(sd_a), .sdram_ba(sd_ba), .sdram_dqm(sd_dqm),
        .sdram_dq_out(sd_dq_out), .sdram_dq_oe(sd_dq_oe), .sdram_dq_in(sd_dq_in));
    sdram_model #(.TCK_NS(MCLK_NS), .CL(2)) sdram (
        .clk(~mclk), .cs_n(sd_cs_n), .ras_n(sd_ras_n), .cas_n(sd_cas_n), .we_n(sd_we_n),
        .a(sd_a), .ba(sd_ba), .dqm(sd_dqm),
        .dq_from_ctrl(sd_dq_out), .dq_oe(sd_dq_oe), .dq_to_ctrl(sd_dq_in));

    // слово холста: строка и слово — чтобы любое смещение было видно
    function automatic logic [31:0] word_of(input int r, input int w);
        word_of = {8'hA5, 8'(r), 8'(w), 8'(r ^ w)};
    endfunction

    task automatic put_word(input int a, input logic [31:0] d);
        @(posedge pclk);
        ow_addr <= 17'(a); ow_data <= d; ow_stb <= 1'b1;
        @(posedge pclk);
        ow_stb <= 1'b0;
        repeat (GAP - 2) @(posedge pclk);
    endtask

    int errors = 0, by_idx [0:7];
    initial begin
        for (int i = 0; i < 8; i++) by_idx[i] = 0;
        // старое содержимое региона — не 0, чтобы «не записано» отличалось от записи
        for (int a = 0; a < H * STRIDE; a++) sdram.mem[REGION + 21'(a)] = 32'hDEAD_0000 | 32'(a & 16'hFFFF);
        #200 rst_n = 1'b1;
        wait (init_done);
        #2000;
        for (int r = 0; r < H; r++)
            for (int w = 0; w < STRIDE; w += 16) begin
                int n;
                n = STRIDE - w < 16 ? STRIDE - w : 16;
                for (int i = 0; i < n; i++) put_word(r * STRIDE + w + i, word_of(r, w + i));
                repeat (PAUSE) @(posedge pclk);
            end
        #100_000;                               // очередь и последние пакеты — в SDRAM
        for (int r = 0; r < H; r++)
            for (int w = 0; w < STRIDE; w++) begin
                logic [31:0] got;
                got = sdram.mem[REGION + 21'(r * STRIDE + w)];
                if (got !== word_of(r, w)) begin
                    errors++;
                    by_idx[w % 8]++;
                    if (errors <= 8) $display("строка %0d слово %0d: %08h, ждали %08h", r, w, got, word_of(r, w));
                end
            end
        $display("холст %0d×%0d, слов %0d, ошибок %0d; по слову пакета: %0d %0d %0d %0d %0d %0d %0d %0d",
                 W, H, H * STRIDE, errors, by_idx[0], by_idx[1], by_idx[2], by_idx[3], by_idx[4],
                 by_idx[5], by_idx[6], by_idx[7]);
        $display("очередь переполнялась %0d; SDRAM: ошибок протокола %0d", ow_ovf, sdram.errors);
        if (errors == 0 && !ow_ovf && sdram.errors == 0) $display("ТЕСТ ПРОЙДЕН");
        else $display("ТЕСТ НЕ ПРОЙДЕН");
        $finish;
    end
    initial begin
        #2_000_000_000;
        $display("ТЕСТ НЕ ПРОЙДЕН: время вышло");
        $finish;
    end
endmodule
