// SPDX-License-Identifier: Apache-2.0
//
// FT812: смена дисплей-листа (REG_DLSWAP) в ft_core при занятом растеризаторе (swap_hold).
//
// Растеризатор держит swap_hold, пока обходит лист, — почти весь свой кадр; окно между его
// кадрами привязано к выводу (60 Гц), а строка события — к развёртке FT812 (59 Гц). Раньше
// смена «по кадру» шла только в саму строку события и только при снятом swap_hold: фаза
// плыла, и смена стояла до полсекунды. Теперь строка события смену лишь разрешает, а
// применяется она в первый такт без swap_hold. Проверяется:
//   - DLSWAP_FRAME при свободном растеризаторе — после строки события, не раньше, и не чаще
//     одной смены на кадр FT812 (темп программ, что ждут смену, — как у FT812);
//   - пока swap_hold поднят, банк не меняется (кадр не рвётся), REG_DLSWAP читается 2;
//   - строка события прошла при поднятом swap_hold — смена сразу, как он снят, до следующей
//     строки события (старое RTL здесь ждало следующую строку события);
//   - DLSWAP_LINE — сразу при снятом swap_hold; без развёртки (PCLK 0) — сразу.
// START_ACTIVE (ядро FT812 слота 1): REG_ID по SPI читается 7C без ACTIVE; без START_ACTIVE
// до ACTIVE чтение не отвечает (SLEEP, как у FT812 при включении).
`timescale 1ns/1ps
module tb_ft_swap;
    parameter real CLK_NS = 16.1616;   // 61,875 МГц
    parameter bit  START_ACTIVE = 1'b0;

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
    reg         hold = 1'b0;           // swap_hold растеризатора
    wire        dl_active, ftick;

    wire        g_req, g_we, g_ack, g_wr_take, g_rd_valid;
    wire [20:0] g_addr;
    wire [5:0]  g_wr_idx, g_rd_idx;
    wire [31:0] g_wdata, g_rd_data;
    wire [3:0]  g_wmask;
    wire [1:0]  op_tag;

    ft_core #(.CLK_KHZ(61_875), .START_ACTIVE(START_ACTIVE)) dut (
        .clk(clk), .rst_n(rst_n),
        .spi_tg(tg), .spi_kind(kind), .spi_h0(h0), .spi_h1(h1), .spi_b(b), .spi_cs_n(cs_n), .host_tg(1'b0), .host_cmd(8'd0), .host_par(8'd0),
        .spi_rd_byte(rd_byte),
        .p_req(p_req), .p_we(p_we), .p_cp(1'b0), .p_addr(p_addr), .p_wdata(p_wdata), .p_ack(p_ack),
        .p_rdata(p_rdata),
        .int_n(), .cmd_kick(), .copro_hold(), .cmd_read_ptr(), .cmd_write_ptr(),
        .dl_active(dl_active), .frame_tick(ftick), .rd_dl_addr(11'd0), .rd_dl_q(), .swap_hold(hold),
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

    localparam logic [21:0] REG_HCYCLE = 22'h30202C, REG_VCYCLE = 22'h302040, REG_VOFFSET = 22'h302044,
                            REG_VSIZE = 22'h302048, REG_DLSWAP = 22'h302054, REG_PCLK = 22'h302070;

    // --- SPI хоста (ACTIVE и чтение REG_ID) --------------------------------------------------
    localparam real SCK_NS = 71.43;
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
    // REG_ID (0x302000) по SPI: адрес, пустой байт, байт данных
    task automatic spi_reg_id(output logic [7:0] id);
        logic [7:0] x;
        cs_n = 1'b0; #(SCK_NS);
        spi_byte(8'h30, x); spi_byte(8'h20, x); spi_byte(8'h00, x); spi_byte(8'h00, x);
        spi_byte(8'h00, id);
        #(SCK_NS); cs_n = 1'b1;
        #1500;
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

    // REG_DLSWAP портом. Сначала — другой регистр: a_ff в ft_core — непрерывное присваивание
    // от функции ff_read(a_addr), и Icarus пересчитывает его только при смене a_addr, а не
    // dlswap_reg внутри функции (синтез — как надо); то же чтение подряд дало бы старое значение.
    task automatic dlswap_rd(output logic [31:0] d);
        logic [31:0] x;
        port_rd(REG_PCLK, x);
        port_rd(REG_DLSWAP, d);
    endtask

    // --- наблюдение: смены банка, строки события, смена при поднятом swap_hold --------------
    int  ticks = 0, swaps = 0, torn = 0;
    reg  last_dl = 1'b0, hold_q = 1'b0;
    longint t_swap = 0;
    always @(posedge clk) begin
        if (ftick) ticks <= ticks + 1;
        hold_q <= hold;
        if (rst_n && dl_active !== last_dl) begin
            swaps  <= swaps + 1;
            t_swap <= longint'($time);
            if (hold_q) torn <= torn + 1;     // банк сменился в такт, когда растеризатор читал лист
        end
        last_dl <= dl_active;
    end

    int errors = 0;
    task automatic check(input bit ok, input string what);
        if (!ok) begin
            errors++;
            $display("[%0t] ОШИБКА: %s", $time, what);
        end
    endtask

    // ждать смену банка не дольше n тактов; 1 — дождались
    task automatic wait_swap(input int s0, input int n, output bit got);
        got = 1'b0;
        for (int k = 0; k < n && !got; k++) begin
            @(posedge clk);
            #1;
            got = swaps != s0;
        end
    endtask

    logic [31:0] w;
    logic [7:0] id, x8;
    bit got;
    int s0, t0, gaps_bad;
    longint t_rel;
    initial begin
        #200 rst_n = 1'b1;
        wait (init_done);
        #2000;
        // 0. до ACTIVE: ядро слота 1 (START_ACTIVE) отвечает сразу, FT812 из включения — нет
        wait (!dut.clearing);
        spi_reg_id(id);
        if (START_ACTIVE) check(id === 8'h7C, $sformatf("0: REG_ID без ACTIVE %02h, ждали 7C", id));
        else check(id === 8'h00, $sformatf("0: REG_ID в SLEEP %02h, ждали 00", id));
        cs_n = 1'b0; #(SCK_NS);              // ACTIVE: 00 00 00
        spi_byte(8'h00, x8); spi_byte(8'h00, x8); spi_byte(8'h00, x8);
        #(SCK_NS); cs_n = 1'b1;
        #2000;
        spi_reg_id(id);
        check(id === 8'h7C, $sformatf("0: REG_ID после ACTIVE %02h, ждали 7C", id));
        // короткая развёртка: строка — 40 тактов FT812, кадр — 12 строк, строка события — 9
        port_wr(REG_HCYCLE, 32'd40);
        port_wr(REG_VCYCLE, 32'd12);
        port_wr(REG_VOFFSET, 32'd2);
        port_wr(REG_VSIZE, 32'd8);
        port_wr(REG_PCLK, 32'd1);
        t0 = ticks;
        while (ticks < t0 + 3) @(posedge clk);
        check(dut.scan_on_r === 1'b1, "развёртка не идёт");

        // 1. растеризатор свободен: DLSWAP_FRAME — после строки события, не раньше
        for (int r = 0; r < 4; r++) begin
            repeat ($urandom_range(0, 450)) @(posedge clk);
            s0 = swaps; t0 = ticks;
            port_wr(REG_DLSWAP, 32'd2);
            wait_swap(s0, 2000, got);
            check(got, "1: смена по кадру не прошла");
            check(ticks > t0, "1: смена по кадру раньше строки события");
            dlswap_rd(w);
            check(w === 32'd0, $sformatf("1: REG_DLSWAP после смены %0d", w));
        end

        // 2. темп: смена сразу после смены — не чаще кадра FT812
        gaps_bad = 0;
        for (int r = 0; r < 8; r++) begin
            s0 = swaps; t0 = ticks;
            port_wr(REG_DLSWAP, 32'd2);
            wait_swap(s0, 2000, got);
            check(got, "2: смена по кадру не прошла");
            if (ticks <= t0) gaps_bad++;
        end
        check(gaps_bad == 0, $sformatf("2: смен чаще кадра FT812: %0d", gaps_bad));

        // 3. swap_hold поднят на несколько кадров: смены нет, REG_DLSWAP — 2; снят между строками
        //    события — смена сразу, не дожидаясь следующей
        for (int r = 0; r < 4; r++) begin
            hold = 1'b1;
            s0 = swaps;
            port_wr(REG_DLSWAP, 32'd2);
            t0 = ticks;
            while (ticks < t0 + 3) @(posedge clk);
            check(swaps == s0, "3: смена при поднятом swap_hold");
            dlswap_rd(w);
            check(w === 32'd2, $sformatf("3: REG_DLSWAP при ждущей смене %0d", w));
            @(posedge ftick);
            repeat (20 + 100 * r) @(posedge clk);     // середина кадра FT812, до строки события
            t0 = ticks;
            @(posedge clk) hold <= 1'b0;
            t_rel = longint'($time);
            wait_swap(s0, 8, got);
            check(got, "3: swap_hold снят — смена не прошла за 8 тактов");
            check(ticks == t0, "3: смена ждала следующую строку события");
        end

        // 4. DLSWAP_LINE: сразу при снятом swap_hold; при поднятом — ждёт
        s0 = swaps;
        port_wr(REG_DLSWAP, 32'd1);
        wait_swap(s0, 8, got);
        check(got, "4: смена по строке не прошла за 8 тактов");
        hold = 1'b1;
        s0 = swaps;
        port_wr(REG_DLSWAP, 32'd1);
        repeat (300) @(posedge clk);
        check(swaps == s0, "4: смена по строке при поднятом swap_hold");
        @(posedge clk) hold <= 1'b0;
        wait_swap(s0, 8, got);
        check(got, "4: смена по строке после снятия swap_hold не прошла");

        // 5. без развёртки (PCLK 0) — сразу
        port_wr(REG_PCLK, 32'd0);
        repeat (100) @(posedge clk);
        s0 = swaps;
        port_wr(REG_DLSWAP, 32'd2);
        wait_swap(s0, 8, got);
        check(got, "5: без развёртки смена не прошла сразу");

        check(torn == 0, $sformatf("банк сменился при поднятом swap_hold: %0d раз", torn));
        $display("смен листа %0d, строк события %0d, ошибок %0d", swaps, ticks, errors);
        if (errors == 0) $display("ТЕСТ ПРОЙДЕН");
        else $display("ТЕСТ НЕ ПРОЙДЕН");
        $finish;
    end

    initial begin
        #50_000_000;
        $display("ТЕСТ НЕ ПРОЙДЕН: время вышло");
        $finish;
    end
endmodule
