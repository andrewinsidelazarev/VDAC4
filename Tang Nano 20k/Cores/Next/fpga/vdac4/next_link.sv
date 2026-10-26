// SPDX-License-Identifier: Apache-2.0
//
// LINK ядра ZX Next на VDAC4 (docs/next-core.md в проекте VDAC4): ESP32-S3 по QSPI
// (next_qspi, протокол ядер VDAC4) ведёт клавиатуру, мышь и джойстик Next — их шлёт с Evo
// плагин WC; забирает звук Next (на ЦАП ESP) и байты UART ESP Next (эмулятор ESP-AT на ESP);
// по записи BOOT просит у BL616 возврат в слот 0 (TS-Config).
//
// Регистры (байтовый адрес; R — читается, W — пишется):
//   0x00 ID          R  0x56444334 «VDC4», как у ядер VDAC4
//   0x04 VERSION     R  {личность 8'h03 (ZX Next), протокол LINK 8'h01, версия}
//   0x08 STATUS      R  0 — переполнение кольца UART (байты Next пропали; сброс — запись 1 в
//                       UART_CTL[0]), 1 — запрошен возврат в слот 0, [31:16] 0
//   0x0C UART_FREE   R  [6:0] свободно байтов в очереди UART_TX (из 64); [7] — передатчик ещё
//                       шлёт байт (старт…стоп): новый UART_DIV — только при 64 и [7] = 0 (0.3+)
//   0x10 KEY         W  [7:0] {отпущена, номер[6:0]} — одно событие клавиши на запись. Номер —
//                       код USB HID, модификаторы E0…E7 → 104…111 (как команда 1 hid.v
//                       FPGA-Companion) → карта 128 бит usb_keyboard ядра
//   0x14 MOUSE       W  [7:0] dx (вправо +), [15:8] dy (вверх +, как PS/2), [18:16] кнопки
//                       {средняя, правая, левая} → ps2_mouse (строб меняется на каждую запись)
//   0x18 JOY         W  [7:0] joystick0: 0 вправо, 1 влево, 2 вниз, 3 вверх, 4 огонь (A),
//                       5 B, 6 C, 7 START → joy_left
//   0x1C KEYS_CLEAR  W  любое значение — все клавиши отпущены, кнопки мыши и джойстик — 0
//   0x20 AUDIO_WP    R  [10:0] указатель записи кольца звука (код Грея), [31:16] кадров
//                       по 2048 отсчётов (двоичный, для оценки частоты)
//   0x24 AUDIO_DIV   R  [15:0] делитель такта ядра на отсчёт звука (частота = такт / делитель)
//   0x28 CLK_KHZ     R  такт ядра, кГц (28 125)
//   0x30 UART_WP     R  [9:0] указатель записи кольца байтов UART Next → ESP (код Грея)
//   0x34 UART_RP     W  [9:0] указатель чтения этого кольца (двоичный): что ESP уже забрала
//   0x38 UART_TX     W  байты ESP → UART Next: [7:0], [15:8], [23:16]; [25:24] — сколько (1…3)
//   0x3C SCRATCH     RW проверка канала — там же, где у ядер VDAC4: по нему ESP узнаёт ядро
//                       после перезагрузки ПЛИС и проверяет LINK (tang_link.cpp)
//   0x40 UART_DIV    RW [15:0] делитель такта ядра на бит UART моста (115200 — 244 при 28,125 МГц);
//                       скорость меняет ESP по AT+UART_CUR эмулятора ESP-AT
//   0x44 UART_CTL    W  бит 0 = 1 — сбросить флаг переполнения (бит 0 STATUS)
//   0x48..0x74 и окна 0x4000..0x7FFC: диагностика удалена, чтение возвращает 0.
//   0x80 SPEED_PREF RW выбор частоты для повторного START. R: 5350 vv ~vv,
//                      vv[2] — выбор есть, vv[1:0] — частота. W: 53500004 | частота.
//   0xF0 BOOT        W  0x424F4F54 («BOOT») — вернуться в слот 0: запрос уходит BL616 (next_cp)
// Окна (только чтение, из BSRAM по фронту SCK):
//   0x1000…0x1FFC   кольцо UART Next → ESP: слово — байт [7:0], 1024 байта
//   0x2000…0x3FFC   кольцо звука: {unsigned L[12:0], 000, unsigned R[12:0], 000}, 2048 отсчётов
//
// Регистры ESP читает как есть (как у fb_link). Указатели колец — код Грея из регистров домена
// ядра (за шаг меняется один разряд: чтение LINK посреди шага даёт старое или новое значение, и
// не раньше записи ячейки); UART_FREE — тоже из регистра, но двоичный: ESP верит двум одинаковым
// чтениям подряд (разбор Codex 21.10.2026, № 6 и № 17).
module next_link #(
    parameter logic [15:0] VERSION   = 16'h0001,
    parameter int          CLK_KHZ   = 28125,       // такт ядра Next (CLKOUTD PLL NextNano)
    parameter int          AUDIO_DIV = 586,         // 28,125 МГц / 586 ≈ 47 995 Гц
    parameter int          UART_DIV  = 244          // 28,125 МГц / 244 ≈ 115 266 бод
) (
    // LINK (домен SCK)
    input  wire         sck,
    input  wire         cs_n,
    input  wire [3:0]   io_in,
    output wire [3:0]   io_out,
    output wire         io_oe,

    // домен ядра (CLK_28)
    input  wire         clk,
    input  wire         rst_n,
    output reg  [127:0] keyboard,
    output reg  [7:0]   mouse_dx,
    output reg  [7:0]   mouse_dy,
    output reg  [2:0]   mouse_btns,      // {средняя, правая, левая}
    output reg          mouse_strobe,
    output reg  [7:0]   joystick0,
    input  wire [12:0]  audio_l,         // audio_mixer: беззнаковые 13 бит
    input  wire [12:0]  audio_r,
    input  wire         uart_tx,         // линия TX UART ESP Next (8N1)
    output wire         uart_rx,         // линия RX UART ESP Next
    input  wire [2:0]   speed_saved_in,
    output reg  [1:0]   speed_saved_out,
    output reg          speed_restore,
    output reg          boot_req
);
    // ------------------------------------------------------------------
    // QSPI
    // ------------------------------------------------------------------
    wire        wr_stb;
    wire [17:0] wr_addr;
    wire [31:0] wr_data;
    wire [12:0] rd_addr;
    reg  [31:0] rd_data;

    next_qspi #(.DUMMY(8)) qspi (
        .sck(sck), .cs_n(cs_n), .io_in(io_in), .io_out(io_out), .io_oe(io_oe),
        .clk(clk), .rst_n(rst_n), .wr_stb(wr_stb), .wr_addr(wr_addr), .wr_data(wr_data),
        .rd_addr(rd_addr), .rd_data(rd_data)
    );

    // Восстановление не зависит от F4/F1: reset LINK — только PLL.
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            speed_restore <= 1'b0;
            speed_saved_out <= 2'b00;
        end else begin
            speed_restore <= 1'b0;
            if (wr_stb && wr_addr == 18'h20 && wr_data[31:2] == 30'h14D40001) begin
                speed_saved_out <= wr_data[1:0];
                speed_restore <= 1'b1;
            end
        end
    end

    function automatic [10:0] gray11(input [10:0] b); gray11 = b ^ (b >> 1); endfunction
    function automatic [9:0]  gray10(input [9:0] b);  gray10 = b ^ (b >> 1); endfunction

    // ------------------------------------------------------------------
    // Звук: отсчёт каждые AUDIO_DIV тактов → кольцо 2048 × 32 (BSRAM, два такта)
    // ------------------------------------------------------------------
    reg  [15:0] a_cnt;
    reg  [10:0] a_wp;
    reg  [15:0] a_wraps;
    reg  [31:0] aram [0:2047] /* synthesis syn_ramstyle = "block_ram" */;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            a_cnt <= '0; a_wp <= '0; a_wraps <= '0;
        end else if (a_cnt == AUDIO_DIV[15:0] - 16'd1) begin
            a_cnt <= '0;
            a_wp  <= a_wp + 11'd1;
            if (a_wp == 11'd2047) a_wraps <= a_wraps + 16'd1;
        end else begin
            a_cnt <= a_cnt + 16'd1;
        end
    end
    // порт записи — из регистров, как BSRAM ядер VDAC4 (cp_link); чтение — по SCK LINK
    reg         a_we;
    reg  [10:0] a_wa;
    reg  [31:0] a_wd;
    always @(posedge clk) begin
        a_we <= rst_n && a_cnt == AUDIO_DIV[15:0] - 16'd1;
        a_wa <= a_wp;                                    // ячейка — до шага указателя
        a_wd <= {audio_l, 3'b000, audio_r, 3'b000};
    end
    reg [31:0] a_q;
    always @(posedge clk)
        if (a_we)
            aram[a_wa] <= a_wd;
    always @(posedge sck) a_q <= aram[rd_addr[10:0]];
    // AUDIO_WP для LINK: Грей из регистра, на такт позже шага — вместе с записью ячейки
    reg  [10:0] a_wp_g;
    reg  [15:0] a_wraps_r;
    always_ff @(posedge clk or negedge rst_n)
        if (!rst_n) begin a_wp_g <= '0; a_wraps_r <= '0; end
        else begin a_wp_g <= gray11(a_wp); a_wraps_r <= a_wraps; end

    // ------------------------------------------------------------------
    // UART Next → ESP: приёмник 8N1 → кольцо 1024 × 8
    // ------------------------------------------------------------------
    reg  [15:0] u_div;
    reg  [1:0]  u_sync;
    reg  [15:0] ur_cnt;
    reg  [3:0]  ur_bit;              // 0 — ждём старт, 1…8 — данные, 9 — стоп
    reg  [7:0]  ur_sh;
    reg         ur_busy;
    reg  [9:0]  u_wp;
    reg  [9:0]  u_rp;                // от ESP
    reg         u_ovf;
    reg  [7:0]  uram [0:1023] /* synthesis syn_ramstyle = "block_ram" */;
    reg         ur_put;
    reg  [7:0]  ur_byte;
    wire        u_line = u_sync[1];
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            u_sync <= 2'b11; ur_cnt <= '0; ur_bit <= '0; ur_sh <= '0; ur_busy <= 1'b0;
            ur_put <= 1'b0; ur_byte <= '0;
        end else begin
            u_sync <= {u_sync[0], uart_tx};
            ur_put <= 1'b0;
            if (!ur_busy) begin
                if (!u_line) begin                       // начало старт-бита
                    ur_busy <= 1'b1; ur_bit <= 4'd0; ur_cnt <= {1'b0, u_div[15:1]};
                end
            end else if (ur_cnt != 16'd0) begin
                ur_cnt <= ur_cnt - 16'd1;
            end else begin
                ur_cnt <= u_div - 16'd1;
                if (ur_bit == 4'd0) begin
                    if (u_line) ur_busy <= 1'b0;         // помеха, не старт
                    else ur_bit <= 4'd1;
                end else if (ur_bit <= 4'd8) begin
                    ur_sh  <= {u_line, ur_sh[7:1]};
                    ur_bit <= ur_bit + 4'd1;
                end else begin                           // стоп-бит
                    ur_busy <= 1'b0;
                    if (u_line) begin ur_put <= 1'b1; ur_byte <= ur_sh; end
                end
            end
        end
    end
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            u_wp <= '0; u_ovf <= 1'b0;
        end else begin
            if (ur_put) begin
                if (u_wp + 10'd1 == u_rp) u_ovf <= 1'b1;     // ESP не забрала: байт пропал
                else u_wp <= u_wp + 10'd1;
            end
            if (wr_stb && wr_addr == 18'h11 && wr_data[0]) u_ovf <= 1'b0;   // UART_CTL
        end
    end
    reg         u_we;
    reg  [9:0]  u_wa;
    reg  [7:0]  u_wd;
    always @(posedge clk) begin
        u_we <= rst_n && ur_put && u_wp + 10'd1 != u_rp;
        u_wa <= u_wp;
        u_wd <= ur_byte;
    end
    reg [7:0] u_q;
    always @(posedge clk)
        if (u_we)
            uram[u_wa] <= u_wd;
    always @(posedge sck) u_q <= uram[rd_addr[9:0]];
    reg  [9:0]  u_wp_g;                  // UART_WP для LINK: Грей из регистра (как a_wp_g)
    always_ff @(posedge clk or negedge rst_n)
        if (!rst_n) u_wp_g <= '0;
        else        u_wp_g <= gray10(u_wp);

    // ------------------------------------------------------------------
    // UART ESP → Next: очередь 64 байта → передатчик 8N1
    // ------------------------------------------------------------------
    reg  [7:0]  tq [0:63];
    reg  [6:0]  tq_n;
    reg  [5:0]  tq_rd, tq_wr;
    reg  [15:0] ut_cnt;
    reg  [3:0]  ut_bit;              // 0 — простой, 1 — старт, 2…9 — данные, 10 — стоп
    reg  [7:0]  ut_sh;
    reg         ut_line;
    assign uart_rx = ut_line;
    wire        tq_pop = ut_bit == 4'd0 && tq_n != 7'd0;

    // ------------------------------------------------------------------
    // Регистры записи
    // ------------------------------------------------------------------
    reg  [31:0] scratch;
    // Наблюдатели SD/CPU/RAM и CRC отсутствуют в этом проверочном варианте.
    wire        is_uart  = wr_stb && wr_addr == 18'h0E;
    wire [1:0]  u_count  = wr_data[25:24];
    function automatic [6:0] room(input [6:0] n); room = 7'd64 - n; endfunction

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            keyboard <= '0; mouse_dx <= '0; mouse_dy <= '0; mouse_btns <= '0; mouse_strobe <= 1'b0;
            joystick0 <= '0; scratch <= '0; u_div <= UART_DIV[15:0]; u_rp <= '0; boot_req <= 1'b0;
        end else begin
            if (wr_stb) begin
            case (wr_addr)
                18'h0F: scratch <= wr_data;
                18'h04: keyboard[wr_data[6:0]] <= ~wr_data[7];
                18'h05: begin
                    mouse_dx <= wr_data[7:0]; mouse_dy <= wr_data[15:8];
                    mouse_btns <= wr_data[18:16]; mouse_strobe <= ~mouse_strobe;
                end
                18'h06: joystick0 <= wr_data[7:0];
                18'h07: begin keyboard <= '0; mouse_btns <= '0; joystick0 <= '0; end
                18'h0D: u_rp <= wr_data[9:0];
                18'h10: u_div <= (wr_data[15:0] < 16'd4) ? 16'd4 : wr_data[15:0];
                18'h3C: if (wr_data == 32'h424F_4F54) boot_req <= 1'b1;
                default: ;
            endcase
            end
        end
    end

    // UART_FREE для LINK — из регистра: {передатчик шлёт, свободно}
    reg  [7:0]  u_free;
    always_ff @(posedge clk or negedge rst_n)
        if (!rst_n) u_free <= 8'd64;
        else        u_free <= {ut_bit != 4'd0, room(tq_n)};

    // очередь UART_TX и передатчик
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            tq_n <= '0; tq_rd <= '0; tq_wr <= '0; ut_cnt <= '0; ut_bit <= '0; ut_sh <= '0;
            ut_line <= 1'b1;
        end else begin
            // приём из LINK: до 3 байтов, если влезают все
            logic [6:0] add;
            add = (is_uart && u_count != 2'd0 && room(tq_n) >= {5'd0, u_count}) ? {5'd0, u_count} : 7'd0;
            if (add != 7'd0) begin
                tq[tq_wr] <= wr_data[7:0];
                if (u_count >= 2'd2) tq[tq_wr + 6'd1] <= wr_data[15:8];
                if (u_count == 2'd3) tq[tq_wr + 6'd2] <= wr_data[23:16];
                tq_wr <= tq_wr + add[5:0];
            end
            tq_n <= tq_n + add - (tq_pop ? 7'd1 : 7'd0);
            if (tq_pop) begin
                ut_sh <= tq[tq_rd]; tq_rd <= tq_rd + 6'd1;
                ut_bit <= 4'd1; ut_cnt <= u_div - 16'd1; ut_line <= 1'b0;   // старт-бит
            end else if (ut_bit != 4'd0) begin
                if (ut_cnt != 16'd0) begin
                    ut_cnt <= ut_cnt - 16'd1;
                end else begin
                    ut_cnt <= u_div - 16'd1;
                    if (ut_bit <= 4'd8) begin
                        ut_line <= ut_sh[0]; ut_sh <= {1'b1, ut_sh[7:1]}; ut_bit <= ut_bit + 4'd1;
                    end else if (ut_bit == 4'd9) begin
                        ut_line <= 1'b1; ut_bit <= 4'd10;            // стоп-бит
                    end else begin
                        ut_bit <= 4'd0;
                    end
                end
            end
        end
    end

    // ------------------------------------------------------------------
    // Чтение: регистры — как есть, окна — BSRAM по фронту SCK
    // ------------------------------------------------------------------

    always_comb begin
        if (rd_addr[12])
            rd_data = 32'd0; // прежние окна журналов
        else if (rd_addr[11])
            rd_data = a_q;                               // 0x2000…0x3FFC
        else if (rd_addr[10])
            rd_data = {24'd0, u_q};                      // 0x1000…0x1FFC
        else case (rd_addr[7:0])
            8'h00: rd_data = 32'h5644_4334;
            8'h01: rd_data = {8'h03, 8'h01, VERSION};
            8'h02: rd_data = {30'd0, boot_req, u_ovf};
            8'h03: rd_data = {24'd0, u_free};
            8'h08: rd_data = {a_wraps_r, 5'd0, a_wp_g};
            8'h09: rd_data = {16'd0, AUDIO_DIV[15:0]};
            8'h0A: rd_data = CLK_KHZ;
            8'h0C: rd_data = {22'd0, u_wp_g};
            8'h0F: rd_data = scratch;
            8'h10: rd_data = {16'd0, u_div};
            8'h20: rd_data = {16'h5350, 5'd0, speed_saved_in, 5'b11111, ~speed_saved_in};
            default: rd_data = 32'd0;
        endcase
    end
endmodule
