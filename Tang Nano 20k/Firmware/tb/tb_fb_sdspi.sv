// SPDX-License-Identifier: Apache-2.0
//
// Стенд моста к microSD (fb_sdspi) с моделью карты (sd_card_model): то, что будет делать ESP —
// 80 тактов при неактивном CS, CMD0, CMD8, ACMD41, CMD58, переход на быстрый SCK, чтение блока
// (READ — маркер и 514 байтов в RX, в том числе со смещением PTR), запись блока (XFER, ответ
// данных, BUSY) и чтение записанного, маркер ошибки, ожидание без ответа, последнее неполное
// слово RX. Ожидается: «ТЕСТ ПРОЙДЕН».
//
`timescale 1ns/1ps
module tb_fb_sdspi;
    localparam real CLK_NS = 13.468;          // 74,25 МГц
    localparam real RD_NS  = 25.0;            // SCK LINK 40 МГц

    reg clk = 1'b0, rd_clk = 1'b0, rst_n = 1'b0;
    always #(CLK_NS / 2.0) clk = ~clk;
    always #(RD_NS / 2.0) rd_clk = ~rd_clk;

    reg         ctrl_we = 1'b0, ptr_we = 1'b0, cmd_we = 1'b0, tx_we = 1'b0;
    reg  [31:0] wdata = '0, tx_wdata = '0;
    reg  [8:0]  tx_waddr = '0, rd_addr = '0;
    wire [31:0] ctrl_rd, ptr_rd, status, rd_q;
    wire        sd_cs_n, sd_sck, sd_mosi, sd_miso;

    fb_sdspi #(.CLK_KHZ(74_250)) dut (
        .clk(clk), .rst_n(rst_n),
        .sd_cs_n(sd_cs_n), .sd_sck(sd_sck), .sd_mosi(sd_mosi), .sd_miso(sd_miso),
        .ctrl_we(ctrl_we), .ptr_we(ptr_we), .cmd_we(cmd_we), .wdata(wdata),
        .ctrl_rd(ctrl_rd), .ptr_rd(ptr_rd), .status(status),
        .tx_we(tx_we), .tx_waddr(tx_waddr), .tx_wdata(tx_wdata),
        .rd_clk(rd_clk), .rd_addr(rd_addr), .rd_q(rd_q)
    );
    sd_card_model #(.BLOCKS(16), .NAC(40), .BUSY(30)) card (
        .cs_n(sd_cs_n), .sck(sd_sck), .mosi(sd_mosi), .miso(sd_miso)
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

    // регистры — строб на такт, как от LINK
    task automatic reg_wr(input integer which, input [31:0] v);
        begin
            @(posedge clk); #1;
            wdata = v;
            ctrl_we = which == 0; ptr_we = which == 1; cmd_we = which == 2;
            @(posedge clk); #1;
            ctrl_we = 1'b0; ptr_we = 1'b0; cmd_we = 1'b0;
        end
    endtask
    task automatic tx_wr(input integer w, input [31:0] v);
        begin
            @(posedge clk); #1;
            tx_we = 1'b1; tx_waddr = w[8:0]; tx_wdata = v;
            @(posedge clk); #1;
            tx_we = 1'b0;
        end
    endtask
    task automatic rx_rd(input integer w, output [31:0] v);
        begin
            @(negedge rd_clk); rd_addr = w[8:0];
            @(posedge rd_clk); @(negedge rd_clk);
            v = rd_q;
        end
    endtask
    task automatic wait_done;
        integer n;
        begin
            n = 0;
            @(posedge clk); @(posedge clk); @(posedge clk);
            while (status[0] && n < 50_000_000) begin @(posedge clk); n = n + 1; end
            if (status[0]) begin errors = errors + 1; $display("ОШИБКА: мост не закончил"); end
        end
    endtask
    // операция: N байтов, op
    task automatic op(input integer o, input integer n);
        begin reg_wr(2, {9'd0, o[2:0], n[19:0]}); wait_done(); end
    endtask

    // TX байтами: буфер байтов → слова с wbase
    reg [7:0] tb [0:2047];
    task automatic tx_bytes(input integer wbase, input integer n);
        integer k;
        reg [31:0] v;
        begin
            for (k = 0; k < (n + 3) / 4; k = k + 1) begin
                v = {tb[4*k], tb[4*k+1], tb[4*k+2], tb[4*k+3]};
                tx_wr(wbase + k, v);
            end
        end
    endtask

    // команда SD: 6 байтов XFER с PTR 0, затем WAIT — R1 в STAT[15:8]
    task automatic sd_cmd(input [5:0] idx, input [31:0] arg, input [7:0] crc, output [7:0] r1);
        begin
            tb[0] = {2'b01, idx}; tb[1] = arg[31:24]; tb[2] = arg[23:16]; tb[3] = arg[15:8];
            tb[4] = arg[7:0]; tb[5] = crc; tb[6] = 8'hFF; tb[7] = 8'hFF;
            reg_wr(1, {5'd0, 11'd514, 7'd0, 9'd0});
            tx_bytes(0, 8);
            op(0, 6);
            op(2, 16);
            r1 = status[15:8];
            if (status[1]) begin errors = errors + 1; $display("ОШИБКА: CMD%0d без ответа", idx); end
        end
    endtask

    // SCK: тактов clk на период и число импульсов
    integer sck_pulses = 0;
    real    t_rise = 0, sck_period = 0;
    always @(posedge sd_sck) begin
        sck_pulses = sck_pulses + 1;
        if (t_rise > 0) sck_period = $realtime - t_rise;
        t_rise = $realtime;
    end

    function automatic [15:0] crc16_tb(input integer base);
        integer k, b;
        reg [15:0] c;
        begin
            c = 16'd0;
            for (k = 0; k < 512; k = k + 1)
                for (b = 7; b >= 0; b = b - 1)
                    c = (c[15] ^ tb[base + k][b]) ? {c[14:0], 1'b0} ^ 16'h1021 : {c[14:0], 1'b0};
            crc16_tb = c;
        end
    endfunction

    integer   k, bad_bytes;
    reg [7:0] r1;
    reg [31:0] v, v2;

    initial begin
        #100; rst_n = 1'b1; #100;
        check("STAT после сброса", status, 32'h5D01_FF00);
        check("CTRL после сброса: ≤400 кГц, CS неактивен", ctrl_rd, {16'd7425, 7'd0, 1'b0, 8'd92});

        // 1. 80 тактов при неактивном CS
        sck_pulses = 0;
        op(1, 10);
        check("80 тактов при неактивном CS: импульсов", sck_pulses, 80);
        check("CS неактивен", sd_cs_n, 1);
        $display("SCK при включении: период %.0f нс (%.0f кГц)", sck_period, 1e6 / sck_period);
        if (sck_period < 2500.0) begin errors = errors + 1; $display("ОШИБКА: SCK быстрее 400 кГц"); end

        // 2. Инициализация на медленном SCK
        reg_wr(0, {23'd0, 1'b1, 8'd92});
        sd_cmd(6'd0, 32'd0, 8'h95, r1);  check("CMD0 R1", r1, 8'h01);
        // CMD8 с последним неполным словом: 6 байтов команды и 5 байтов 0xFF одним XFER
        for (k = 0; k < 16; k = k + 1) tb[k] = 8'hFF;
        tb[0] = 8'h48; tb[1] = 0; tb[2] = 0; tb[3] = 8'h01; tb[4] = 8'hAA; tb[5] = 8'h87;
        tx_bytes(0, 12);
        op(0, 11);
        rx_rd(1, v);  check("CMD8: RX слово 1 (байт 6 — NCR, 7 — R1)", v, 32'hFFFF_FF01);
        rx_rd(2, v);  check("CMD8: RX слово 2 (R7, хвост — 0xFF)", v, 32'h0000_01FF);
        op(1, 1);
        rx_rd(0, v);  check("CMD8: эхо шаблона", v[31:24], 8'hAA);
        k = 0;
        do begin
            sd_cmd(6'd55, 32'd0, 8'h65, r1);
            sd_cmd(6'd41, 32'h4000_0000, 8'h77, r1);
            k = k + 1;
        end while (r1 != 8'h00 && k < 10);
        check("ACMD41: готова", r1, 8'h00);
        sd_cmd(6'd58, 32'd0, 8'hFD, r1); check("CMD58 R1", r1, 8'h00);
        op(1, 4);
        rx_rd(0, v); check("CMD58: OCR (SDHC)", v, 32'hC0FF_8000);

        // 3. Быстрый SCK: div 1 — clk / 4
        reg_wr(0, {23'd0, 1'b1, 8'd1});
        sd_cmd(6'd16, 32'd512, 8'hFF, r1); check("CMD16 R1", r1, 8'h00);
        $display("SCK быстрый: период %.2f нс (%.2f МГц)", sck_period, 1e3 / sck_period);
        if (sck_period > 4.0 * CLK_NS + 1.0 || sck_period < 4.0 * CLK_NS - 1.0) begin
            errors = errors + 1; $display("ОШИБКА: SCK не clk/4");
        end

        // 3а. div 0 — как 1: SCK не быстрее clk / 4, ответ тот же
        reg_wr(0, {23'd0, 1'b1, 8'd0});
        check("div 0 читается как 1", ctrl_rd[8:0], {1'b1, 8'd1});
        sd_cmd(6'd16, 32'd512, 8'hFF, r1); check("CMD16 R1 при div 0", r1, 8'h00);
        if (sck_period < 4.0 * CLK_NS - 1.0) begin errors = errors + 1; $display("ОШИБКА: div 0 — SCK быстрее clk/4"); end

        // 4. Чтение блока 3 в RX с 0
        sd_cmd(6'd17, 32'd3, 8'hFF, r1); check("CMD17 R1", r1, 8'h00);
        reg_wr(1, {5'd0, 11'd514, 7'd0, 9'd0});
        op(3, 100_000);
        check("READ: маркер", status[15:8], 8'hFE);
        check("READ: не таймаут", status[1], 0);
        bad_bytes = 0;
        for (k = 0; k < 128; k = k + 1) begin
            rx_rd(k, v);
            if (v !== {card.mem[1536 + 4*k], card.mem[1537 + 4*k], card.mem[1538 + 4*k], card.mem[1539 + 4*k]})
                bad_bytes = bad_bytes + 1;
            {tb[4*k], tb[4*k+1], tb[4*k+2], tb[4*k+3]} = v;
        end
        check("READ блока 3: слов не тех", bad_bytes, 0);
        rx_rd(128, v);
        check("READ блока 3: CRC16 и хвост 0xFF", v, {crc16_tb(0), 16'hFFFF});

        // 5. То же в верхнюю половину буфера (PTR = 256)
        sd_cmd(6'd17, 32'd7, 8'hFF, r1);
        reg_wr(1, {5'd0, 11'd514, 7'd0, 9'd256});
        check("PTR читается", ptr_rd, {5'd0, 11'd514, 7'd0, 9'd256});
        op(3, 100_000);
        bad_bytes = 0;
        for (k = 0; k < 128; k = k + 1) begin
            rx_rd(256 + k, v);
            if (v !== {card.mem[3584 + 4*k], card.mem[3585 + 4*k], card.mem[3586 + 4*k], card.mem[3587 + 4*k]})
                bad_bytes = bad_bytes + 1;
        end
        check("READ блока 7 с PTR 256: слов не тех", bad_bytes, 0);
        // слова 0–1 RX переписал XFER команды CMD17; слово 2 — ещё блок 3
        rx_rd(2, v);
        check("PTR 256: блок 3 в начале RX цел", v, {card.mem[1544], card.mem[1545], card.mem[1546], card.mem[1547]});

        // 6. Запись блока 5: [FF FE данные CRC] одним XFER, ответ данных, BUSY
        sd_cmd(6'd24, 32'd5, 8'hFF, r1); check("CMD24 R1", r1, 8'h00);
        tb[0] = 8'hFF; tb[1] = 8'hFE;
        for (k = 0; k < 512; k = k + 1) tb[2 + k] = (k * 3 + 1) & 8'hFF;
        for (k = 0; k < 512; k = k + 1) tb[1024 + k] = tb[2 + k];
        v = crc16_tb(1024);
        tb[514] = v[15:8]; tb[515] = v[7:0];
        reg_wr(1, {5'd0, 11'd514, 7'd0, 9'd0});
        tx_bytes(0, 516);
        op(0, 516);
        op(2, 8);
        check("ответ данных: принято", status[12:8], 5'h05);
        op(4, 100_000);
        check("BUSY: дождались", status[1:0], 2'b00);
        check("BUSY: карта свободна", status[15:8], 8'hFF);
        check("карта: записей", card.n_write, 1);
        sd_cmd(6'd17, 32'd5, 8'hFF, r1);
        op(3, 100_000);
        bad_bytes = 0;
        for (k = 0; k < 128; k = k + 1) begin
            rx_rd(k, v);
            v2 = {tb[1024 + 4*k], tb[1025 + 4*k], tb[1026 + 4*k], tb[1027 + 4*k]};
            if (v !== v2) bad_bytes = bad_bytes + 1;
        end
        check("записанный блок 5 читается: слов не тех", bad_bytes, 0);

        // 7. Блок вне карты: маркер ошибки, RX не трогается
        sd_cmd(6'd17, 32'd99, 8'hFF, r1);
        op(3, 100_000);
        check("маркер ошибки", status[15:8], 8'h08);
        check("маркер ошибки: не таймаут", status[1], 0);
        rx_rd(2, v); check("после маркера ошибки RX цел", v, {tb[1032], tb[1033], tb[1034], tb[1035]});

        // 8. Ожидание без ответа (CS неактивен) — таймаут; N = 0 — сразу таймаут
        reg_wr(0, {23'd0, 1'b0, 8'd1});
        op(2, 10);
        check("WAIT без ответа: таймаут", status[1:0], 2'b10);
        reg_wr(2, {9'd0, 3'd2, 20'd0});
        @(posedge clk); @(posedge clk);
        check("WAIT N = 0: сразу таймаут, не занят", status[1:0], 2'b10);
        check("карта: плохих команд", card.n_bad, 0);

        $display("проверок %0d, ошибок %0d", checks, errors);
        if (errors) $display("ТЕСТ НЕ ПРОЙДЕН");
        else        $display("ТЕСТ ПРОЙДЕН");
        $finish;
    end
endmodule
