// Проверка fb_flash с моделью SPI-flash (tb/spi_flash_model.sv): загрузка настроек при старте
// и мост для ESP — так, как им будет пользоваться прошивка ESP.
//   iverilog -g2012 -DSIMULATION -I rtl -s tb_fb_flash -o t.vvp rtl/fb_flash.sv \
//       tb/spi_flash_model.sv tb/tb_fb_flash.sv && vvp -n t.vvp
`timescale 1ns/1ps
module tb_fb_flash;
    localparam logic [23:0] SET_ADDR = 24'h7F_F000;
    localparam real         CLK_NS   = 13.468;                  // 74,25 МГц

    reg clk = 1'b0, rst_n = 1'b0;
    always #(CLK_NS / 2.0) clk = ~clk;

    wire f_sck, f_cs_n, f_mosi, f_miso;
    reg         buf_we = 1'b0, cmd_we = 1'b0;
    reg  [3:0]  buf_waddr = '0, buf_raddr = '0;
    reg  [31:0] buf_wdata = '0, cmd = '0;
    wire [31:0] buf_rdata, status;
    wire        load_stb;
    wire [7:0]  load_cfg;
    wire [127:0] load_adj;

    fb_flash #(.CLK_KHZ(74_250), .SET_ADDR(SET_ADDR), .SLOTS(128), .BOOT_US(10)) dut (
        .clk(clk), .rst_n(rst_n),
        .f_sck(f_sck), .f_cs_n(f_cs_n), .f_mosi(f_mosi), .f_miso(f_miso),
        .buf_we(buf_we), .buf_waddr(buf_waddr), .buf_wdata(buf_wdata),
        .buf_raddr(buf_raddr), .buf_rdata(buf_rdata),
        .cmd_we(cmd_we), .cmd(cmd), .status(status),
        .load_stb(load_stb), .load_cfg(load_cfg), .load_adj(load_adj)
    );

    spi_flash_model #(.SET_ADDR(SET_ADDR)) flash (
        .sck(f_sck), .cs_n(f_cs_n), .mosi(f_mosi), .miso(f_miso)
    );

    integer errors = 0, checks = 0;
    task automatic err(input string s);
        begin errors = errors + 1; $display("[%0t] ОШИБКА: %0s", $time, s); end
    endtask
    task automatic expect32(input string what, input [31:0] got, input [31:0] want);
        begin
            checks = checks + 1;
            if (got !== want) err($sformatf("%0s: %08h, ожидалось %08h", what, got, want));
        end
    endtask

    // Загрузки за время одного старта
    integer loads = 0;
    reg [7:0]   last_cfg;
    reg [127:0] last_adj;
    always @(posedge clk) if (load_stb) begin
        loads = loads + 1; last_cfg = load_cfg; last_adj = load_adj;
    end

    // ---- запись ячейки в память модели напрямую ----
    function automatic [31:0] rec_word(input integer w, input [7:0] cfg, input [127:0] adj);
        reg [31:0] w1, sum;
        begin
            w1  = {16'd0, 8'h01, cfg};
            sum = 32'h5644_5354 + w1 + adj[31:0] + adj[63:32] + adj[95:64] + adj[127:96];
            case (w)
                0: rec_word = 32'h5644_5354;
                1: rec_word = w1;
                2: rec_word = adj[31:0];
                3: rec_word = adj[63:32];
                4: rec_word = adj[95:64];
                5: rec_word = adj[127:96];
                6: rec_word = 32'd0;
                default: rec_word = ~sum;
            endcase
        end
    endfunction

    task automatic put_slot(input integer slot, input [7:0] cfg, input [127:0] adj, input bit corrupt);
        integer w, b;
        reg [31:0] v;
        begin
            for (w = 0; w < 8; w = w + 1) begin
                v = rec_word(w, cfg, adj);
                if (corrupt && w == 3) v = v ^ 32'h0000_0100;
                for (b = 0; b < 4; b = b + 1)
                    flash.mem_set[slot * 32 + w * 4 + b] = v[31 - b * 8 -: 8];
            end
        end
    endtask

    task automatic erase_model;
        integer i;
        for (i = 0; i < 4096; i = i + 1) flash.mem_set[i] = 8'hFF;
    endtask

    // ---- старт Tang ----
    task automatic restart;
        begin
            rst_n = 1'b0; loads = 0;
            repeat (5) @(posedge clk);
            rst_n = 1'b1;
            wait (status[1]);
            wait (!status[0]);
            repeat (3) @(posedge clk);
        end
    endtask

    // ---- мост, как его ведёт ESP ----
    task automatic buf_put(input integer i, input [31:0] w);
        begin
            @(negedge clk); buf_we = 1'b1; buf_waddr = i[3:0]; buf_wdata = w;
            @(negedge clk); buf_we = 1'b0;
        end
    endtask
    // Снимок буфера моста (чтение по LINK — слово за словом)
    reg [31:0] bw [0:15];
    task automatic snap;
        integer i;
        for (i = 0; i < 16; i = i + 1) begin buf_raddr = i[3:0]; #1; bw[i] = buf_rdata; end
    endtask
    task automatic run_cmd(input integer tx, input integer rx, input bit keep);
        begin
            @(negedge clk); cmd_we = 1'b1; cmd = {15'd0, keep, 1'b0, rx[6:0], 1'b0, tx[6:0]};
            @(negedge clk); cmd_we = 1'b0;
            @(negedge clk);
            wait (!status[0]);
        end
    endtask
    // Байты в буфер подряд (байт 0 — старший байт слова 0)
    reg [7:0] bytes [0:63];
    task automatic bytes_to_buf(input integer n);
        integer i;
        for (i = 0; i < (n + 3) / 4; i = i + 1)
            buf_put(i, {bytes[i*4], bytes[i*4+1], bytes[i*4+2], bytes[i*4+3]});
    endtask
    task automatic buf_to_bytes(input integer n);
        integer i;
        begin
            snap();
            for (i = 0; i < (n + 3) / 4; i = i + 1) begin
                bytes[i*4] = bw[i][31:24]; bytes[i*4+1] = bw[i][23:16];
                bytes[i*4+2] = bw[i][15:8]; bytes[i*4+3] = bw[i][7:0];
            end
        end
    endtask

    task automatic flash_wait_ready;
        integer polls;
        begin
            polls = 0;
            forever begin
                bytes[0] = 8'h05; bytes_to_buf(1);
                run_cmd(1, 1, 1'b0);
                buf_to_bytes(1);
                if (!bytes[0][0]) break;
                polls = polls + 1;
                if (polls > 10000) begin err("flash не освободилась"); break; end
            end
        end
    endtask
    task automatic flash_wren;
        begin bytes[0] = 8'h06; bytes_to_buf(1); run_cmd(1, 0, 1'b0); end
    endtask

    // Записать ячейку через мост: WREN, 0x02 + адрес + 32 байта одной командой
    task automatic bridge_write_slot(input integer slot, input [7:0] cfg, input [127:0] adj);
        integer w, b;
        reg [23:0] a;
        reg [31:0] v;
        begin
            a = SET_ADDR + slot * 32;
            flash_wren();
            bytes[0] = 8'h02; bytes[1] = a[23:16]; bytes[2] = a[15:8]; bytes[3] = a[7:0];
            for (w = 0; w < 8; w = w + 1) begin
                v = rec_word(w, cfg, adj);
                for (b = 0; b < 4; b = b + 1) bytes[4 + w * 4 + b] = v[31 - b * 8 -: 8];
            end
            bytes_to_buf(36);
            run_cmd(36, 0, 1'b0);
            flash_wait_ready();
        end
    endtask

    localparam [127:0] ADJ_A = {16'h0102, 16'h0304, 16'h0506, 16'h0708, 16'h090A, 16'h0B0C, 16'h0D0E, 16'h0F10};
    localparam [127:0] ADJ_B = {16'hFB05, 16'h00FF, 16'h7F80, 16'h0000, 16'h1234, 16'h3FC1, 16'hFD03, 16'h02FE};
    localparam [127:0] ADJ_C = {8{16'hFE02}};

    initial begin : main
        integer i;
        reg [23:0] a;
        $display("=== fb_flash: загрузка настроек и мост");

        // 1. Чистый сектор: ничего не загружается, первая чистая ячейка — 0
        restart();
        expect32("чистый: найдено", status[2], 0);
        expect32("чистый: первая чистая", status[23:16], 0);
        expect32("чистый: загрузок", loads, 0);
        expect32("чистый: чтение остановилось на первой ячейке", flash.reads_set <= 8, 1);

        // 2. Ячейки 0 и 1 целые, 2 испорчена, дальше чисто → берётся 1
        put_slot(0, 8'h5B, ADJ_A, 1'b0);
        put_slot(1, 8'hA4, ADJ_B, 1'b0);
        put_slot(2, 8'h33, ADJ_C, 1'b1);
        restart();
        expect32("3 ячейки: найдено", status[2], 1);
        expect32("3 ячейки: ячейка", status[15:8], 1);
        expect32("3 ячейки: первая чистая", status[23:16], 3);
        expect32("3 ячейки: загрузок", loads, 1);
        expect32("3 ячейки: настройки", last_cfg, 8'hA4);
        checks = checks + 1;
        if (last_adj !== ADJ_B) err($sformatf("3 ячейки: поправки %032h", last_adj));

        // 3. JEDEC ID через мост
        bytes[0] = 8'h9F; bytes_to_buf(1);
        run_cmd(1, 3, 1'b0);
        snap();
        expect32("JEDEC ID", bw[0] & 32'hFFFF_FF00, 32'hEF40_1700);

        // 4. Ячейка 3 через мост, чтение обратно и новый старт → берётся 3
        bridge_write_slot(3, 8'hC7, ADJ_C);
        a = SET_ADDR + 3 * 32;
        bytes[0] = 8'h03; bytes[1] = a[23:16]; bytes[2] = a[15:8]; bytes[3] = a[7:0];
        bytes_to_buf(4);
        run_cmd(4, 32, 1'b0);
        snap();
        for (i = 0; i < 8; i = i + 1)
            expect32($sformatf("чтение ячейки 3, слово %0d", i), bw[i], rec_word(i, 8'hC7, ADJ_C));
        restart();
        expect32("после записи: ячейка", status[15:8], 3);
        expect32("после записи: первая чистая", status[23:16], 4);
        expect32("после записи: настройки", last_cfg, 8'hC7);

        // 5. Длинное чтение с удержанием CS: 96 байтов двумя командами = ячейки 0…2
        a = SET_ADDR;
        bytes[0] = 8'h03; bytes[1] = a[23:16]; bytes[2] = a[15:8]; bytes[3] = a[7:0];
        bytes_to_buf(4);
        run_cmd(4, 48, 1'b1);
        expect32("CS удерживается", status[3], 1);
        snap();
        for (i = 0; i < 8; i = i + 1)
            expect32($sformatf("длинное чтение, слово %0d", i), bw[i], rec_word(i, 8'h5B, ADJ_A));
        expect32("длинное чтение, слово 8", bw[8], rec_word(0, 8'hA4, ADJ_B));
        run_cmd(0, 48, 1'b0);
        expect32("CS снят", status[3], 0);
        // в буфере байты 48…95: слова 4…7 ячейки 1 и ячейка 2
        snap();
        for (i = 0; i < 4; i = i + 1)
            expect32($sformatf("длинное чтение, слово %0d", 12 + i), bw[i], rec_word(4 + i, 8'hA4, ADJ_B));

        // 6. Стирание сектора через мост → ничего не загружается
        flash_wren();
        a = SET_ADDR;
        bytes[0] = 8'h20; bytes[1] = a[23:16]; bytes[2] = a[15:8]; bytes[3] = a[7:0];
        bytes_to_buf(4);
        run_cmd(4, 0, 1'b0);
        flash_wait_ready();
        restart();
        expect32("после стирания: найдено", status[2], 0);
        expect32("после стирания: загрузок", loads, 0);

        // 7. Полный сектор: 128 целых ячеек → последняя, чистых нет
        for (i = 0; i < 128; i = i + 1) put_slot(i, i[7:0], {8{i[7:0], 8'h00}}, 1'b0);
        restart();
        expect32("полный: ячейка", status[15:8], 127);
        expect32("полный: первая чистая", status[23:16], 128);
        expect32("полный: настройки", last_cfg, 8'd127);
        expect32("полный: загрузок", loads, 1);

        expect32("нарушений протокола flash", flash.errors, 0);
        $display("проверок: %0d, ошибок: %0d", checks, errors);
        if (errors == 0) $display("ТЕСТ ПРОЙДЕН"); else $display("ТЕСТ НЕ ПРОЙДЕН");
        $finish;
    end

    initial begin
        #(50_000_000);
        $display("ТАЙМАУТ");
        $finish;
    end
endmodule
