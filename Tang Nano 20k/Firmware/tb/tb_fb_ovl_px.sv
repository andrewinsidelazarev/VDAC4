// SPDX-License-Identifier: Apache-2.0
//
// Оверлеи ESP (fb_ovl): вывод точек. Холсты лежат в модели SDRAM (запись окном LINK проверяет
// tb_fb_ovl); растр — маленький, картинка на входе — узор от координат. Оверлей 0 — RGB565 с
// прозрачностью 9 (×1), оверлей 1 — режим 4 бит (1.42): номер цвета палитры, у цвета своя
// непрозрачность 0…16 (0 — картинка, 16 — цвет). Каждая точка вывода сверяется с формулой fb_ovl:
// v + ⌊(c − v)·a1/16⌋ по каналам 8 бит, оверлей 1 — поверх оверлея 0. Кадры: 4 бит ×1, затем ×2,
// затем оверлей 0 тоже в 4 бит (другая палитра) — сверка трёх последних кадров каждого набора.
`timescale 1ns/1ps
module tb_fb_ovl_px;
    parameter real PCLK_NS = 13.468;   // 74,25 МГц
    parameter real MCLK_NS = 16.1616;  // 61,875 МГц

    localparam logic [20:0] REGION = 21'h108000;
    localparam int HA = 160, HT = 220, VA = 24, VT = 32, VS0 = 0, VS1 = 2, VA0 = 6;
    localparam int B0 = 0, B1 = 4096;              // начала холстов (слова региона)

    reg pclk = 1'b0, mclk = 1'b0;
    always #(PCLK_NS / 2) pclk = ~pclk;
    always #(MCLK_NS / 2) mclk = ~mclk;
    reg rst_n = 1'b0;

    // настройки оверлеев
    reg  [1:0]  on = 2'b00, x2 = 2'b00, p4 = 2'b00;
    reg  [7:0]  alpha = 8'h00;
    reg  [21:0] ox = '0, ow = '0;
    reg  [19:0] oy = '0, oh = '0;
    reg  [33:0] obase = '0;
    reg         pal_we = 1'b0, pal_k = 1'b0;
    reg  [3:0]  pal_idx = '0;
    reg  [20:0] pal_data = '0;

    // растр
    reg  [8:0]  hx = '0;
    reg  [5:0]  vy = '0;
    wire        de = hx < HA && vy >= VA0 && vy < VA0 + VA;
    wire        hs = hx >= HA + 8 && hx < HA + 24;
    wire        vs = vy >= VS0 && vy < VS1;
    wire [7:0]  ar = 8'((hx * 3) & 255);
    wire [7:0]  ag = 8'(((vy - VA0) * 7) & 255);
    wire [7:0]  ab = 8'(hx ^ (vy - VA0));
    always @(posedge pclk) begin
        hx <= hx == HT - 1 ? 9'd0 : hx + 9'd1;
        if (hx == HT - 1) vy <= vy == VT - 1 ? 6'd0 : vy + 6'd1;
    end

    wire        s_req, s_we, s_ack, s_wr_take, s_rd_valid;
    wire [20:0] s_addr;
    wire [2:0]  s_len;
    wire [5:0]  s_wr_idx, s_rd_idx;
    wire [31:0] s_wdata, s_rd_data;
    wire [3:0]  s_wmask;
    wire [7:0]  o_r, o_g, o_b;
    wire        o_de, o_hs, o_vs, ow_ovf;

    fb_ovl #(.N(2), .REGION(REGION), .HS_NEG(1'b0), .VS_NEG(1'b0)) dut (
        .pclk(pclk), .prst_n(rst_n),
        .cfg_on(on), .cfg_x2(x2), .cfg_p4(p4), .cfg_alpha(alpha), .cfg_x(ox), .cfg_y(oy), .cfg_w(ow),
        .cfg_h(oh), .cfg_base(obase),
        .pal_we(pal_we), .pal_k(pal_k), .pal_idx(pal_idx), .pal_data(pal_data),
        .ow_stb(1'b0), .ow_addr(17'd0), .ow_data(32'd0), .ow_ovf(ow_ovf),
        .in_r(ar), .in_g(ag), .in_b(ab), .in_de(de), .in_hs(hs), .in_vs(vs),
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

    // --- холсты и палитры (эталон) ---------------------------------------------------------------
    function automatic logic [15:0] c565(input int cx, input int cy);      // точка холста RGB565
        c565 = {5'(cx), 6'(cy * 3 + 17), 5'(cx ^ cy)};
    endfunction
    function automatic logic [3:0] nib(input int cx, input int cy);        // номер цвета, 4 бит
        nib = 4'(cx * 3 + cy * 5);
    endfunction
    logic [20:0] pal [0:1][0:15];
    function automatic logic [20:0] pal_of(input int k, input int i);
        logic [4:0] a1;
        a1 = i == 0 ? 5'd0 : i == 1 ? 5'd16 : 5'((i * 3 + k * 5) % 17);
        pal_of = {a1, 5'(i * 2 + k), 6'(63 - i * 4), 5'(i + 7 * k)};
    endfunction

    // настройки набора (в эталон — те же)
    int e_on [0:1], e_x2 [0:1], e_p4 [0:1], e_a1 [0:1], e_x [0:1], e_y [0:1], e_w [0:1], e_h [0:1];
    int e_base [0:1];

    function automatic logic [7:0] ex5(input logic [4:0] v); ex5 = {v, v[4:2]}; endfunction
    function automatic logic [7:0] ex6(input logic [5:0] v); ex6 = {v, v[5:4]}; endfunction
    function automatic logic [7:0] mix(input logic [7:0] v, input logic [7:0] c, input int a1);
        int p;
        p = (int'(c) - int'(v)) * a1;
        mix = 8'(int'(v) + (p >>> 4));
    endfunction

    // ожидаемая точка вывода (x, y активной области)
    task automatic expect_px(input int x, input int y, output logic [23:0] rgb);
        logic [7:0] r, g, b;
        r = 8'((x * 3) & 255); g = 8'((y * 7) & 255); b = 8'(x ^ y);
        for (int k = 0; k < 2; k++) begin
            int sw, sh, dx, dy, cx, cy, a1;
            logic [15:0] c;
            sw = e_x2[k] ? 2 * e_w[k] : e_w[k];
            sh = e_x2[k] ? 2 * e_h[k] : e_h[k];
            dx = x - e_x[k]; dy = y - e_y[k];
            if (e_on[k] && dx >= 0 && dx < sw && dy >= 0 && dy < sh) begin
                cx = e_x2[k] ? dx / 2 : dx;
                cy = e_x2[k] ? dy / 2 : dy;
                if (e_p4[k]) begin
                    c = pal[k][nib(cx, cy)][15:0];
                    a1 = pal[k][nib(cx, cy)][20:16];
                end else begin
                    c = c565(cx, cy);
                    a1 = e_a1[k];
                end
                r = mix(r, ex5(c[15:11]), a1);
                g = mix(g, ex6(c[10:5]), a1);
                b = mix(b, ex5(c[4:0]), a1);
            end
        end
        rgb = {r, g, b};
    endtask

    // холст оверлея k — в SDRAM по настройкам набора
    task automatic fill(input int k);
        int stride;
        stride = e_p4[k] ? ((e_w[k] + 63) / 64) * 8 : ((e_w[k] + 15) / 16) * 8;
        for (int cy = 0; cy < e_h[k]; cy++)
            for (int w = 0; w < stride; w++) begin
                logic [31:0] d;
                if (e_p4[k]) for (int i = 0; i < 8; i++) d[i*4 +: 4] = nib(w * 8 + i, cy);
                else d = {c565(w * 2 + 1, cy), c565(w * 2, cy)};
                sdram.mem[REGION + 21'(e_base[k] + cy * stride + w)] = d;
            end
    endtask

    task automatic apply();
        for (int k = 0; k < 2; k++) begin
            on[k] = e_on[k][0]; x2[k] = e_x2[k][0]; p4[k] = e_p4[k][0];
            alpha[k*4 +: 4] = 4'(e_a1[k] - 1);
            ox[k*11 +: 11] = 11'(e_x[k]); oy[k*10 +: 10] = 10'(e_y[k]);
            ow[k*11 +: 11] = 11'(e_w[k]); oh[k*10 +: 10] = 10'(e_h[k]);
            obase[k*17 +: 17] = 17'(e_base[k]);
        end
    endtask

    task automatic load_pal(input int k);
        for (int i = 0; i < 16; i++) begin
            pal[k][i] = pal_of(k, i);
            @(posedge pclk);
            pal_we <= 1'b1; pal_k <= 1'(k); pal_idx <= 4'(i); pal_data <= pal_of(k, i);
            @(posedge pclk);
            pal_we <= 1'b0;
        end
    endtask

    // --- сверка кадров вывода --------------------------------------------------------------------
    int px_x = 0, px_y = 0, frame = 0, check_from = 1 << 30, errors = 0, checked = 0;
    reg o_de_q = 1'b0, o_vs_q = 1'b0;
    always @(posedge pclk) begin
        o_de_q <= o_de; o_vs_q <= o_vs;
        if (o_vs && !o_vs_q) begin
            frame <= frame + 1;
            px_y <= 0;
        end else if (o_de_q && !o_de) px_y <= px_y + 1;
        if (o_de) begin
            if (frame >= check_from) begin
                logic [23:0] want;
                expect_px(px_x, px_y, want);
                checked++;
                if ({o_r, o_g, o_b} !== want) begin
                    errors++;
                    if (errors <= 10)
                        $display("кадр %0d точка (%0d, %0d): %06h, ждали %06h", frame, px_x, px_y,
                                 {o_r, o_g, o_b}, want);
                end
            end
            px_x <= px_x + 1;
        end else px_x <= 0;
    end

    task automatic run_set(input string name);
        int f0, e0, c0;
        apply();
        @(posedge o_vs);                      // настройки берутся в начале кадра
        f0 = frame; e0 = errors; c0 = checked;
        check_from = frame + 2;               // строки 0 и 1 заказаны в гашении этого кадра
        wait (frame == f0 + 5);
        check_from = 1 << 30;
        $display("%s: точек %0d, ошибок %0d", name, checked - c0, errors - e0);
    endtask

    initial begin
        for (int a = 0; a < 16384; a++) sdram.mem[REGION + 21'(a)] = 32'hDEAD_BEEF;
        e_on[0] = 1; e_x2[0] = 0; e_p4[0] = 0; e_a1[0] = 10; e_x[0] = 10; e_y[0] = 2;
        e_w[0] = 40; e_h[0] = 10; e_base[0] = B0;
        e_on[1] = 1; e_x2[1] = 0; e_p4[1] = 1; e_a1[1] = 16; e_x[1] = 30; e_y[1] = 4;
        e_w[1] = 100; e_h[1] = 12; e_base[1] = B1;
        fill(0); fill(1);
        #200 rst_n = 1'b1;
        wait (init_done);
        load_pal(1);
        run_set("RGB565 (прозрачность 9) и 4 бит ×1");
        e_x2[1] = 1; e_w[1] = 50; e_h[1] = 6; e_x[1] = 60; e_y[1] = 8; e_base[1] = B1 + 2048;
        fill(1);                              // шаг строки другой (8 слов) — холст заново
        run_set("4 бит ×2");
        e_p4[0] = 1; e_base[0] = 8192; e_w[0] = 70; e_x[0] = 3; e_y[0] = 0; e_h[0] = 9;
        fill(0);
        load_pal(0);
        run_set("оба оверлея в 4 бит");
        $display("SDRAM: ошибок протокола %0d", sdram.errors);
        if (errors == 0 && checked > 0 && sdram.errors == 0) $display("ТЕСТ ПРОЙДЕН");
        else $display("ТЕСТ НЕ ПРОЙДЕН");
        $finish;
    end
    initial begin
        #200_000_000;
        $display("ТЕСТ НЕ ПРОЙДЕН: время вышло");
        $finish;
    end
endmodule
