// SPDX-License-Identifier: Apache-2.0
//
// Вторая SD ZX Next (SD1, выбор FD в порту 0xE7) на VDAC4: карта Evo. Модуль ведёт себя как карта
// SDHC в режиме SPI на выводах мастера SPI Next (sd_sclk_o / sd_mosi_o zxnext_top — регистры
// CLK_28 по cpu_cen); команды отвечает сам, а сектор CMD17 / CMD24 просит у ESP через LINK
// (next_link: SD1_REQ, SD1_LBA, окно 0x4000). ESP передаёт запрос плагину WC VDAC4 Cores, плагин
// читает или пишет сектор карты Evo (SD Z-Controller) драйвером WC.
//
// Команды — те, что шлёт драйвер SD NextZXOS (ПЗУ 2 enNextZX.rom и enNxtmmc.rom: 0x18D6…0x1A88):
//   CMD0 → R1 01; CMD8 → R7 (01, 00, 00, напряжение, эхо); CMD55 + ACMD41 → 00 (карта готова);
//   CMD58 → R3 (00, C0 FF 80 00: питание есть, CCS = 1 — адрес в блоках); CMD16, CMD59, CMD12 → R1;
//   CMD13 → R2 (R1, 04 — прошлая запись не вышла, иначе 00); CMD17 — чтение блока; CMD24 — запись;
//   CMD9 → R1 00, токен FE, CSD 2.0 (16 байтов), CRC16; прочее (CMD18, CMD25…) → R1 с битом 2
//   «нет такой команды».
// CMD9 обязателен: NextZXOS при поиске карт (ПЗУ 2, 0x19C7 → 0x1904) после инициализации читает
// CSD и берёт из него размер карты (0x1843: (C_SIZE + 1) × 1024 блоков) — по нему проверяет номер
// каждого блока (0x1897). Отказ CMD9 — карты нет. Размер Evo ядро не знает, поэтому C_SIZE 3FFFFE —
// почти 2 ТБ (FFFFFC00 блоков): номер любого блока настоящей карты в него входит.
// Чтение: R1 00, затем FF, пока ESP не положит сектор (драйвер ждёт токен до 10 × 12 800 опросов —
// ~165 мс на 28 МГц), затем FE, 512 байтов, CRC16. Отказ ESP или срок — токен ошибки 01.
// Запись: R1 00, токен FE от Next, 512 байтов, два байта CRC (не проверяются), ответ 05 и «занято»
// (00), пока ESP не запишет сектор (драйвер ждёт «занято» без предела; здесь — не дольше
// WR_TMO_MS, затем ошибка в R2 CMD13).
//
// Выдача MISO. Мастер (spi_master_next_CE) берёт бит из регистра zxn_spi_miso, а тот — с вывода
// за такт cpu_cen до фронта SCK на выводе. При CPU 28 МГц (cpu_cen каждый такт) бит после спада
// SCK пришёл бы на такт позже, поэтому следующий бит выставляется сразу после фронта SCK: мастер
// читает его в последний такт перед следующим фронтом, как и бит реальной карты.
// Ответ на команду — через байт после CRC (NCR = 2): решение по принятому байту идёт в tx_next, а
// в сдвиг он попадает на границе следующего байта.
//
// present = 0 (плагин не обслуживает карту Evo) — карты нет: MISO всегда 1, как пустой слот.
module next_sd1 #(
    parameter int CLK_HZ    = 28125000,
    parameter int RD_TMO_MS = 1000,        // чтение: ESP не ответил — токен ошибки
    parameter int WR_TMO_MS = 5000         // запись: «занято» не дольше, затем ошибка в CMD13
) (
    input  wire        clk,
    input  wire        rst_n,
    // SPI Next (домен clk)
    input  wire        cs_n,               // SD1 выбрана — 0
    input  wire        sclk,
    input  wire        mosi,
    output wire        miso,
    // LINK (next_link)
    input  wire        present,            // карта есть (ESP: плагин обслуживает SD Evo)
    output reg         req,                // запрос сектора ждёт ESP
    output reg         req_wr,             // 1 — запись (данные — в буфере записи), 0 — чтение
    output reg  [31:0] req_lba,            // номер блока
    output reg  [7:0]  req_tag,            // номер запроса: ESP возвращает его в done_tag
    input  wire        done,               // строб: ESP закончил запрос
    input  wire [7:0]  done_tag,
    input  wire        done_ok,
    output wire [6:0]  rbuf_addr,          // буфер чтения (ESP → Next): слово, байт 0 — [7:0]
    input  wire [31:0] rbuf_q,             // на такт позже адреса
    output reg         wbuf_we,            // буфер записи (Next → ESP)
    output reg  [6:0]  wbuf_addr,
    output reg  [31:0] wbuf_d,
    output reg  [15:0] rd_cnt,             // секторов прочитано / записано (для журнала ESP)
    output reg  [15:0] wr_cnt
);
    localparam int RD_TMO = CLK_HZ / 1000 * RD_TMO_MS;
    localparam int WR_TMO = CLK_HZ / 1000 * WR_TMO_MS;

    // ------------------------------------------------------------------
    // Байты SPI (режим 0): приём по фронту SCK, выдача — сразу после фронта
    // ------------------------------------------------------------------
    reg        sclk_d;
    reg [2:0]  bitn;
    reg [7:0]  rx_sh;
    reg [7:0]  tx_sh;
    reg [7:0]  tx_next;                    // байт, что уйдёт следующим за текущим
    reg        byte_stb;                   // такт после восьмого фронта: принят rx_byte
    reg [7:0]  rx_byte;
    wire       rise = sclk && !sclk_d && !cs_n;
    // Поиск начала команды, как у карты: пока ничего не выдаётся (ждём команду, MISO — единицы),
    // единицы на MOSI в начале байта не считаются — байт начинается со стартового бита 0. Иначе
    // хвост байта, на который пришёлся спад CS (драйвер выбирает карту посреди передачи), сдвигал
    // счёт битов, и команда 0x40 приходила как 0xA0.
    wire       hunt;
    assign miso = tx_sh[7];

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            sclk_d <= 1'b0; bitn <= '0; rx_sh <= '0; tx_sh <= 8'hFF; byte_stb <= 1'b0; rx_byte <= '0;
        end else begin
            sclk_d   <= sclk;
            byte_stb <= 1'b0;
            if (cs_n) begin
                bitn  <= '0;
                tx_sh <= 8'hFF;
            end else if (rise && !(hunt && bitn == 3'd0 && mosi && tx_sh == 8'hFF)) begin
                rx_sh <= {rx_sh[6:0], mosi};
                bitn  <= bitn + 3'd1;
                if (bitn == 3'd7) begin
                    byte_stb <= 1'b1;
                    rx_byte  <= {rx_sh[6:0], mosi};
                    tx_sh    <= tx_next;
                end else begin
                    tx_sh <= {tx_sh[6:0], 1'b1};
                end
            end
        end
    end

    // CSD 2.0 (SDHC/SDXC): TAAC 0E, TRAN_SPEED 32 (25 МГц), CCC 5B5, READ_BL_LEN 9, C_SIZE 3FFFFE,
    // ERASE_BLK_EN 1, SECTOR_SIZE 7F, R2W_FACTOR 2, WRITE_BL_LEN 9; последний байт — CRC7 и 1
    function automatic [7:0] csd_byte(input [3:0] i);
        case (i)
        4'd0:  csd_byte = 8'h40;  4'd1:  csd_byte = 8'h0E;  4'd2:  csd_byte = 8'h00;  4'd3:  csd_byte = 8'h32;
        4'd4:  csd_byte = 8'h5B;  4'd5:  csd_byte = 8'h59;  4'd6:  csd_byte = 8'h00;  4'd7:  csd_byte = 8'h3F;
        4'd8:  csd_byte = 8'hFF;  4'd9:  csd_byte = 8'hFE;  4'd10: csd_byte = 8'h7F;  4'd11: csd_byte = 8'h80;
        4'd12: csd_byte = 8'h0A;  4'd13: csd_byte = 8'h40;  4'd14: csd_byte = 8'h00;  default: csd_byte = 8'h4D;
        endcase
    endfunction

    // CRC16-CCITT блока данных (x^16 + x^12 + x^5 + 1, начальное 0), байтом за такт
    function automatic [15:0] crc16_byte(input [15:0] c, input [7:0] d);
        reg [15:0] x;
        integer i;
        x = c;
        for (i = 7; i >= 0; i = i - 1)
            x = (x[15] ^ d[i]) ? {x[14:0], 1'b0} ^ 16'h1021 : {x[14:0], 1'b0};
        crc16_byte = x;
    endfunction

    // ------------------------------------------------------------------
    // Команды
    // ------------------------------------------------------------------
    localparam logic [2:0] S_IDLE = 3'd0, S_CMD = 3'd1, S_RESP = 3'd2, S_RD_WAIT = 3'd3,
                           S_RD_DATA = 3'd4, S_WR_TOKEN = 3'd5, S_WR_DATA = 3'd6, S_WR_BUSY = 3'd7;
    reg [2:0]   st, after;
    reg [5:0]   cmd;
    reg [31:0]  arg;
    reg [2:0]   cnt;
    reg [39:0]  resp;                      // ответ: старший байт — первым
    reg [2:0]   resp_n;
    reg         idle_st;                   // R1 бит 0: карта ещё не инициализирована
    reg         app;                       // прошла CMD55: следующая — ACMD
    reg         wr_err;                    // последняя запись не вышла (R2 CMD13)
    reg         csd;                       // идёт CMD9: блок данных — CSD, не сектор ESP
    reg         rd_ready, rd_ok;           // ESP положил сектор / с ошибкой
    reg [9:0]   idx;                       // байт блока: 0…511 данные, 512–513 CRC
    reg [15:0]  crc;
    reg [23:0]  wword;                     // три принятых байта слова записи
    reg [27:0]  tmo;
    wire [7:0]  r1 = {7'd0, idle_st};
    wire [7:0]  rd_byte = rbuf_q[{idx[1:0], 3'b000} +: 8];
    wire [7:0]  blk_byte = csd ? csd_byte(idx[3:0]) : rd_byte;   // байт блока данных
    wire        blk_end = csd ? idx[4] : idx[9];                 // данные кончились — CRC
    assign rbuf_addr = idx[8:2];
    assign hunt = st == S_IDLE && tx_next == 8'hFF;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            st <= S_IDLE; after <= S_IDLE; cmd <= '0; arg <= '0; cnt <= '0; resp <= '1; resp_n <= '0;
            idle_st <= 1'b1; app <= 1'b0; wr_err <= 1'b0; rd_ready <= 1'b0; rd_ok <= 1'b0; csd <= 1'b0;
            idx <= '0; crc <= '0; wword <= '0; tmo <= '0; tx_next <= 8'hFF;
            req <= 1'b0; req_wr <= 1'b0; req_lba <= '0; req_tag <= '0;
            wbuf_we <= 1'b0; wbuf_addr <= '0; wbuf_d <= '0; rd_cnt <= '0; wr_cnt <= '0;
        end else begin
            wbuf_we <= 1'b0;

            // итог ESP: только для своего запроса (старый, брошенный, не считается)
            if (done && req && done_tag == req_tag) begin
                req <= 1'b0;
                if (req_wr) begin
                    wr_err <= !done_ok;
                    if (done_ok) wr_cnt <= wr_cnt + 16'd1;
                end else begin
                    rd_ready <= 1'b1;
                    rd_ok    <= done_ok;
                    if (done_ok) rd_cnt <= rd_cnt + 16'd1;
                end
            end

            // сроки ожидания ESP
            if (req) begin
                tmo <= tmo + 28'd1;
                if (!req_wr && tmo == RD_TMO[27:0]) begin
                    req <= 1'b0; rd_ready <= 1'b1; rd_ok <= 1'b0;
                end
                if (req_wr && tmo == WR_TMO[27:0]) begin
                    req <= 1'b0; wr_err <= 1'b1;
                end
            end else begin
                tmo <= '0;
            end

            if (!present) begin
                // карты нет: всё сначала, MISO — единицы
                st <= S_IDLE; idle_st <= 1'b1; app <= 1'b0; tx_next <= 8'hFF;
                rd_ready <= 1'b0;
                if (!req_wr) req <= 1'b0;
            end else if (cs_n) begin
                // CS снят: команда брошена (чтение — ESP больше не ждём); запись дописывается
                if (st != S_WR_BUSY) begin
                    st <= S_IDLE;
                    tx_next <= 8'hFF;
                    rd_ready <= 1'b0;
                    if (!req_wr) req <= 1'b0;
                end
            end else if (byte_stb) begin
                case (st)
                S_IDLE: begin
                    tx_next <= 8'hFF;
                    if (rx_byte[7:6] == 2'b01) begin
                        cmd <= rx_byte[5:0];
                        cnt <= '0;
                        st  <= S_CMD;
                    end
                end

                S_CMD: begin
                    tx_next <= 8'hFF;
                    if (cnt != 3'd4) begin
                        arg <= {arg[23:0], rx_byte};
                        cnt <= cnt + 3'd1;
                    end else begin
                        // принят CRC — исполнить (CRC не проверяется, как с CMD59 «выкл»)
                        resp   <= {r1, 32'hFFFF_FFFF};
                        resp_n <= 3'd1;
                        after  <= S_IDLE;
                        app    <= 1'b0;
                        csd    <= 1'b0;
                        st     <= S_RESP;
                        case (cmd)
                        6'd0: begin
                            idle_st <= 1'b1;
                            resp <= {8'h01, 32'hFFFF_FFFF};
                        end
                        6'd8: begin
                            resp   <= {r1, 8'h00, 8'h00, {4'h0, arg[11:8]}, arg[7:0]};
                            resp_n <= 3'd5;
                        end
                        6'd55: app <= 1'b1;
                        6'd41: begin
                            if (app) begin
                                idle_st <= 1'b0;
                                resp <= {8'h00, 32'hFFFF_FFFF};
                            end else begin
                                resp <= {r1 | 8'h04, 32'hFFFF_FFFF};
                            end
                        end
                        6'd1: begin
                            idle_st <= 1'b0;
                            resp <= {8'h00, 32'hFFFF_FFFF};
                        end
                        6'd58: begin
                            resp   <= {r1, 32'hC0FF_8000};
                            resp_n <= 3'd5;
                        end
                        6'd12, 6'd16, 6'd59: ;
                        6'd13: begin
                            resp   <= {r1, wr_err ? 8'h04 : 8'h00, 24'hFFFFFF};
                            resp_n <= 3'd2;
                            wr_err <= 1'b0;
                        end
                        6'd9: begin
                            // CSD — блок данных, как сектор, но из ядра (ESP не нужен)
                            if (idle_st) resp <= {r1 | 8'h04, 32'hFFFF_FFFF};
                            else begin after <= S_RD_WAIT; csd <= 1'b1; end
                        end
                        6'd17: begin
                            if (idle_st) resp <= {r1 | 8'h04, 32'hFFFF_FFFF};
                            else after <= S_RD_WAIT;
                        end
                        6'd24: begin
                            if (idle_st) resp <= {r1 | 8'h04, 32'hFFFF_FFFF};
                            else after <= S_WR_TOKEN;
                        end
                        default: resp <= {r1 | 8'h04, 32'hFFFF_FFFF};
                        endcase
                    end
                end

                S_RESP: begin
                    tx_next <= resp[39:32];
                    resp    <= {resp[31:0], 8'hFF};
                    resp_n  <= resp_n - 3'd1;
                    if (resp_n == 3'd1) begin
                        st <= after;
                        if (after == S_RD_WAIT && csd) begin
                            // CSD готов сразу: токен — следующим байтом
                            rd_ready <= 1'b1; rd_ok <= 1'b1;
                        end else if (after == S_RD_WAIT) begin
                            // чтение: запрос к ESP — номер блока как есть (SDHC)
                            req <= 1'b1; req_wr <= 1'b0; req_lba <= arg; req_tag <= req_tag + 8'd1;
                            rd_ready <= 1'b0;
                        end
                    end
                end

                S_RD_WAIT: begin
                    if (rd_ready) begin
                        rd_ready <= 1'b0;
                        tx_next  <= rd_ok ? 8'hFE : 8'h01;
                        idx      <= '0;
                        crc      <= '0;
                        st       <= rd_ok ? S_RD_DATA : S_IDLE;
                    end else begin
                        tx_next <= 8'hFF;
                    end
                end

                S_RD_DATA: begin
                    if (!blk_end) begin
                        tx_next <= blk_byte;
                        crc     <= crc16_byte(crc, blk_byte);
                        idx     <= idx + 10'd1;
                    end else if (!idx[0]) begin
                        tx_next <= crc[15:8];
                        idx     <= idx + 10'd1;
                    end else begin
                        tx_next <= crc[7:0];
                        st      <= S_IDLE;
                    end
                end

                S_WR_TOKEN: begin
                    tx_next <= 8'hFF;
                    if (rx_byte == 8'hFE) begin
                        idx <= '0;
                        st  <= S_WR_DATA;
                    end else if (rx_byte != 8'hFF) begin
                        st <= S_IDLE;
                    end
                end

                S_WR_DATA: begin
                    tx_next <= 8'hFF;
                    if (!idx[9]) begin
                        wword <= {rx_byte, wword[23:8]};
                        if (idx[1:0] == 2'd3) begin
                            wbuf_we   <= 1'b1;
                            wbuf_addr <= idx[8:2];
                            wbuf_d    <= {rx_byte, wword};
                        end
                        idx <= idx + 10'd1;
                    end else if (!idx[0]) begin
                        idx <= idx + 10'd1;          // первый байт CRC
                    end else begin
                        // второй байт CRC: «данные приняты», затем «занято», пока пишет ESP
                        tx_next <= 8'h05;
                        req <= 1'b1; req_wr <= 1'b1; req_lba <= arg; req_tag <= req_tag + 8'd1;
                        st <= S_WR_BUSY;
                    end
                end

                S_WR_BUSY: begin
                    if (req) begin
                        tx_next <= 8'h00;
                    end else begin
                        tx_next <= 8'hFF;
                        st      <= S_IDLE;
                    end
                end

                default: st <= S_IDLE;
                endcase
            end
        end
    end
endmodule
