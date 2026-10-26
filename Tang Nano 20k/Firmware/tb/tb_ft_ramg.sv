// SPDX-License-Identifier: Apache-2.0
//
// FT812: RAM_G в SDRAM (ft_core + ft_ramg + sdram_burst + модель SDRAM с проверкой протокола).
//
// Хост-машины пишут и читают RAM_G вперемешку: Evo по SPI (байты, 14 МГц; часть чтений —
// на 28 МГц) и порт слов (ESP по LINK). Всё записанное помнит эталон; каждое прочитанное
// известное значение сверяется с ним. Ещё проверяется:
//   - ROM_CHIPID 08 12 01 00 в RAM_G по 0x0C0000 после сброса ядра;
//   - в режиме 2 МБ адреса 0x200000… — ПЗУ: читается образ шрифтов, запись не меняет его;
//   - в режиме 3 МБ те же адреса — RAM_G;
//   - модель SDRAM не нашла нарушений протокола, очередь хоста не переполнялась.
// HOG = 1 — SDRAM занята, как в ядре с растеризатором и выводом кадра: второй клиент без
// перерыва читает пачками по 8 пакетов и чередуется с RAM_G (как fb_core в ts_fb_core),
// а хост пишет и мелкими посылками вразброс на 28 МГц. Ответ SPI на промах кэша тогда
// опаздывает (ждёт чужую пачку, а у SPI — 8 тактов SCK): чтения SPI считаются отдельно, а
// записи сверяются в конце портом (он ответа ждёт) — все известные байты.
// На 28 МГц чтение промахом мимо кэша может опоздать (ramg_late): первые два байта посылки
// и первые два байта нового пакета SDRAM считаются отдельно, остальные сверяются.
`timescale 1ns/1ps
module tb_ft_ramg;
    parameter real CLK_NS = 16.1616;   // 61,875 МГц
    parameter int  OPS    = 400;
    parameter int  SEED   = 1;
    parameter logic [1:0] RD_PHASE = 2'd0;
    parameter bit  HOG    = 1'b0;
    parameter bit  SCATTER = HOG;      // мелкие записи вразброс на 28 МГц

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
    reg         mode3 = 1'b0;
    reg         romwe = 1'b0;          // окно пишет в образ ПЗУ (FT_CFG бит 3)
    wire        late, drop;

    wire        g_req, g_we, g_ack, g_wr_take, g_rd_valid;
    wire [20:0] g_addr;
    wire [5:0]  g_wr_idx, g_rd_idx;
    wire [31:0] g_wdata, g_rd_data;
    wire [3:0]  g_wmask;
    wire [1:0]  op_tag;

    ft_core #(.CLK_KHZ(61_875)) dut (
        .clk(clk), .rst_n(rst_n),
        .spi_tg(tg), .spi_kind(kind), .spi_h0(h0), .spi_h1(h1), .spi_b(b), .spi_cs_n(cs_n), .host_tg(1'b0), .host_cmd(8'd0), .host_par(8'd0),
        .spi_rd_byte(rd_byte),
        .p_req(p_req), .p_we(p_we), .p_cp(1'b0), .p_addr(p_addr), .p_wdata(p_wdata), .p_ack(p_ack),
        .p_rdata(p_rdata),
        .int_n(), .cmd_kick(), .copro_hold(), .cmd_read_ptr(), .cmd_write_ptr(),
        .dl_active(), .frame_tick(), .rd_dl_addr(11'd0), .rd_dl_q(), .swap_hold(1'b0),
        .disp_hsize(), .disp_vsize(), .disp_on(), .status(), .ramg_late(late), .ramg_drop(drop),
        .ramg_3mb(mode3), .rom_we(romwe),
        .g_req(g_req), .g_we(g_we), .g_addr(g_addr), .g_ack(g_ack), .g_wr_take(g_wr_take),
        .g_wr_idx(g_wr_idx[2:0]), .g_wdata(g_wdata), .g_wmask(g_wmask), .g_rd_valid(g_rd_valid),
        .g_rd_idx(g_rd_idx[2:0]), .g_rd_data(g_rd_data));

    wire        sd_cs_n, sd_ras_n, sd_cas_n, sd_we_n, sd_dq_oe, init_done;
    wire [10:0] sd_a;
    wire [1:0]  sd_ba;
    wire [3:0]  sd_dqm;
    wire [31:0] sd_dq_out, sd_dq_in;
    // второй клиент SDRAM (HOG): метка 2'b00, чтение 8 пакетов подряд; RAM_G — метка 2'b10
    reg         hog_on = 1'b0, last_ft = 1'b0;
    reg  [20:0] hog_addr = 21'h100000;
    wire        sd_ack;
    wire        hog_req = HOG && hog_on;
    wire        sel_ft  = g_req && (!hog_req || !last_ft);
    wire        own_ft  = op_tag == 2'b10;
    wire        wr_take_any, rd_valid_any;
    assign g_ack      = sd_ack && own_ft;
    assign g_wr_take  = wr_take_any && own_ft;
    assign g_rd_valid = rd_valid_any && own_ft;
    always @(posedge clk) if (sd_ack) begin
        last_ft <= own_ft;
        if (!own_ft) hog_addr <= hog_addr + 21'd64;
    end
    sdram_burst #(.TAGW(2), .CLK_KHZ(61_875), .INIT_US(5)) ctrl (
        .clk(clk), .rst_n(rst_n), .rd_phase(RD_PHASE),
        .req(g_req || hog_req), .we(sel_ft ? g_we : 1'b0), .addr(sel_ft ? g_addr : hog_addr),
        .req_len(sel_ft ? 3'd0 : 3'd7), .req_tag(sel_ft ? 2'b10 : 2'b00), .ack(sd_ack), .op_tag(op_tag),
        .wr_take(wr_take_any), .wr_idx(g_wr_idx), .wdata(g_wdata), .wmask(g_wmask),
        .rd_valid(rd_valid_any), .rd_idx(g_rd_idx), .rd_data(g_rd_data), .init_done(init_done),
        .sdram_cs_n(sd_cs_n), .sdram_ras_n(sd_ras_n), .sdram_cas_n(sd_cas_n),
        .sdram_we_n(sd_we_n), .sdram_a(sd_a), .sdram_ba(sd_ba), .sdram_dqm(sd_dqm),
        .sdram_dq_out(sd_dq_out), .sdram_dq_oe(sd_dq_oe), .sdram_dq_in(sd_dq_in));
    sdram_model #(.TCK_NS(CLK_NS), .CL(2)) sdram (
        .clk(~clk), .cs_n(sd_cs_n), .ras_n(sd_ras_n), .cas_n(sd_cas_n), .we_n(sd_we_n),
        .a(sd_a), .ba(sd_ba), .dqm(sd_dqm),
        .dq_from_ctrl(sd_dq_out), .dq_oe(sd_dq_oe), .dq_to_ctrl(sd_dq_in));

    // +trace: обмен ft_ramg с контроллером
    reg [7:0] last_rb;
    always @(posedge sck) if ($test$plusargs("trace") && spi.load && spi.bitn == 3'd7) $display("[%0t]   load txb <= %02h", $time, spi.rd_byte);
    always @(posedge clk) if ($test$plusargs("trace")) begin
        if (g_req && g_ack) $display("[%0t] SD %s %06h", $time, g_we ? "WR" : "RD", g_addr);
        if (g_wr_take) $display("[%0t]   take %0d %08h m%h", $time, g_wr_idx, g_wdata, g_wmask);
        if (g_rd_valid) $display("[%0t]   rd %0d %08h", $time, g_rd_idx, g_rd_data);
        if (dut.gq_valid && dut.gq_ready) $display("[%0t] Q src%0d %s %06h be%h %08h", $time, dut.rq_src, dut.rq_we ? "W" : "R", dut.rq_addr, dut.rq_be, dut.rq_wd);
        if (dut.gr_valid) $display("[%0t] R %08h", $time, dut.gr_data);
        if (dut.spi_rd_byte !== last_rb) $display("[%0t]   rd_byte %02h (lane %0d, host %0d)", $time, dut.spi_rd_byte, dut.g_rd_lane, dut.g_rd_host);
        last_rb <= dut.spi_rd_byte;
    end

    // --- SPI хоста --------------------------------------------------------------------------
    real sck_ns = 71.43;                       // 14 МГц
    task automatic spi_byte(input logic [7:0] o, output logic [7:0] i);
        for (int n = 7; n >= 0; n--) begin
            mosi = o[n];
            #(sck_ns / 2 - 1.0);
            i[n] = miso;
            #1.0;
            sck = 1'b1;
            #(sck_ns / 2);
            sck = 1'b0;
        end
    endtask

    logic [7:0] tx[0:511];
    logic [7:0] rx[0:511];
    logic [7:0] got;
    task automatic spi_xact(input int n);
        cs_n = 1'b0;
        #(sck_ns);
        for (int k = 0; k < n; k++) begin
            spi_byte(tx[k], got);           // Icarus: выход задачи — не в элемент массива
            rx[k] = got;
        end
        #(sck_ns);
        cs_n = 1'b1;
        #1500;                                 // Z80: между посылками микросекунды
    endtask

    task automatic spi_write(input logic [21:0] a, input int n);
        tx[0] = {2'b10, a[21:16]}; tx[1] = a[15:8]; tx[2] = a[7:0];
        spi_xact(n + 3);
    endtask

    task automatic spi_read(input logic [21:0] a, input int n);
        tx[0] = {2'b00, a[21:16]}; tx[1] = a[15:8]; tx[2] = a[7:0]; tx[3] = 8'h00;
        for (int k = 0; k < n; k++) tx[4 + k] = 8'hFF;
        spi_xact(n + 4);
    endtask

    // --- порт слов ---------------------------------------------------------------------------
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

    // --- эталон ----------------------------------------------------------------------------------
    localparam int G_BYTES = 3 * 1024 * 1024;
    reg  [7:0] ref_d [0:G_BYTES-1];             // эталон RAM_G
    reg        ref_k [0:G_BYTES-1];             // байт известен
    int        known[$];                        // адреса записанного — для выбора чтений
    initial for (int i = 0; i < G_BYTES; i++) ref_k[i] = 1'b0;
    task automatic put(input int a, input logic [7:0] d);
        ref_d[a] = d;
        if (!ref_k[a]) known.push_back(a);
        ref_k[a] = 1'b1;
    endtask
    function automatic int pick_known();
        return known[$urandom_range(0, known.size() - 1)];
    endfunction
    int errors = 0, checked = 0, late_bytes = 0;
    int cur_a = 0;
    int op_w = 0, op_r = 0, op_r28 = 0, op_pw = 0, op_pr = 0;

    task automatic expect_byte(input int a, input logic [7:0] got, input string who, input bit first28);
        if (!ref_k[a]) return;
        checked++;
        if (got !== ref_d[a]) begin
            if (first28) begin late_bytes++; return; end
            errors++;
            if (errors <= 10) $display("[%0t] %s: адрес %06h прочитано %02h, ждали %02h (SCK %.1f нс, байт %0d посылки)",
                                       $time, who, a, got, ref_d[a], sck_ns, a - cur_a);
        end
    endtask

    int a, n, r, wa;
    logic [31:0] w;
    int font_w;
    integer seed;
    initial begin
        seed = SEED;
        void'($urandom(seed));
        #200 rst_n = 1'b1;
        wait (init_done);
        #2000;
        // ACTIVE: FT812 выходит из SLEEP
        tx[0] = 8'h00; tx[1] = 8'h00; tx[2] = 8'h00;
        spi_xact(3);

        if ($test$plusargs("trace")) begin
            port_wr(22'h001000, 32'h1122_3344);
            port_rd(22'h001000, w);
            $display("порт: прочитано %08h", w);
        end
        // ROM_CHIPID
        spi_read(22'h0C0000, 4);
        if ({rx[7], rx[6], rx[5], rx[4]} !== 32'h0001_1208) begin
            errors++;
            $display("ROM_CHIPID: %02h %02h %02h %02h", rx[4], rx[5], rx[6], rx[7]);
        end
        put(32'h0C0000, 8'h08); put(32'h0C0001, 8'h12); put(32'h0C0002, 8'h01); put(32'h0C0003, 8'h00);
        hog_on = 1'b1;

        for (int op = 0; op < OPS; op++) begin
            r = $urandom_range(0, 99);
            if (r < 35) begin                       // SPI: запись
                if (SCATTER && $urandom_range(0, 1)) begin    // мелкие посылки вразброс, 28 МГц
                    sck_ns = 35.714;
                    for (int s = 0; s < 6; s++) begin
                        n = $urandom_range(1, 6);
                        a = $urandom_range(0, 32'h1FFFFF - n);
                        for (int k = 0; k < n; k++) begin
                            tx[3 + k] = $urandom;
                            put(a + k, tx[3 + k]);
                        end
                        spi_write(a[21:0], n);
                    end
                    sck_ns = 71.43;
                end else begin
                    n = $urandom_range(1, 4) == 1 ? $urandom_range(65, 300) : $urandom_range(1, 64);
                    a = $urandom_range(0, 32'h1FFFFF - n);
                    if ($urandom_range(0, 3) == 0) a = 32'h0C0100 + $urandom_range(0, 255);   // рядом
                    for (int k = 0; k < n; k++) begin
                        tx[3 + k] = $urandom;
                        put(a + k, tx[3 + k]);
                    end
                    spi_write(a[21:0], n);
                end
                op_w++;
            end else if (r < 60) begin             // SPI: чтение известного, 14 или 28 МГц
                a = pick_known();
                n = $urandom_range(1, 64);
                if (a + n > 32'h200000) n = 32'h200000 - a;
                if ($urandom_range(0, 4) == 0) begin sck_ns = 35.714; op_r28++; end else op_r++;
                cur_a = a;
                spi_read(a[21:0], n);
                // На 28 МГц промах кэша опаздывает: первые два байта посылки и первые два
                // байта каждого нового пакета (32 байта) считаются отдельно
                for (int k = 0; k < n; k++)
                    expect_byte(a + k, rx[4 + k], sck_ns < 50.0 ? "SPI 28 МГц" : "SPI 14 МГц",
                                HOG || (sck_ns < 50.0 && (k <= 1 || (a + k) % 32 <= 1)));
                sck_ns = 71.43;
            end else if (r < 80) begin             // порт: запись слова
                wa = $urandom_range(0, 32'h1FFFFF) & ~3;
                if ($urandom_range(0, 2) == 0) wa = pick_known() & ~3;
                w = $urandom;
                port_wr(wa[21:0], w);
                for (int k = 0; k < 4; k++) put(wa + k, w[8*k +: 8]);
                op_pw++;
            end else begin                         // порт: чтение слова
                wa = pick_known() & ~3;
                port_rd(wa[21:0], w);
                for (int k = 0; k < 4; k++) expect_byte(wa + k, w[8*k +: 8], "порт", 1'b0);
                op_pr++;
            end
            #($urandom_range(0, 2000));
        end

        // ПЗУ в режиме 2 МБ: образ шрифтов из SDRAM, запись игнорируется
        font_w = 21'h0C0000 + (32'h200100 - 32'h1E0000) / 4;
        for (int k = 0; k < 4; k++) sdram.mem[font_w + k] = 32'hA5A5_0000 + k;
        spi_read(22'h200100, 16);
        for (int k = 0; k < 16; k++)
            if (rx[4 + k] !== 8'((32'hA5A5_0000 + k / 4) >> (8 * (k % 4)))) begin
                errors++;
                if (errors <= 10) $display("ПЗУ: байт %0d прочитан %02h", k, rx[4 + k]);
            end
        for (int k = 0; k < 16; k++) tx[3 + k] = 8'h5A;
        spi_write(22'h200100, 16);
        port_rd(22'h200100, w);
        if (w !== 32'hA5A5_0000) begin errors++; $display("ПЗУ записалось: %08h", w); end
        // FT_CFG бит 3 (rom_we): окно LINK заливает образ ПЗУ — запись доходит, SPI — по-прежнему нет
        romwe = 1'b1;
        repeat (4) @(posedge clk);
        port_wr(22'h2FFFFC, 32'h0020_1EE0);                    // ROM_FONTROOT, как у FT812
        port_wr(22'h200104, 32'h1234_5678);
        port_rd(22'h2FFFFC, w);
        if (w !== 32'h0020_1EE0) begin errors++; $display("ПЗУ с rom_we: ROM_FONTROOT %08h", w); end
        port_rd(22'h200104, w);
        if (w !== 32'h1234_5678) begin errors++; $display("ПЗУ с rom_we: слово %08h", w); end
        if (sdram.mem[font_w + 1] !== 32'h1234_5678) begin errors++; $display("ПЗУ с rom_we: в SDRAM %08h", sdram.mem[font_w + 1]); end
        for (int k = 0; k < 4; k++) tx[3 + k] = 8'h77;
        spi_write(22'h200104, 4);
        port_rd(22'h200104, w);
        if (w !== 32'h1234_5678) begin errors++; $display("ПЗУ с rom_we: записалось по SPI %08h", w); end
        romwe = 1'b0;
        repeat (4) @(posedge clk);
        port_wr(22'h200104, 32'hDEAD_BEEF);
        port_rd(22'h200104, w);
        if (w !== 32'h1234_5678) begin errors++; $display("ПЗУ без rom_we записалось: %08h", w); end

        // режим 3 МБ: 0x200000… — RAM_G
        mode3 = 1'b1;
        repeat (4) @(posedge clk);
        for (int k = 0; k < 40; k++) begin tx[3 + k] = 8'(k * 7 + 3); put(32'h280000 + k, tx[3 + k]); end
        spi_write(22'h280000, 40);
        cur_a = 32'h280000;
        spi_read(22'h280000, 40);
        // Под HOG начало пакета (32 байта) опаздывает, как у случайных чтений выше; записанное
        // целиком сверяется портом в конце
        for (int k = 0; k < 40; k++) expect_byte(32'h280000 + k, rx[4 + k], "3 МБ", HOG && k % 32 <= 1);
        port_rd(22'h280004, w);
        for (int k = 0; k < 4; k++) expect_byte(32'h280004 + k, w[8*k +: 8], "3 МБ порт", 1'b0);

        if (HOG || SCATTER || $test$plusargs("sweep")) begin   // всё записанное — портом
            int swept = 0;
            hog_on = 1'b0;
            for (int i = 0; i < known.size(); i++) begin
                wa = known[i] & ~3;
                if (known[i] >= 32'h200000) continue;
                port_rd(wa[21:0], w);
                for (int k = 0; k < 4; k++) expect_byte(wa + k, w[8*k +: 8], "сверка портом", 1'b0);
                swept++;
            end
            $display("сверка записанного портом: слов %0d", swept);
        end
        $display("операций: SPI запись %0d, чтение 14 МГц %0d, 28 МГц %0d; порт запись %0d, чтение %0d",
                 op_w, op_r, op_r28, op_pw, op_pr);
        $display("сверено байтов %0d, ошибок %0d; на 28 МГц опоздало байтов %0d (флаг ramg_late %0d)",
                 checked, errors, late_bytes, late);
        $display("SDRAM: ошибок протокола %0d; очередь хоста переполнялась %0d", sdram.errors, drop);
        if (errors == 0 && sdram.errors == 0 && (!drop || HOG)) $display("ТЕСТ ПРОЙДЕН");
        else $display("ТЕСТ НЕ ПРОЙДЕН");
        $finish;
    end

    initial begin
        #2_000_000_000;
        $display("ТЕСТ НЕ ПРОЙДЕН: время вышло");
        $finish;
    end
endmodule
