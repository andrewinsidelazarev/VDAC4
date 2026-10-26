// SPDX-License-Identifier: Apache-2.0
//
// Связь с BL616 (cp_spi + cp_link + ft_port_arb + ft_core + RAM_G в SDRAM). Тестбенч — за
// BL616: ведущий SPI, режим 0, SCK 26,67 МГц (+sck_ns= — другой). Одновременно окно LINK
// (клиент a арбитра) читает REG_ID — его ответы тоже сверяются.
//
// Проверяется:
//   - STATUS: метка, версия, свободное место, REG_CMD_*, флаги, счётчик сбросов;
//   - WRITE и READ_REQ/READ_DATA: RAM_G (SDRAM), RAM_DL, RAM_CMD, регистры — против эталона;
//   - длинные WRITE подряд без опроса: очередь не переполняется, всё записано;
//   - запись сопроцессора в REG_INT_FLAGS поднимает флаги (ИЛИ); слово состояния 0x3FFFFC;
//   - почтовый ящик: запись со стороны LINK (свой клок, 74,25 МГц), чтение BL616 по 0x3F0000;
//   - IRQ_N: кольцо не пусто → низкий; REG_CMD_READ = REG_CMD_WRITE → высокий;
//     сброс сопроцессора (REG_CPURESET 1 → 0) → низкий до STATUS;
//   - модель SDRAM не нашла нарушений протокола.
`timescale 1ns/1ps
module tb_cp_link;
    parameter real CLK_NS = 16.1616;   // 61,875 МГц
    parameter int  SEED   = 1;

    reg clk = 1'b0;
    always #(CLK_NS / 2) clk = ~clk;
    reg rst_n = 1'b0;

    // хост Evo молчит
    wire        h_tg;
    wire [1:0]  h_kind;
    wire [7:0]  h_h0, h_h1, h_b, h_rd;
    ft_spi hspi (.sck(1'b0), .cs_n(1'b1), .mosi(1'b0), .miso(), .tg(h_tg), .ev_kind(h_kind),
                 .ev_h0(h_h0), .ev_h1(h_h1), .ev_b(h_b), .rd_byte(h_rd));

    // BL616
    reg  sck = 1'b0, cs_n = 1'b1, mosi = 1'b0;
    wire miso, irq_n, ovf;
    wire         c_tg;
    wire [1:0]   c_kind;
    wire [31:0]  c_data, rb_q;
    wire [127:0] st_vec;
    wire [8:0]   rb_addr;
    cp_spi cspi (.sck(sck), .cs_n(cs_n), .mosi(mosi), .miso(miso), .tg(c_tg), .ev_kind(c_kind),
                 .ev_data(c_data), .st_vec(st_vec), .rb_addr(rb_addr), .rb_q(rb_q));

    wire        b_req, b_we, b_ack, p_req, p_we, p_cp, p_ack, a_ack;
    wire [21:2] b_addr, p_addr;
    wire [31:0] b_wdata, p_wdata, p_rdata;
    wire [11:0] cmd_read, cmd_write;
    wire [12:0] cmd_dl;
    wire [1:0]  dlswap;
    wire [2:0]  cpureset;
    wire [7:0]  resets;
    // сторона LINK почтового ящика: свой клок, как домен пикселей
    reg         pclk = 1'b0;
    always #6.734 pclk = ~pclk;
    reg         mb_we = 1'b0;
    reg  [8:0]  mb_waddr = '0;
    reg  [31:0] mb_wdata = '0;
    reg         boot = 1'b0;       // ядро просит перезагрузку ПЛИС (1.13)
    task automatic mb_write(input int a, input logic [31:0] v);
        @(posedge pclk);
        mb_we <= 1'b1; mb_waddr <= 9'(a); mb_wdata <= v;
        @(posedge pclk);
        mb_we <= 1'b0;
    endtask

    cp_link link (
        .clk(clk), .rst_n(rst_n), .sck(sck), .spi_tg(c_tg), .spi_kind(c_kind), .spi_data(c_data),
        .st_vec(st_vec), .rb_addr(rb_addr), .rb_q(rb_q),
        .p_req(b_req), .p_we(b_we), .p_addr(b_addr), .p_wdata(b_wdata), .p_ack(b_ack),
        .p_rdata(p_rdata), .cmd_read(cmd_read), .cmd_write(cmd_write), .cmd_dl(cmd_dl),
        .dlswap(dlswap), .cpureset(cpureset), .resets(resets), .boot(boot), .irq_n(irq_n), .overflow(ovf),
        .info(), .mb_clk(pclk), .mb_we(mb_we), .mb_waddr(mb_waddr), .mb_wdata(mb_wdata));


    // окно LINK
    reg         a_req = 1'b0;
    reg  [21:2] a_addr = 20'(22'h302000 >> 2);
    ft_port_arb arb (
        .clk(clk), .rst_n(rst_n),
        .a_req(a_req), .a_we(1'b0), .a_addr(a_addr), .a_wdata(32'd0), .a_ack(a_ack),
        .b_req(b_req), .b_we(b_we), .b_addr(b_addr), .b_wdata(b_wdata), .b_ack(b_ack),
        .p_req(p_req), .p_we(p_we), .p_cp(p_cp), .p_addr(p_addr), .p_wdata(p_wdata), .p_ack(p_ack));

    wire        g_req, g_we, g_ack, g_wr_take, g_rd_valid;
    wire [20:0] g_addr;
    wire [5:0]  g_wr_idx, g_rd_idx;
    wire [31:0] g_wdata, g_rd_data;
    wire [3:0]  g_wmask;
    wire [1:0]  op_tag;
    ft_core #(.CLK_KHZ(61_875)) dut (
        .clk(clk), .rst_n(rst_n),
        .spi_tg(h_tg), .spi_kind(h_kind), .spi_h0(h_h0), .spi_h1(h_h1), .spi_b(h_b), .spi_cs_n(1'b1), .host_tg(1'b0), .host_cmd(8'd0), .host_par(8'd0),
        .spi_rd_byte(h_rd),
        .p_req(p_req), .p_we(p_we), .p_cp(p_cp), .p_addr(p_addr), .p_wdata(p_wdata), .p_ack(p_ack),
        .p_rdata(p_rdata),
        .int_n(), .cmd_kick(), .copro_hold(), .cmd_read_ptr(cmd_read), .cmd_write_ptr(cmd_write),
        .dl_active(), .frame_tick(), .cmd_dl_val(cmd_dl), .dlswap_val(dlswap),
        .cpureset_val(cpureset), .copro_resets(resets),
        .rd_dl_addr(11'd0), .rd_dl_q(), .swap_hold(1'b0),
        .disp_hsize(), .disp_vsize(), .disp_on(), .status(), .ramg_late(), .ramg_drop(),
        .ramg_3mb(1'b0), .rom_we(1'b0),
        .g_req(g_req), .g_we(g_we), .g_addr(g_addr), .g_ack(g_ack), .g_wr_take(g_wr_take),
        .g_wr_idx(g_wr_idx[2:0]), .g_wdata(g_wdata), .g_wmask(g_wmask), .g_rd_valid(g_rd_valid),
        .g_rd_idx(g_rd_idx[2:0]), .g_rd_data(g_rd_data));

    wire        sd_cs_n, sd_ras_n, sd_cas_n, sd_we_n, sd_dq_oe, init_done;
    wire [10:0] sd_a;
    wire [1:0]  sd_ba;
    wire [3:0]  sd_dqm;
    wire [31:0] sd_dq_out, sd_dq_in;
    sdram_burst #(.TAGW(2), .CLK_KHZ(61_875), .INIT_US(5)) ctrl (
        .clk(clk), .rst_n(rst_n), .rd_phase(2'd0),
        .req(g_req), .we(g_we), .addr(g_addr), .req_len(3'd0), .req_tag(2'b10), .ack(g_ack), .op_tag(op_tag),
        .wr_take(g_wr_take), .wr_idx(g_wr_idx), .wdata(g_wdata), .wmask(g_wmask),
        .rd_valid(g_rd_valid), .rd_idx(g_rd_idx), .rd_data(g_rd_data), .init_done(init_done),
        .sdram_cs_n(sd_cs_n), .sdram_ras_n(sd_ras_n), .sdram_cas_n(sd_cas_n),
        .sdram_we_n(sd_we_n), .sdram_a(sd_a), .sdram_ba(sd_ba), .sdram_dqm(sd_dqm),
        .sdram_dq_out(sd_dq_out), .sdram_dq_oe(sd_dq_oe), .sdram_dq_in(sd_dq_in));
    sdram_model #(.TCK_NS(CLK_NS), .CL(2)) sdram (
        .clk(~clk), .cs_n(sd_cs_n), .ras_n(sd_ras_n), .cas_n(sd_cas_n), .we_n(sd_we_n),
        .a(sd_a), .ba(sd_ba), .dqm(sd_dqm),
        .dq_from_ctrl(sd_dq_out), .dq_oe(sd_dq_oe), .dq_to_ctrl(sd_dq_in));

    int errors = 0, checks = 0;
    task automatic expect32(input string what, input logic [31:0] got, input logic [31:0] want);
        checks++;
        if (got !== want) begin
            errors++;
            if (errors < 30) $display("ОШИБКА %s: %08h, ждали %08h", what, got, want);
        end
    endtask

    // --- окно LINK читает REG_ID вперемешку ----------------------------------------------------
    int a_reads = 0;
    initial begin
        @(posedge rst_n);
        repeat (400) @(posedge clk);
        forever begin
            repeat ($urandom_range(5, 60)) @(posedge clk);
            a_req <= 1'b1;
            do @(posedge clk); while (!a_ack);
            a_req <= 1'b0;
            expect32("LINK: REG_ID", p_rdata, 32'h7C);
            a_reads++;
        end
    end

    // --- SPI BL616 -------------------------------------------------------------------------------
    real sck_ns = 37.5;                       // 26,67 МГц
    task automatic spi_byte(input logic [7:0] o, output logic [7:0] i);
        for (int n = 7; n >= 0; n--) begin
            mosi = o[n];
            #(sck_ns / 2);
            sck = 1'b1;
            i[n] = miso;
            #(sck_ns / 2);
            sck = 1'b0;
        end
    endtask

    logic [7:0] tx[0:4095];
    logic [7:0] rx[0:4095];
    logic [7:0] got;
    task automatic xact(input int n);
        cs_n = 1'b0;
        #(sck_ns);
        for (int k = 0; k < n; k++) begin
            spi_byte(tx[k], got);
            rx[k] = got;
        end
        #(sck_ns / 2);
        cs_n = 1'b1;
        #200;
    endtask

    // снимок STATUS
    logic [7:0]  s_flags, s_resets;
    logic [11:0] s_wr, s_rd;
    logic [12:0] s_dl;
    logic [1:0]  s_swap;
    logic [2:0]  s_cpur;
    logic [9:0]  s_free;
    task automatic status();
        tx[0] = 8'hC3;
        for (int k = 1; k < 18; k++) tx[k] = 8'h00;
        xact(18);
        expect32("STATUS: метка и версия", {rx[3], rx[2]}, 16'h01C5);
        s_flags  = rx[4];
        s_resets = rx[5];
        s_wr     = {rx[7], rx[6]};
        s_rd     = {rx[9], rx[8]};
        s_dl     = {rx[11], rx[10]};
        s_swap   = rx[12][1:0];
        s_cpur   = rx[12][4:2];
        s_free   = {rx[15], rx[14]};
    endtask

    task automatic wr(input logic [21:0] a, input logic [31:0] w[], input int n);
        tx[0] = 8'hC1; tx[1] = {2'b00, a[21:16]}; tx[2] = a[15:8]; tx[3] = a[7:0];
        for (int k = 0; k < n; k++)
            for (int b = 0; b < 4; b++)
                tx[4 + 4 * k + b] = 8'(w[k] >> (8 * b));
        xact(4 + 4 * n);
    endtask

    task automatic wr1(input logic [21:0] a, input logic [31:0] v);
        logic [31:0] w[];
        w = new[1];
        w[0] = v;
        wr(a, w, 1);
    endtask

    logic [31:0] rd_buf[0:511];
    task automatic rd(input logic [21:0] a, input int n);
        int polls;
        tx[0] = 8'hC2; tx[1] = {2'b00, a[21:16]}; tx[2] = a[15:8]; tx[3] = a[7:0]; tx[4] = 8'(n - 1);
        xact(5);
        polls = 0;
        do begin
            status();
            polls++;
        end while (!s_flags[0] && polls < 1000);
        if (!s_flags[0]) begin
            errors++;
            $display("ОШИБКА: чтение %06h не готово", a);
        end
        tx[0] = 8'hC4;
        for (int k = 1; k < 2 + 4 * n; k++) tx[k] = 8'h00;
        xact(2 + 4 * n);
        for (int k = 0; k < n; k++)
            rd_buf[k] = {rx[2 + 4 * k + 3], rx[2 + 4 * k + 2], rx[2 + 4 * k + 1], rx[2 + 4 * k]};
    endtask

    // --- эталон и проверки ---------------------------------------------------------------------
    logic [31:0] w[];
    logic [31:0] g_ref[0:1023];
    int seed;
    initial begin
        seed = SEED;
        void'($urandom(seed));
        if ($value$plusargs("sck_ns=%f", sck_ns)) ;
        #200;
        rst_n = 1'b1;
        wait (init_done);
        repeat (300) @(posedge clk);

        status();
        expect32("свободно в очереди", s_free, 512);
        expect32("флаги после сброса (очередь пуста, чтение готово)", s_flags & 8'h3F, 8'h03);
        expect32("сбросы после включения", s_resets, 1);
        expect32("IRQ_N после STATUS", irq_n, 1);

        // RAM_G: 300 слов, затем 4 × 250 слов подряд без опроса
        w = new[300];
        for (int k = 0; k < 300; k++) begin w[k] = $urandom; g_ref[k] = w[k]; end
        wr(22'h001000, w, 300);
        rd(22'h001000, 256);
        for (int k = 0; k < 256; k++) expect32($sformatf("RAM_G %0d", k), rd_buf[k], g_ref[k]);
        rd(22'h001000 + 256 * 4, 44);
        for (int k = 0; k < 44; k++) expect32($sformatf("RAM_G %0d", 256 + k), rd_buf[k], g_ref[256 + k]);

        w = new[250];
        for (int r = 0; r < 4; r++) begin
            for (int k = 0; k < 250; k++) begin w[k] = $urandom; g_ref[250 * r + k] = w[k]; end
            wr(22'h040000 + 22'(1000 * r), w, 250);
        end
        status();
        expect32("переполнения нет", s_flags[3], 0);
        for (int r = 0; r < 4; r++) begin
            rd(22'h040000 + 22'(1000 * r), 250);
            for (int k = 0; k < 250; k++) expect32($sformatf("RAM_G блок %0d слово %0d", r, k), rd_buf[k], g_ref[250 * r + k]);
        end

        // RAM_DL, RAM_CMD
        w = new[64];
        for (int k = 0; k < 64; k++) begin w[k] = $urandom; g_ref[k] = w[k]; end
        wr(22'h300100, w, 64);
        rd(22'h300100, 64);
        for (int k = 0; k < 64; k++) expect32($sformatf("RAM_DL %0d", k), rd_buf[k], g_ref[k]);
        w = new[32];
        for (int k = 0; k < 32; k++) begin w[k] = $urandom; g_ref[k] = w[k]; end
        wr(22'h308000, w, 32);
        rd(22'h308000, 32);
        for (int k = 0; k < 32; k++) expect32($sformatf("RAM_CMD %0d", k), rd_buf[k], g_ref[k]);

        // регистры сопроцессора: REG_CMD_DL, REG_CMD_WRITE → STATUS, IRQ
        wr1(22'h302100, 32'd1234);
        wr1(22'h3020FC, 32'd64);
        status();
        expect32("STATUS: REG_CMD_DL", s_dl, 1234);
        expect32("STATUS: REG_CMD_WRITE", s_wr, 64);
        expect32("STATUS: REG_CMD_READ", s_rd, 0);
        expect32("STATUS: кольцо не пусто", s_flags[4], 1);
        expect32("IRQ_N: кольцо не пусто", irq_n, 0);
        wr1(22'h3020F8, 32'd64);
        status();
        expect32("STATUS: REG_CMD_READ", s_rd, 64);
        expect32("STATUS: кольцо пусто", s_flags[4], 0);
        expect32("IRQ_N: кольцо пусто", irq_n, 1);

        // запрос перезагрузки ПЛИС (1.13): бит 7 флагов STATUS и IRQ_N — BL616 даёт JTAG Reconfig
        expect32("STATUS: перезагрузки не просят", s_flags[7], 0);
        boot = 1'b1;
        repeat (4) @(posedge clk);
        expect32("IRQ_N: просят перезагрузку", irq_n, 0);
        status();
        expect32("STATUS: просят перезагрузку", s_flags[7], 1);
        boot = 1'b0;
        repeat (4) @(posedge clk);
        status();
        expect32("STATUS: запрос снят", s_flags[7], 0);
        expect32("IRQ_N: запрос снят", irq_n, 1);

        // почтовый ящик: ESP пишет 512 слов, BL616 читает 256 + 256 и по слову
        for (int k = 0; k < 512; k++) begin g_ref[k % 1024] = $urandom; mb_write(k, g_ref[k]); end
        rd(22'h3F0000, 256);
        for (int k = 0; k < 256; k++) expect32($sformatf("ящик %0d", k), rd_buf[k], g_ref[k]);
        rd(22'h3F0000 + 256 * 4, 256);
        for (int k = 0; k < 256; k++) expect32($sformatf("ящик %0d", 256 + k), rd_buf[k], g_ref[256 + k]);
        mb_write(0, 32'hB6010005);
        rd(22'h3F0000, 1);
        expect32("ящик: заголовок после перезаписи", rd_buf[0], 32'hB6010005);
        rd(22'h3F07FC, 1);
        expect32("ящик: последнее слово", rd_buf[0], g_ref[511]);

        // слово состояния прошивки: в cp_link, не в ft_core
        wr1(22'h3FFFFC, 32'hC0011234);
        status();
        expect32("слово состояния BL616", link.info, 32'hC0011234);

        // REG_INT_FLAGS: запись сопроцессора — ИЛИ
        wr1(22'h3020A8, 32'h20);
        wr1(22'h3020A8, 32'h40);
        rd(22'h3020A8, 1);
        expect32("REG_INT_FLAGS после записей сопроцессора", rd_buf[0], 32'h60);

        // DLSWAP: STATUS показывает ждущую смену (развёртки нет — сразу выполнена)
        wr1(22'h302054, 32'd2);
        status();
        expect32("STATUS: DLSWAP выполнен без развёртки", s_swap, 0);

        // сброс сопроцессора
        wr1(22'h302020, 32'd1);
        status();
        expect32("STATUS: сброс держит", s_flags[2], 1);
        expect32("STATUS: REG_CPURESET", s_cpur, 1);
        wr1(22'h302020, 32'd0);
        repeat (20) @(posedge clk);
        expect32("IRQ_N после сброса сопроцессора", irq_n, 0);
        status();
        expect32("STATUS: сбросы", s_resets, 2);
        expect32("IRQ_N после STATUS", irq_n, 1);

        // чтения по одному слову по всей карте вперемешку с записями
        for (int k = 0; k < 40; k++) begin
            logic [21:0] a;
            logic [31:0] v;
            case (k % 3)
                0: a = 22'h010000 + 22'(4 * $urandom_range(0, 4095));
                1: a = 22'h300000 + 22'(4 * $urandom_range(0, 2047));
                default: a = 22'h308000 + 22'(4 * $urandom_range(0, 1023));
            endcase
            v = $urandom;
            wr1(a, v);
            rd(a, 1);
            expect32($sformatf("слово %06h", a), rd_buf[0], v);
        end

        status();
        expect32("переполнения нет в конце", s_flags[3], 0);
        expect32("очередь пуста в конце", s_flags[1], 1);
        repeat (200) @(posedge clk);
        $display("проверок %0d, ошибок %0d; чтений LINK %0d; SDRAM: ошибок протокола %0d",
                 checks, errors, a_reads, sdram.errors);
        if (errors == 0 && sdram.errors == 0 && a_reads > 50)
            $display("ТЕСТ ПРОЙДЕН");
        else
            $display("ТЕСТ НЕ ПРОЙДЕН");
        $finish;
    end

    initial begin
        #50ms;
        $display("ТЕСТ НЕ ПРОЙДЕН: время вышло");
        $finish;
    end
endmodule
