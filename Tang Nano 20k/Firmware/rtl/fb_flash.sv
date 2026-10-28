// SPI-flash Tang Nano 20K (выводы MSPI ПЛИС: MCLK 59, MCS_N 60, MO 61, MI 62; после загрузки
// ПЛИС они — обычные выводы, use_mspi_as_gpio) — настройки кадрового буфера и мост для ESP.
//
// ЗАГРУЗКА НАСТРОЕК. После сброса (BOOT_US) читается сектор настроек SET_ADDR (4 КБ):
// SLOTS ячеек по 32 байта, восемь слов старшим байтом вперёд:
//   0 MAGIC "VDST";
//   1 [3:0] MUX CLOCK, [5:4] MUX DELAY − 1, 6 MUX PHASE, 7 AUTO ALIGN, [15:8] формат (1),
//     16 ASPECT (0 — 4:3, 1 — 16:9), [18:17] SIZE (0 — 100 %, 1 — 95 %, 2 — 90 %),
//     19 FRAME BLEND (ядро 1.32); в записях до них — нули;
//   2…5 поправки окна по растрам: слово 2 + m/2, полуслово m%2 — {V ADJUST, H ADJUST} растра m;
//   6 [1:0] FT SIZE — размер картинки ядра FT812 (с ESP 0.1.104; ядро это слово только суммирует);
//   7 ~(сумма слов 0…6).
// Ячейки пишутся по порядку (это делает ESP через мост), сектор стирается, когда кончились.
// Чтение идёт до первой чистой ячейки (слово 0 = FFFFFFFF); берётся последняя целая —
// ячейка, недописанная при пропадании питания, не проходит по сумме и пропускается.
// Найденное выдаётся одним тактом load_stb.
//
// МОСТ (для ESP по LINK): буфер 64 байта (16 слов, байт 0 — старший байт слова 0) и команда
// cmd: [6:0] сколько байтов буфера передать, [14:8] сколько байтов принять в буфер
// (с байта 0), 16 — оставить CS активным (следующая команда продолжит ту же операцию
// flash). Протокол flash (чтение, запись страниц, стирание, состояние) — на стороне ESP.
// Пока busy, команды и запись буфера не принимаются по смыслу: ESP ждёт busy = 0.
//
// SCK flash — clk / 4 (18,6 МГц при 74,25 МГц): с запасом для любой команды 0x03.
//
module fb_flash #(
    parameter int          CLK_KHZ  = 74_250,
    parameter logic [23:0] SET_ADDR = 24'h7F_F000,
    parameter int          SLOTS    = 128,
    parameter int          BOOT_US  = 1_000      // ожидание после сброса
) (
    input  wire         clk,
    input  wire         rst_n,

    output reg          f_sck,
    output reg          f_cs_n,
    output reg          f_mosi,
    input  wire         f_miso,

    // Мост
    input  wire         buf_we,
    input  wire [3:0]   buf_waddr,
    input  wire [31:0]  buf_wdata,
    input  wire [3:0]   buf_raddr,
    output wire [31:0]  buf_rdata,
    input  wire         cmd_we,
    input  wire [31:0]  cmd,
    // 0 занят, 1 загрузка закончена, 2 настройки найдены, 3 CS удерживается;
    // [15:8] ячейка загруженных настроек; [23:16] первая чистая ячейка при загрузке (SLOTS — нет)
    output wire [31:0]  status,

    // Загруженные настройки
    output reg          load_stb,
    output reg  [11:0]  load_cfg,
    output reg  [127:0] load_adj
);
    localparam logic [31:0] MAGIC = 32'h5644_5354;
    localparam int          BOOT_CYCLES = CLK_KHZ * BOOT_US / 1000;
    localparam logic [7:0]  SLOT_LAST = SLOTS - 1;
    localparam logic [3:0]  GAP_LAST = 4'd15;          // CS неактивен между операциями 16 тактов

    // ------------------------------------------------------------------
    // Байт по SPI, режим 0: MOSI меняется после спада SCK, MISO читается перед спадом
    // ------------------------------------------------------------------
    reg       x_start;
    reg [7:0] x_byte;
    reg       x_busy, x_done;
    reg [1:0] x_ph;
    reg [2:0] x_bit;
    reg [7:0] x_tx, x_rx;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            f_sck <= 1'b0; f_mosi <= 1'b0;
            x_busy <= 1'b0; x_done <= 1'b0; x_ph <= '0; x_bit <= '0; x_tx <= '0; x_rx <= '0;
        end else begin
            x_done <= 1'b0;
            if (x_start) begin
                x_busy <= 1'b1; x_ph <= '0; x_bit <= 3'd7;
                x_tx <= x_byte; f_mosi <= x_byte[7];
            end else if (x_busy) begin
                x_ph <= x_ph + 2'd1;
                case (x_ph)
                    2'd0: f_sck <= 1'b1;
                    2'd2: begin f_sck <= 1'b0; x_rx <= {x_rx[6:0], f_miso}; end
                    2'd3: if (x_bit == 3'd0) begin
                              x_busy <= 1'b0; x_done <= 1'b1;
                          end else begin
                              x_bit  <= x_bit - 3'd1;
                              f_mosi <= x_tx[x_bit - 3'd1];
                          end
                    default: ;
                endcase
            end
        end
    end

    // ------------------------------------------------------------------
    // Буфер моста: четыре байтовые полосы по 16 слов в распределённой памяти (байт k слова —
    // полоса k); копия A — для чтения окном LINK, копия B — для передачи во flash. Порт
    // записи у полосы один: принятый из flash байт важнее записи LINK (её во время команды
    // по протоколу и не бывает — ESP ждёт busy = 0).
    // ------------------------------------------------------------------
    reg [7:0] ba0 [0:15], ba1 [0:15], ba2 [0:15], ba3 [0:15];
    reg [7:0] bb0 [0:15], bb1 [0:15], bb2 [0:15], bb3 [0:15];
    wire       rx_we;                     // байт из flash — в буфер (S_RX)
    wire [6:0] bp_w;                      // его номер
    wire [3:0] lane_we = rx_we ? (4'b0001 << bp_w[1:0]) : {4{buf_we}};
    wire [3:0] wa      = rx_we ? bp_w[5:2] : buf_waddr;
    wire [7:0] wd0 = rx_we ? x_rx : buf_wdata[31:24];
    wire [7:0] wd1 = rx_we ? x_rx : buf_wdata[23:16];
    wire [7:0] wd2 = rx_we ? x_rx : buf_wdata[15:8];
    wire [7:0] wd3 = rx_we ? x_rx : buf_wdata[7:0];
    always_ff @(posedge clk) begin
        if (lane_we[0]) begin ba0[wa] <= wd0; bb0[wa] <= wd0; end
        if (lane_we[1]) begin ba1[wa] <= wd1; bb1[wa] <= wd1; end
        if (lane_we[2]) begin ba2[wa] <= wd2; bb2[wa] <= wd2; end
        if (lane_we[3]) begin ba3[wa] <= wd3; bb3[wa] <= wd3; end
    end
    assign buf_rdata = {ba0[buf_raddr], ba1[buf_raddr], ba2[buf_raddr], ba3[buf_raddr]};
    // байт для передачи: idx — номер в буфере
    function automatic [7:0] tx_pick(input [5:0] idx, input [7:0] q0, input [7:0] q1,
                                     input [7:0] q2, input [7:0] q3);
        case (idx[1:0])
            2'd0: tx_pick = q0;
            2'd1: tx_pick = q1;
            2'd2: tx_pick = q2;
            default: tx_pick = q3;
        endcase
    endfunction

    // ------------------------------------------------------------------
    // Загрузка и мост
    // ------------------------------------------------------------------
    typedef enum logic [2:0] { S_WAIT, S_HDR, S_SCAN, S_IDLE, S_TX, S_RX, S_GAP } st_t;
    st_t st;
    reg [16:0]  wait_cnt;
    reg [1:0]   hdr_i;
    reg [4:0]   bi;              // байт в ячейке
    reg [7:0]   slot;
    reg [23:0]  wacc;
    reg [31:0]  sum;
    reg         magic_ok;
    reg [31:0]  t1, t2, t3, t4, t5;
    reg         boot_done, found;
    reg [7:0]   found_slot, free_slot;
    reg [6:0]   tx_n, rx_n;
    reg [6:0]   bp;
    reg         keep, busy;
    reg [3:0]   gap_cnt;

    assign rx_we = st == S_RX && x_done;
    assign bp_w  = bp;
    wire [5:0] tx_idx = st == S_IDLE ? 6'd0 : bp[5:0] + 6'd1;
    wire [7:0] tx_byte = tx_pick(tx_idx, bb0[tx_idx[5:2]], bb1[tx_idx[5:2]], bb2[tx_idx[5:2]],
                                 bb3[tx_idx[5:2]]);

    // Разбор ячейки: слово готово на четвёртом байте
    wire [31:0] word       = {wacc, x_rx};
    wire        word_end   = x_done && bi[1:0] == 2'd3;
    wire        slot_erased = word_end && bi[4:2] == 3'd0 && word == 32'hFFFF_FFFF;
    wire        slot_last  = word_end && bi[4:2] == 3'd7;
    wire        slot_valid = slot_last && magic_ok && word == ~sum;
    wire        scan_stop  = slot_erased || (slot_last && slot == SLOT_LAST);

    assign status = {8'd0, free_slot, found_slot, 4'd0, keep && !f_cs_n, found, boot_done, busy};

    function automatic [6:0] clamp64(input [6:0] n);
        clamp64 = (n > 7'd64) ? 7'd64 : n;
    endfunction

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            st <= S_WAIT; wait_cnt <= '0; hdr_i <= '0; bi <= '0; slot <= '0; wacc <= '0;
            sum <= '0; magic_ok <= 1'b0; t1 <= '0; t2 <= '0; t3 <= '0; t4 <= '0; t5 <= '0;
            boot_done <= 1'b0; found <= 1'b0; found_slot <= '0; free_slot <= '0;
            tx_n <= '0; rx_n <= '0; bp <= '0; keep <= 1'b0; busy <= 1'b1; gap_cnt <= '0;
            f_cs_n <= 1'b1; x_start <= 1'b0; x_byte <= '0;
            load_stb <= 1'b0; load_cfg <= '0; load_adj <= '0;
        end else begin
            x_start  <= 1'b0;
            load_stb <= 1'b0;
            case (st)
                // ---- загрузка настроек ----
                S_WAIT: begin
                    if (wait_cnt == BOOT_CYCLES[16:0]) begin
                        f_cs_n <= 1'b0;
                        x_start <= 1'b1; x_byte <= 8'h03; hdr_i <= '0;
                        st <= S_HDR;
                    end else wait_cnt <= wait_cnt + 17'd1;
                end
                S_HDR: if (x_done) begin
                    x_start <= 1'b1;
                    case (hdr_i)
                        2'd0: x_byte <= SET_ADDR[23:16];
                        2'd1: x_byte <= SET_ADDR[15:8];
                        2'd2: x_byte <= SET_ADDR[7:0];
                        default: begin x_byte <= 8'h00; bi <= '0; slot <= '0; st <= S_SCAN; end
                    endcase
                    hdr_i <= hdr_i + 2'd1;
                end
                S_SCAN: if (x_done) begin
                    wacc <= {wacc[15:0], x_rx};
                    if (word_end) begin
                        case (bi[4:2])
                            3'd0: begin magic_ok <= (word == MAGIC); sum <= word; end
                            3'd1: begin t1 <= word; sum <= sum + word; end
                            3'd2: begin t2 <= word; sum <= sum + word; end
                            3'd3: begin t3 <= word; sum <= sum + word; end
                            3'd4: begin t4 <= word; sum <= sum + word; end
                            3'd5: begin t5 <= word; sum <= sum + word; end
                            3'd6: sum <= sum + word;
                            default: if (slot != SLOT_LAST) slot <= slot + 8'd1;
                        endcase
                    end
                    if (slot_valid) begin
                        found <= 1'b1; found_slot <= slot;
                        load_cfg <= {t1[19:16], t1[7:0]};
                        load_adj <= {t5, t4, t3, t2};
                    end
                    bi <= bi + 5'd1;
                    if (scan_stop) begin
                        free_slot <= slot_erased ? slot : SLOTS[7:0];
                        f_cs_n <= 1'b1; boot_done <= 1'b1;
                        load_stb <= found || slot_valid;
                        gap_cnt <= '0; st <= S_GAP;
                    end else begin
                        x_start <= 1'b1; x_byte <= 8'h00;
                    end
                end
                // ---- мост ----
                S_IDLE: if (cmd_we) begin
                    tx_n <= clamp64(cmd[6:0]); rx_n <= clamp64(cmd[14:8]); keep <= cmd[16];
                    busy <= 1'b1; bp <= '0;
                    f_cs_n <= 1'b0;
                    if (cmd[6:0] != 7'd0) begin
                        x_start <= 1'b1; x_byte <= tx_byte; st <= S_TX;
                    end else if (cmd[14:8] != 7'd0) begin
                        x_start <= 1'b1; x_byte <= 8'h00; st <= S_RX;
                    end else begin
                        // пустая команда: только снять или оставить CS
                        if (!cmd[16]) begin f_cs_n <= 1'b1; gap_cnt <= '0; st <= S_GAP; end
                        else busy <= 1'b0;
                    end
                end
                S_TX: if (x_done) begin
                    if (bp + 7'd1 < tx_n) begin
                        bp <= bp + 7'd1;
                        x_start <= 1'b1; x_byte <= tx_byte;
                    end else if (rx_n != 7'd0) begin
                        bp <= '0;
                        x_start <= 1'b1; x_byte <= 8'h00; st <= S_RX;
                    end else if (keep) begin
                        busy <= 1'b0; st <= S_IDLE;
                    end else begin
                        f_cs_n <= 1'b1; gap_cnt <= '0; st <= S_GAP;
                    end
                end
                S_RX: if (x_done) begin
                    // байт — в буфер (rx_we)
                    if (bp + 7'd1 < rx_n) begin
                        bp <= bp + 7'd1;
                        x_start <= 1'b1; x_byte <= 8'h00;
                    end else if (keep) begin
                        busy <= 1'b0; st <= S_IDLE;
                    end else begin
                        f_cs_n <= 1'b1; gap_cnt <= '0; st <= S_GAP;
                    end
                end
                S_GAP: begin
                    if (gap_cnt == GAP_LAST) begin
                        busy <= 1'b0; st <= S_IDLE;
                    end else gap_cnt <= gap_cnt + 4'd1;
                end
                default: st <= S_IDLE;
            endcase
        end
    end
endmodule
