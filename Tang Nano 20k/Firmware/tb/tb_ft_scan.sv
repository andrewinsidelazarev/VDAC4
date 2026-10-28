`timescale 1ns/1ps
//
// Вывод кадра FT812 через масштаб 720p: fb_core (режим FT, чтение запросами по 8 пакетов) →
// fb_line_ram → fb_scanout (уменьшение 1024×768 → 960×720 или окна ASPECT/SIZE) →
// RGB. Кадр RGB565 — из растеризатора (tb_ft_render: ft_frame.hex) в модели SDRAM с
// FB_BASE. Выход третьего кадра — в scan_out.hex (строка: 1280 точек RRGGBB); сверка с
// эталоном развёртки — scripts/ft_scan_check.py.
// OUT768 = 1 — вывод ядра FT812 с 1.41: 1366×768 с сокращённым гашением (72 МГц, строка 1500,
// кадр 800), строка scan_out.hex — 1366 точек.
//
//   +frame=ft_frame.hex
module tb_ft_scan;
    parameter bit ASPECT = 1'b0;
    parameter int SIZE = 0;
    parameter int SRC_W = 1024, SRC_H = 768;
    parameter bit FT_W512 = (SRC_W <= 512);        // кадр видео 512×384 (ядро видео)
    parameter bit OUT768 = 1'b0;                   // вывод 1366×768 (ядро FT812 с 1.41)

    reg clk_mem = 1'b0, pix = 1'b0, rst_n = 1'b0;
    always #(16.16 / 2) clk_mem = ~clk_mem;        // 61,875 МГц
    always #((OUT768 ? 13.889 : 13.468) / 2) pix = ~pix;   // 72 / 74,25 МГц

    localparam logic [20:0] FB_BASE = 21'h120000;

    wire        req_toggle, req_first, req_two, req_ft;
    wire [9:0]  req_line;
    wire        lb_we;
    wire [10:0] lb_waddr;
    wire [31:0] lb_wdata;
    wire [9:0]  ra_e, ra_o, rb_e, rb_o;
    wire [15:0] qa_e, qa_o, qb_e, qb_o;
    wire        mem_req, mem_we, mem_tag, mem_ack, mem_op_tag, wr_take, rd_valid, init_done;
    wire [2:0]  mem_len;
    wire [20:0] mem_addr;
    wire [31:0] mem_wdata, rd_data;
    wire [5:0]  wr_idx, rd_idx;
    wire        ft_front, ft_showing, fetch_late;

    fb_core #(.H_ACTIVE(720), .V_ACTIVE(576), .BASE(FB_BASE)) fb (
        .clk(clk_mem), .rst_n(rst_n),
        .fifo_rd(), .fifo_q(33'd0), .fifo_empty(1'b1),
        .req_toggle(req_toggle), .req_line(req_line), .req_first(req_first),
        .req_two(req_two), .req_ft(req_ft),
        .ft_mode(1'b1), .ft_pub_v(init_done), .ft_pub_buf(1'b0), .ft_words(10'((SRC_W + 1) / 2)),
        .ft_front(ft_front), .ft_showing(ft_showing),
        .lb_we(lb_we), .lb_addr(lb_waddr), .lb_wdata(lb_wdata),
        .mem_req(mem_req), .mem_we(mem_we), .mem_addr(mem_addr), .mem_tag(mem_tag),
        .mem_len(mem_len), .mem_ack(mem_ack), .mem_ack_tag(mem_op_tag), .mem_wr_tag(mem_op_tag),
        .mem_rd_tag(mem_op_tag),
        .mem_wr_take(wr_take), .mem_wr_idx(wr_idx[2:0]), .mem_wdata(mem_wdata),
        .mem_rd_valid(rd_valid), .mem_rd_data(rd_data),
        .frames_written(), .frames_rejected(), .fetch_late(fetch_late), .have_frame()
    );

    fb_line_ram lram (
        .wr_clk(clk_mem), .we(lb_we), .waddr(lb_waddr), .wdata(lb_wdata),
        .rd_clk(pix), .ra_e(ra_e), .ra_o(ra_o), .rb_e(rb_e), .rb_o(rb_o),
        .qa_e(qa_e), .qa_o(qa_o), .qb_e(qb_e), .qb_o(qb_o)
    );

    wire [7:0] r, g, b;
    wire       de, hs, vs;
    localparam int OUT_W = OUT768 ? 1366 : 1280, OUT_H = OUT768 ? 768 : 720;
    fb_scanout #(
        .H_ACTIVE(720), .V_ACTIVE(576), .OUT_W(OUT_W), .OUT_H(OUT_H), .IMG_X(280), .IMG_Y(72),
        .H_FP(OUT768 ? 14 : 440), .H_SYNC(OUT768 ? 56 : 40), .H_BP(OUT768 ? 64 : 220),
        .V_FP(OUT768 ? 1 : 5), .V_SYNC(OUT768 ? 3 : 5), .V_BP(OUT768 ? 28 : 20),
        .HS_NEG(1'b0), .VS_NEG(1'b0), .SCALE(1'b1), .OUT768(OUT768), .FT_W512(FT_W512)
    ) scan (
        .pixel_clk(pix), .rst_n(rst_n), .blank(1'b0), .aspect(ASPECT), .size(2'(SIZE)),
        .ft(1'b1), .src_w(11'(SRC_W)), .src_h(10'(SRC_H)),
        .req_toggle(req_toggle), .req_line(req_line), .req_first(req_first),
        .req_two(req_two), .req_ft(req_ft),
        .lb_ra_e(ra_e), .lb_ra_o(ra_o), .lb_rb_e(rb_e), .lb_rb_o(rb_o),
        .lb_qa_e(qa_e), .lb_qa_o(qa_o), .lb_qb_e(qb_e), .lb_qb_o(qb_o),
        .out_r(r), .out_g(g), .out_b(b), .out_de(de), .out_hs(hs), .out_vs(vs),
        .chk_err(), .chk_tick()
    );

    wire        sd_cs_n, sd_ras_n, sd_cas_n, sd_we_n, sd_dq_oe;
    wire [10:0] sd_a;
    wire [1:0]  sd_ba;
    wire [3:0]  sd_dqm;
    wire [31:0] sd_dq_out, sd_dq_in;
    wire [0:0]  op_tag;
    assign mem_op_tag = op_tag[0];
    sdram_burst #(.TAGW(1), .CLK_KHZ(61_875), .INIT_US(5)) ctrl (
        .clk(clk_mem), .rst_n(rst_n), .rd_phase(2'd1),
        .req(mem_req), .we(mem_we), .addr(mem_addr), .req_len(mem_len), .req_tag(mem_tag),
        .ack(mem_ack), .op_tag(op_tag),
        .wr_take(wr_take), .wr_idx(wr_idx), .wdata(mem_wdata), .wmask(4'hF),
        .rd_valid(rd_valid), .rd_idx(rd_idx), .rd_data(rd_data), .init_done(init_done),
        .sdram_cs_n(sd_cs_n), .sdram_ras_n(sd_ras_n), .sdram_cas_n(sd_cas_n),
        .sdram_we_n(sd_we_n), .sdram_a(sd_a), .sdram_ba(sd_ba), .sdram_dqm(sd_dqm),
        .sdram_dq_out(sd_dq_out), .sdram_dq_oe(sd_dq_oe), .sdram_dq_in(sd_dq_in));
    sdram_model #(.TCK_NS(16.16)) mem (
        .clk(~clk_mem), .cs_n(sd_cs_n), .ras_n(sd_ras_n), .cas_n(sd_cas_n), .we_n(sd_we_n),
        .a(sd_a), .ba(sd_ba), .dqm(sd_dqm), .dq_from_ctrl(sd_dq_out), .dq_oe(sd_dq_oe),
        .dq_to_ctrl(sd_dq_in));

    // кадр FT в SDRAM
    reg [31:0] frame [0:768 * 512 - 1];
    integer i;
    initial begin
        string ff;
        if (!$value$plusargs("frame=%s", ff)) ff = "ft_frame.hex";
        $readmemh(ff, frame);
        for (i = 0; i < 768 * 512; i = i + 1) mem.mem[FB_BASE + i] = frame[i];
    end

    // выход: кадры по VS, третий — в файл
    integer frames = 0, fo = 0, x = 0, lines = 0;
    reg prev_vs = 1'b0, prev_de = 1'b0;
    always @(posedge pix) begin
        if (vs && !prev_vs) begin
            frames = frames + 1;
            if (frames == 3) fo = $fopen("scan_out.hex", "w");
            if (frames == 4) begin
                $fclose(fo);
                $display("кадр выведен: строк %0d; запрос строки опоздал: %0d; ошибок SDRAM: %0d",
                         lines, fetch_late, mem.errors);
                if (lines == OUT_H && !fetch_late && mem.errors == 0)
                    $display("ТЕСТ ПРОЙДЕН (сверка — ft_scan_check.py)");
                else
                    $display("ТЕСТ НЕ ПРОЙДЕН");
                $finish;
            end
        end
        prev_vs = vs;
        if (frames == 3) begin
            if (de) begin
                $fwrite(fo, "%02h%02h%02h ", r, g, b);
                x = x + 1;
            end else if (prev_de) begin
                $fwrite(fo, "\n");
                lines = lines + 1;
                x = 0;
            end
        end
        prev_de = de;
    end

    initial begin
        #100 rst_n = 1'b1;
        #(64'd200_000_000);
        $display("ТЕСТ НЕ ПРОЙДЕН: время вышло, кадров %0d", frames);
        $finish;
    end
endmodule
