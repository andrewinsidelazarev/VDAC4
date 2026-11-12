// SPDX-License-Identifier: Apache-2.0
//
// Стенд ядра видео 1.47 целиком (ts_fb_core HAS_VIDEO, как в vvid_top) — пути видео с карты Tang и
// флешки USB, по LINK, как их ведёт ESP:
//   1. VERSION, MF_STAT пуст;
//   2. окно MF_DATA (0x40000…): 300 слов транзакциями по 16 → медиа-FIFO в SDRAM (слово k — по
//      адресу FIFO_BASE + k), флаг потери 0;
//   3. мост microSD (SD_* с 0x110, TX — окно 0x2000): CMD18, STREAM 2 блоков → медиа-FIFO следом
//      (256 слов); CMD12; CMD17 — блок в RX, чтение окном 0x3000;
//   4. MF_STAT — заполнение: разбор берёт слова (содержимое ему — мусор, это не важно), в конце 0.
// Ожидается: «ТЕСТ ПРОЙДЕН».
//
//   iverilog -g2012 -DSIMULATION -DVP_PLAIN -I rtl -I third_party/core_jpeg/src_v -s tb_video_link ...
//   (scripts/run_fb_sim.sh video)
`timescale 1ns/1ps
module tb_video_link;
    localparam real MEM_NS  = 16.162;         // 61,875 МГц
    localparam real PIX_NS  = 13.468;         // 74,25 МГц
    localparam real LINK_NS = 37.5;           // LINK SCK 26,67 МГц, как у ESP

    reg clk_mem = 1'b0, pixel_clk = 1'b0, rst_n = 1'b0;
    always #(MEM_NS / 2.0) clk_mem = ~clk_mem;
    always #(PIX_NS / 2.0) pixel_clk = ~pixel_clk;

    // LINK (как в tb_ts_fb)
    reg        lk_sck = 1'b0, lk_cs_n = 1'b1, lk_moe = 1'b0;
    reg  [3:0] lk_mo = 4'hF;
    wire [3:0] lk_so;
    wire       lk_soe;
    wire [3:0] lk_io = lk_moe ? lk_mo : (lk_soe ? lk_so : 4'hF);

    // microSD Tang
    wire tf_cs_n, tf_sck, tf_mosi, tf_miso;
    sd_card_model #(.BLOCKS(16), .NAC(4), .BUSY(4)) tfcard (
        .cs_n(tf_cs_n), .sck(tf_sck), .mosi(tf_mosi), .miso(tf_miso)
    );

    wire        sd_cs_n, sd_ras_n, sd_cas_n, sd_we_n, sd_dq_oe, mem_ready;
    wire [10:0] sd_a;
    wire [1:0]  sd_ba;
    wire [3:0]  sd_dqm;
    wire [31:0] sd_dq_out, sd_dq_in;
    localparam int OUT_W = 1280, OUT_H = 720;

    ts_fb_core #(
        .OUT_W(OUT_W), .OUT_H(OUT_H), .IMG_X((OUT_W - 720) / 2), .IMG_Y((OUT_H - 576) / 2),
        .H_FP(440), .H_SYNC(40), .H_BP(220), .V_FP(5), .V_SYNC(5), .V_BP(20),
        .HS_NEG(1'b0), .VS_NEG(1'b0), .PIX_KHZ(74_250), .MEM_KHZ(61_875),
        .SDRAM_INIT_US(20), .DEF_SRC(2'd0), .DEF_RD(2'd1), .CALIBRATE(1'b0),
        .SCALE(1'b1), .HDMI(1'b1), .FLASH_BOOT_US(20),
        .BOOT_SLOT(8'd3), .BOOT_NEXT(8'd0), .HAS_EVO(1'b0), .HAS_FT(1'b0), .HAS_VIDEO(1'b1),
        .SWITCH_IDLE_MS(2), .SD_FAST(1'b0)
    ) core (
        .clk_cap(1'b0), .rst_cap_n(1'b0),
        .clk_mem(clk_mem), .rst_mem_n(rst_n),
        .pixel_clk(pixel_clk), .rst_pix_n(rst_n),
        .clk_sd(1'b0), .rst_sd_n(1'b0),
        .blank(1'b1), .btn(2'b00), .boot_req(), .cap_clk_phase(),
        .link_sck(lk_sck), .link_cs_n(lk_cs_n), .link_io_in(lk_io), .link_io_out(lk_so), .link_io_oe(lk_soe),
        .ft_sck(1'b0), .ft_cs_n(1'b1), .ft_mosi(1'b0), .ft_miso(), .ft_int_n(),
        .cp_sck(1'b0), .cp_cs_n(1'b1), .cp_mosi(1'b0), .cp_miso(), .cp_irq_n(),
        .flash_sck(), .flash_cs_n(), .flash_mosi(), .flash_miso(1'b1),
        .sd_cs_n(tf_cs_n), .sd_sck(tf_sck), .sd_mosi(tf_mosi), .sd_miso(tf_miso),
        .mux_d(8'd0), .hsync_in(1'b0), .vsync_in(1'b0), .mux_sel(),
        .sdram_cs_n(sd_cs_n), .sdram_ras_n(sd_ras_n), .sdram_cas_n(sd_cas_n), .sdram_we_n(sd_we_n),
        .sdram_a(sd_a), .sdram_ba(sd_ba), .sdram_dqm(sd_dqm),
        .sdram_dq_out(sd_dq_out), .sdram_dq_oe(sd_dq_oe), .sdram_dq_in(sd_dq_in), .sdram_dly(),
        .sdram_dly_df(1'b0), .sdram_phase(),
        .vid_r(), .vid_g(), .vid_b(), .vid_de(), .vid_hs(), .vid_vs(),
        .tmds_blue(), .tmds_green(), .tmds_red(),
        .cap_overflow(), .frames_written(), .frames_rejected(), .fetch_late(),
        .have_frame(), .mem_ready(mem_ready), .raster_locked(), .raster_mode(),
        .raster_line_cycles(), .raster_lines(), .raster_hs_cycles(),
        .aligned_h(), .aligned_v(), .bb_valid(), .bb_x0(), .bb_x1(), .bb_y0(), .bb_y1(),
        .pattern_on(), .chk_err(), .chk_tick()
    );

    // память — по адресу слова логики (строка, банк, столбец — как раскладывает sdram_burst)
    sdram_cyc #(.CL(2)) sdram (
        .clk(~clk_mem), .cs_n(sd_cs_n), .ras_n(sd_ras_n), .cas_n(sd_cas_n), .we_n(sd_we_n),
        .a(sd_a), .ba(sd_ba), .dqm(sd_dqm),
        .dq_from_ctrl(sd_dq_out), .dq_oe(sd_dq_oe), .dq_to_ctrl(sd_dq_in)
    );

    integer errors = 0, checks = 0;
    task automatic check(input string what, input [31:0] got, input [31:0] want);
        begin
            checks = checks + 1;
            if (got !== want) begin
                errors = errors + 1;
                $display("ОШИБКА %0s: %08h, ожидалось %08h", what, got, want);
            end
        end
    endtask

    // ------------------------------------------------------------------
    // LINK: транзакции ведущего, как в tb_ts_fb (0x01 — запись nw слов, 0x02 — чтение)
    // ------------------------------------------------------------------
    localparam int LK_DUMMY = 8;
    reg [31:0] lk_words [0:15];
    task automatic lk_clock;
        begin #(LINK_NS / 2.0) lk_sck = 1'b1; #(LINK_NS / 2.0) lk_sck = 1'b0; end
    endtask
    task automatic link_xfer(input [7:0] cmd, input [31:0] addr, input integer nw);
        integer i, w;
        reg [31:0] v;
        begin
            lk_cs_n = 1'b0; lk_moe = 1'b1;
            #(LINK_NS);
            for (i = 7; i >= 0; i = i - 1) begin lk_mo = {3'b111, cmd[i]}; lk_clock(); end
            for (i = 7; i >= 0; i = i - 1) begin lk_mo = addr[i*4 +: 4]; lk_clock(); end
            if (cmd == 8'h01) begin
                for (w = 0; w < nw; w = w + 1)
                    for (i = 7; i >= 0; i = i - 1) begin lk_mo = lk_words[w][i*4 +: 4]; lk_clock(); end
            end else begin
                lk_moe = 1'b0;
                for (i = 0; i < LK_DUMMY; i = i + 1) lk_clock();
                for (w = 0; w < nw; w = w + 1) begin
                    v = 'x;
                    for (i = 0; i < 8; i = i + 1) begin
                        #(LINK_NS / 2.0) v = {v[27:0], lk_io}; lk_sck = 1'b1;
                        #(LINK_NS / 2.0) lk_sck = 1'b0;
                    end
                    lk_words[w] = v;
                end
            end
            #(LINK_NS / 2.0);
            lk_cs_n = 1'b1; lk_moe = 1'b0; lk_mo = 4'hF;
            #(LINK_NS * 2);
        end
    endtask
    task automatic lk_wr(input [31:0] addr, input [31:0] v);
        begin lk_words[0] = v; link_xfer(8'h01, addr, 1); end
    endtask
    task automatic lk_rd(input [31:0] addr, output [31:0] v);
        begin link_xfer(8'h02, addr, 1); v = lk_words[0]; end
    endtask

    // мост к microSD ядра видео: регистры с 0x110
    localparam logic [31:0] SD_CTRL = 32'h110, SD_PTR = 32'h114, SD_CMD = 32'h118, SD_STRM = 32'h11C,
                            MF_STAT = 32'h124;
    reg [31:0] sd_st;
    task automatic sd_op(input integer o, input integer n);
        integer t;
        begin
            lk_wr(SD_CMD, {9'd0, o[2:0], n[19:0]});
            t = 0;
            do begin lk_rd(SD_CMD, sd_st); t = t + 1; end while (sd_st[0] && t < 20000);
            if (t >= 20000) begin errors = errors + 1; $display("ОШИБКА: мост не закончил операцию"); end
        end
    endtask
    // команда: 0xFF, 6 байтов (TX — окно 0x2000, байт 0 — старший), затем WAIT — R1
    task automatic sd_cmd(input [5:0] idx, input [31:0] arg, input [7:0] crc, output [7:0] r1);
        begin
            lk_words[0] = {8'hFF, 2'b01, idx, arg[31:16]};
            lk_words[1] = {arg[15:0], crc, 8'hFF};
            link_xfer(8'h01, 32'h2000, 2);
            lk_wr(SD_PTR, {5'd0, 11'd514, 7'd0, 9'd0});
            sd_op(0, 7);
            sd_op(2, 16);
            r1 = sd_st[15:8];
        end
    endtask

    integer   k, bad, i;
    reg [31:0] v, want;
    reg [7:0]  r1;
    initial begin
        #200; rst_n = 1'b1;
        wait (mem_ready);
        #2000;

        // 1. личность и версия, вход пуст
        lk_rd(32'h004, v);  check("VERSION", v, 32'h0501_012F);
        lk_rd(MF_STAT, v);  check("MF_STAT после сброса", v, 32'd0);
        lk_rd(SD_CMD, v);   check("SD_STAT: мост есть", v[31:16], 16'h5D01);

        // 2. окно MF_DATA: 300 слов транзакциями по 16 (адрес внутри окна не важен)
        for (k = 0; k < 300; k = k + 16) begin
            for (i = 0; i < 16; i = i + 1) lk_words[i] = 32'hA5000000 ^ ((k + i) * 32'h00010203);
            link_xfer(8'h01, 32'h40000 + 4 * k, (300 - k) < 16 ? 300 - k : 16);
        end
        #20000;                                   // пауза входа: позиция объявлена, слова в SDRAM
        lk_rd(MF_STAT, v);
        check("MF_DATA: потерь нет", v[23], 1'b0);
        bad = 0;
        for (k = 0; k < 300; k = k + 1)
            if (sdram.mem[k] !== (32'hA5000000 ^ (k * 32'h00010203))) begin
                if (bad < 3) $display("  SDRAM слово %0d: %08h", k, sdram.mem[k]);
                bad = bad + 1;
            end
        check("MF_DATA: слов в SDRAM не тех", bad, 0);

        // 3. мост: быстрый SCK, CS; CMD18 с блока 4; STREAM 2 блоков
        lk_wr(SD_CTRL, {23'd0, 1'b1, 8'd1});
        sd_cmd(6'd18, 32'd4, 8'hFF, r1);  check("CMD18 R1", r1, 8'h00);
        lk_wr(SD_STRM, 32'd2);
        sd_op(5, 10000);
        check("STREAM: STAT", sd_st[2:0], 3'b000);
        check("STREAM: маркер", sd_st[15:8], 8'hFE);
        #20000;
        bad = 0;
        for (k = 0; k < 256; k = k + 1) begin
            want = 0;
            for (i = 0; i < 4; i = i + 1) want[8 * i +: 8] = tfcard.mem[4 * 512 + 4 * k + i];
            if (sdram.mem[300 + k] !== want) begin
                if (bad < 3) $display("  SDRAM слово %0d: %08h, ожидалось %08h", 300 + k, sdram.mem[300 + k], want);
                bad = bad + 1;
            end
        end
        check("STREAM: слов в SDRAM не тех", bad, 0);

        // CMD12: команда, байт мусора, R1, конец «занята»
        lk_words[0] = {8'hFF, 8'h4C, 16'h0000};
        lk_words[1] = {16'h0000, 8'h61, 8'hFF};
        link_xfer(8'h01, 32'h2000, 2);
        sd_op(0, 7); sd_op(1, 1); sd_op(2, 16);
        check("CMD12 R1", sd_st[15:8], 8'h00);
        sd_op(4, 10000);
        check("CMD12: карта свободна", sd_st[1:0], 2'b00);

        // CMD17 блока 9 — в RX, чтение окном 0x3000
        sd_cmd(6'd17, 32'd9, 8'hFF, r1); check("CMD17 R1", r1, 8'h00);
        lk_wr(SD_PTR, {5'd0, 11'd514, 7'd0, 9'd0});
        sd_op(3, 10000);
        check("READ: маркер", sd_st[15:8], 8'hFE);
        bad = 0;
        for (k = 0; k < 128; k = k + 16) begin
            link_xfer(8'h02, 32'h3000 + 4 * k, 16);
            for (i = 0; i < 16; i = i + 1)
                if (lk_words[i] !== {tfcard.mem[9 * 512 + 4 * (k + i)], tfcard.mem[9 * 512 + 4 * (k + i) + 1],
                                     tfcard.mem[9 * 512 + 4 * (k + i) + 2], tfcard.mem[9 * 512 + 4 * (k + i) + 3]})
                    bad = bad + 1;
        end
        check("RX окном 0x3000: слов не тех", bad, 0);

        // 4. разбор взял все слова (заполнение 0), потерь нет
        lk_rd(MF_STAT, v);
        $display("MF_STAT: %08h", v);
        check("MF_STAT: всё разобрано, потерь нет", v[23:0], 24'd0);
        lk_wr(MF_STAT, 32'd0);
        check("карта: плохих команд", tfcard.n_bad, 0);

        $display("проверок %0d, ошибок %0d", checks, errors);
        if (errors) $display("ТЕСТ НЕ ПРОЙДЕН");
        else        $display("ТЕСТ ПРОЙДЕН");
        $finish;
    end
    initial begin
        #20_000_000;
        $display("ОШИБКА: стенд не закончил за 20 мс"); $display("ТЕСТ НЕ ПРОЙДЕН"); $finish;
    end
endmodule
