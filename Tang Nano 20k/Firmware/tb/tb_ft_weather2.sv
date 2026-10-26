// SPDX-License-Identifier: Apache-2.0
//
// FT812 (ft_spi + ft_core) против записанного обмена заставки Weather 2 с bt8xxemu.
//
// Файл +spi=… (по умолчанию weather2_spi.txt): посылка на строку — кадр Z80, число байтов,
// затем пары «байт Z80 / байт FT812» (4 hex-цифры). Посылки идут на SCK 28 МГц байт за
// байтом без пауз — худший случай для ядра. Сопроцессора нет: заглушка забирает слова
// кольца RAM_CMD через порт слов и двигает REG_CMD_READ.
//
// Проверяется:
//   - каждый байт MISO совпадает с ответом bt8xxemu, кроме данных REG_CMD_READ и
//     REG_CMDB_SPACE (они зависят от скорости сопроцессора);
//   - заглушка вычитала из кольца ровно то, что Z80 записал в REG_CMDB_WRITE, байт в байт;
//   - регистры режима 1024×768@59 и период кадра 1344 × 806 тактов 64 МГц (16,926 мс).
`timescale 1ns/1ps
module tb_ft_weather2;
    parameter real CLK_NS = 16.1616;   // 61,875 МГц
    parameter real SCK_NS = 35.714;    // 28 МГц

    reg clk = 1'b0;
    always #(CLK_NS / 2) clk = ~clk;
    reg rst_n = 1'b0;

    reg  sck = 1'b0, cs_n = 1'b1, mosi = 1'b0;
    wire miso;
    wire tg;
    wire [1:0] kind;
    wire [7:0] h0, h1, b, rd_byte;
    ft_spi spi (.sck(sck), .cs_n(cs_n), .mosi(mosi), .miso(miso), .tg(tg), .ev_kind(kind),
                .ev_h0(h0), .ev_h1(h1), .ev_b(b), .rd_byte(rd_byte));

    reg         p_req = 1'b0, p_we = 1'b0;
    reg  [21:2] p_addr = '0;
    reg  [31:0] p_wdata = '0;
    wire        p_ack;
    wire [31:0] p_rdata;
    wire [11:0] rp, wp;
    wire        ftick, int_n;
    wire        g_req, g_we, g_ack, g_wr_take, g_rd_valid;
    wire [20:0] g_addr;
    wire [5:0]  g_wr_idx, g_rd_idx;
    wire [31:0] g_wdata, g_rd_data;
    wire [3:0]  g_wmask;
    ft_core #(.CLK_KHZ(61_875)) dut (
        .clk(clk), .rst_n(rst_n),
        .spi_tg(tg), .spi_kind(kind), .spi_h0(h0), .spi_h1(h1), .spi_b(b), .spi_cs_n(cs_n), .host_tg(1'b0), .host_cmd(8'd0), .host_par(8'd0),
        .spi_rd_byte(rd_byte),
        .p_req(p_req), .p_we(p_we), .p_cp(1'b0), .p_addr(p_addr), .p_wdata(p_wdata), .p_ack(p_ack),
        .p_rdata(p_rdata),
        .int_n(int_n), .cmd_kick(), .copro_hold(), .cmd_read_ptr(rp), .cmd_write_ptr(wp),
        .dl_active(), .frame_tick(ftick), .rd_dl_addr(11'd0), .rd_dl_q(), .swap_hold(1'b0),
        .disp_hsize(), .disp_vsize(), .disp_on(), .status(), .ramg_late(), .ramg_drop(), .ramg_3mb(1'b0), .rom_we(1'b0),
        .g_req(g_req), .g_we(g_we), .g_addr(g_addr), .g_ack(g_ack), .g_wr_take(g_wr_take),
        .g_wr_idx(g_wr_idx[2:0]), .g_wdata(g_wdata), .g_wmask(g_wmask), .g_rd_valid(g_rd_valid),
        .g_rd_idx(g_rd_idx[2:0]), .g_rd_data(g_rd_data));

    // RAM_G: контроллер и модель SDRAM (Weather 2 сама RAM_G не трогает; ядро пишет туда
    // ROM_CHIPID при сбросе, ACTIVE читает адрес 0)
    wire        sd_cs_n, sd_ras_n, sd_cas_n, sd_we_n, sd_dq_oe;
    wire [10:0] sd_a;
    wire [1:0]  sd_ba;
    wire [3:0]  sd_dqm;
    wire [31:0] sd_dq_out, sd_dq_in;
    sdram_burst #(.TAGW(2), .CLK_KHZ(61_875), .INIT_US(5)) ctrl (
        .clk(clk), .rst_n(rst_n), .rd_phase(2'd0),
        .req(g_req), .we(g_we), .addr(g_addr), .req_len(3'd0), .req_tag(2'b10), .ack(g_ack), .op_tag(),
        .wr_take(g_wr_take), .wr_idx(g_wr_idx), .wdata(g_wdata), .wmask(g_wmask),
        .rd_valid(g_rd_valid), .rd_idx(g_rd_idx), .rd_data(g_rd_data), .init_done(),
        .sdram_cs_n(sd_cs_n), .sdram_ras_n(sd_ras_n), .sdram_cas_n(sd_cas_n),
        .sdram_we_n(sd_we_n), .sdram_a(sd_a), .sdram_ba(sd_ba), .sdram_dqm(sd_dqm),
        .sdram_dq_out(sd_dq_out), .sdram_dq_oe(sd_dq_oe), .sdram_dq_in(sd_dq_in));
    sdram_model #(.TCK_NS(CLK_NS), .CL(2)) sdram (
        .clk(~clk), .cs_n(sd_cs_n), .ras_n(sd_ras_n), .cas_n(sd_cas_n), .we_n(sd_we_n),
        .a(sd_a), .ba(sd_ba), .dqm(sd_dqm),
        .dq_from_ctrl(sd_dq_out), .dq_oe(sd_dq_oe), .dq_to_ctrl(sd_dq_in));

    // --- порт слов -------------------------------------------------------------------------
    task automatic port_rd(input logic [21:0] a, output logic [31:0] d);
        @(posedge clk);
        p_addr <= a[21:2]; p_we <= 1'b0; p_req <= 1'b1;
        do @(posedge clk); while (!p_ack);
        p_req <= 1'b0;
        d = p_rdata;
    endtask

    task automatic port_wr(input logic [21:0] a, input logic [31:0] d);
        @(posedge clk);
        p_addr <= a[21:2]; p_we <= 1'b1; p_wdata <= d; p_req <= 1'b1;
        do @(posedge clk); while (!p_ack);
        p_req <= 1'b0;
    endtask

    // --- заглушка сопроцессора: вычитывает кольцо ----------------------------------------------
    byte unsigned consumed[$];
    reg  copro_on = 1'b1;
    reg  copro_busy = 1'b0;
    initial begin : copro
        logic [11:0] r;
        logic [31:0] w;
        wait (rst_n);
        forever begin
            @(posedge clk);
            if (copro_on && rp != wp) begin
                copro_busy = 1'b1;
                r = rp;
                while (r != wp) begin
                    port_rd(22'h308000 + 22'(r), w);
                    for (int k = 0; k < 4; k++) consumed.push_back(w[8*k +: 8]);
                    r = r + 12'd4;
                end
                port_wr(22'h3020F8, 32'(r));
                copro_busy = 1'b0;
            end
        end
    end

    // --- SPI хоста -------------------------------------------------------------------------------
    // Режим 0. Evo видит ответ с задержкой буферов, поэтому бит берётся перед фронтом SCK.
    task automatic spi_byte(input logic [7:0] o, output logic [7:0] i);
        for (int n = 7; n >= 0; n--) begin
            mosi = o[n];
            #(SCK_NS / 2 - 1.0);
            i[n] = miso;
            #1.0;
            sck = 1'b1;
            #(SCK_NS / 2);
            sck = 1'b0;
        end
    endtask

    byte unsigned expected_cmdb[$];
    realtime t_frame0, t_frame1;
    int errors = 0, checked = 0, skipped = 0;
    int fd, frame, n, tok, rc, tr = 0, last_frame = 0;
    logic [7:0] outb[0:131071];
    logic [7:0] inb[0:131071];
    logic [7:0] got;
    logic [21:0] addr;
    string path;

    initial begin
        if (!$value$plusargs("spi=%s", path)) path = "weather2_spi.txt";
        fd = $fopen(path, "r");
        if (fd == 0) begin $display("нет файла %s", path); $finish; end
        #200 rst_n = 1'b1;
        #1000;
        while (!$feof(fd)) begin
            rc = $fscanf(fd, "%d %d", frame, n);
            if (rc != 2) break;
            for (int k = 0; k < n; k++) begin
                rc = $fscanf(fd, "%h", tok);
                outb[k] = tok[15:8];
                inb[k]  = tok[7:0];
            end
            addr = {outb[0][5:0], outb[1], outb[2]};
            if (n >= 3 && outb[0][7:6] == 2'b10 && addr == 22'h302578)
                for (int k = 3; k < n; k++) expected_cmdb.push_back(outb[k]);
            // кадры Z80 между посылками — паузой (кадр Evo 20 мс, здесь 50 мкс)
            if (frame != last_frame) #(50_000.0);
            last_frame = frame;
            // посылка
            cs_n = 1'b0;
            #(SCK_NS);
            for (int k = 0; k < n; k++) begin
                spi_byte(outb[k], got);
                if (outb[0][7:6] == 2'b00 && k >= 4 &&
                    (addr == 22'h3020F8 || addr == 22'h302574)) begin
                    skipped++;
                end else begin
                    checked++;
                    if (got !== inb[k]) begin
                        errors++;
                        if (errors <= 10)
                            $display("посылка %0d (кадр %0d) байт %0d: Tang %02h, bt8xxemu %02h, адрес %06h",
                                     tr, frame, k, got, inb[k], addr);
                    end
                end
            end
            #(SCK_NS);
            cs_n = 1'b1;
            #400;                              // Z80: между посылками микросекунды
            tr++;
        end
        $fclose(fd);
        // дождаться сопроцессора и двух событий кадра
        wait (rp == wp && !copro_busy);
        repeat (2) @(posedge ftick);
        t_frame0 = $realtime;
        @(posedge ftick);
        t_frame1 = $realtime;
        finish_checks();
    end

    task automatic finish_checks();
        logic [31:0] w;
        int bad = 0;
        copro_on = 1'b0;
        $display("посылок %0d, байтов MISO сверено %0d, ошибок %0d (данные CMD_READ/CMDB_SPACE: %0d)",
                 tr, checked, errors, skipped);
        $display("в кольцо записано %0d байтов, вычитано %0d", expected_cmdb.size(), consumed.size());
        if (expected_cmdb.size() != consumed.size()) bad++;
        for (int k = 0; k < expected_cmdb.size() && k < consumed.size(); k++)
            if (expected_cmdb[k] != consumed[k]) begin
                if (bad < 5) $display("кольцо: байт %0d записан %02h, вычитан %02h", k,
                                      expected_cmdb[k], consumed[k]);
                bad++;
            end
        port_rd(22'h30202C, w); if (w != 1344) begin bad++; $display("HCYCLE %0d", w); end
        port_rd(22'h302040, w); if (w != 806)  begin bad++; $display("VCYCLE %0d", w); end
        port_rd(22'h302034, w); if (w != 1024) begin bad++; $display("HSIZE %0d", w); end
        port_rd(22'h302048, w); if (w != 768)  begin bad++; $display("VSIZE %0d", w); end
        port_rd(22'h302070, w); if (w != 1)    begin bad++; $display("PCLK %0d", w); end
        port_rd(22'h302038, w); if (w != 24)   begin bad++; $display("HSYNC0 %0d", w); end
        port_rd(22'h302068, w); if (w != 0)    begin bad++; $display("CSPREAD %0d", w); end
        port_rd(22'h302000, w); if (w != 32'h7C) begin bad++; $display("ID %08h", w); end
        port_rd(22'h302574, w); if (w != 32'hFFC) begin bad++; $display("CMDB_SPACE %03h", w); end
        $display("период кадра FT812 %.1f мкс (1344 × 806 / 64 МГц = 16926.0)",
                 (t_frame1 - t_frame0) / 1000.0);
        if (t_frame1 - t_frame0 < 16_920_000.0 || t_frame1 - t_frame0 > 16_932_000.0) bad++;
        if (errors == 0 && bad == 0) $display("ТЕСТ ПРОЙДЕН");
        else $display("ТЕСТ НЕ ПРОЙДЕН: %0d + %0d", errors, bad);
        $finish;
    endtask

    initial begin
        #2_000_000_000;
        $display("ТЕСТ НЕ ПРОЙДЕН: время вышло");
        $finish;
    end
endmodule
