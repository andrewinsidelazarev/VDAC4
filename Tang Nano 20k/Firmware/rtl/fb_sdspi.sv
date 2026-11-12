// SPDX-License-Identifier: Apache-2.0
//
// Мост к microSD самой Tang Nano 20K для ESP (ядро TS-Config, слот 0; docs/next-core.md; с 1.47 — и
// ядро видео, слот 3): карта в режиме SPI на выводах слота CS 81, MOSI 82, SCK 83, MISO 84. DAT1 и
// DAT2 слота (85, 80) на VDAC4 — LINK IRQ и PCLK Evo; в режиме SPI карта их не ведёт. Протокол SD
// (CMD0, CMD8, ACMD41, CMD17, CMD18, CMD24 …) и FatFs — на ESP. Мост делает побайтный обмен,
// ожидание ответа карты и приём блока после маркера: блок 512 байтов ESP читает за ~18 обменов LINK
// (запись — за ~22), а не побайтно (тест прошивки ESP tests/test_sd_bridge_host.py).
//
// БУФЕРЫ (байт 0 — старший байт слова, как у моста к flash):
//   TX — 2^TX_AW слов: пишет ESP (окно LINK), читает мост; у TS-Config 512 слов (BSRAM), у ядра
//        видео 16 (распределённая память: BSRAM там нет, а карту оно только читает — команды);
//   RX — 512 слов (BSRAM): пишет мост, читает ESP (окно LINK, домен SCK LINK).
// РЕГИСТРЫ:
//   CTRL  [7:0] div: полупериод SCK — div + 1 тактов clk (SCK = clk / (2·(div + 1))); div 0 — как
//         1 (SCK не быстрее clk / 4): MISO берётся из регистра входа, а карта меняет его по спаду
//         SCK с задержкой до 14 нс;
//         8 — CS активен (CS = 0). Чтение — то же и [31:16] такт моста, десятки кГц.
//         После сброса — не быстрее 400 кГц, CS неактивен.
//   PTR   [8:0] слово буферов, с которого идёт обмен (TX и RX — с одного места);
//         [26:16] байтов блока для READ и STREAM (после маркера 0xFE: 512 данных и 2 CRC — 514).
//   CMD   (запись запускает операцию; пока занят — не принимается) [19:0] N, [22:20] операция:
//         0 XFER — N байтов из TX, ответ карты — в RX;
//         1 FILL — N байтов 0xFF, ответ — в RX;
//         2 WAIT — 0xFF, пока карта отвечает 0xFF, не больше N байтов (R1, маркер данных);
//         3 READ — как WAIT; пришёл маркер 0xFE — ещё PTR[26:16] байтов 0xFF, ответ — в RX;
//         4 BUSY — 0xFF, пока карта не ответит 0xFF (держит 0, пока пишет), не больше N байтов;
//         5 STREAM (только ROUTE = 1, ядро видео 1.47) — блоки CMD18 мимо RX, в поток r_*
//           (медиа-FIFO): STRM[11:0] блоков, у каждого — ожидание маркера (первого — не больше N
//           байтов, следующих — не больше 2^20), PTR[26:16] байтов блока (514), из них все, кроме
//           двух последних (CRC16), — в поток словами, первый байт — младший ([7:0], порядок
//           медиа-FIFO). Поток не взял слово (r_ready = 0) — мост ждёт перед следующим байтом: SCK
//           стоит, карта ждёт (режим SPI это допускает). CRC16 сверяется (по всем байтам блока
//           остаток — 0): не сошёлся — STAT 2, данные блока уже в потоке (повтора нет: битый кадр
//           отбросит сторож декодера). Маркер ошибки (не 0xFF и не 0xFE) или таймаут — операция
//           кончается (STAT[15:8] — тот байт; ESP узнаёт, что поток оборвался). Начало и хвост
//           файла не на границе блока ESP шлёт сам (окно MF_DATA) — здесь только целые блоки.
//           CMD12 (стоп чтения) шлёт ESP — XFER.
//   STAT  0 — занят; 1 — WAIT, READ, BUSY, STREAM: за N байтов не дождались; 2 — STREAM: CRC
//         блока не сошёлся; [15:8] последний принятый байт (у WAIT и READ — тот, что не 0xFF: R1,
//         маркер или ошибка данных; у STREAM — маркер последнего блока); [31:16] 0x5D01.
//   STRM  (ROUTE = 1, только запись) [11:0] блоков операции STREAM.
// Ответ в RX ложится словами: слово пишется, когда собраны его 4 байта или кончился обмен; байты
// слова за последним — 0xFF.
//
// SPI режим 0: MOSI меняется при SCK = 0, MISO берётся по фронту SCK (из регистра входа — значение
// за такт до фронта: карта выставила его по спаду, полупериод назад).
//
module fb_sdspi #(
    parameter int CLK_KHZ = 74_250,
    parameter int TX_AW   = 9,             // слов TX — 2^TX_AW (9 — BSRAM, меньше — распределённая)
    parameter bit ROUTE   = 1'b0           // операция STREAM и поток r_*
) (
    input  wire        clk,
    input  wire        rst_n,

    output reg         sd_cs_n,
    output reg         sd_sck,
    output reg         sd_mosi,
    input  wire        sd_miso,

    // Регистры (домен clk)
    input  wire        ctrl_we,
    input  wire        ptr_we,
    input  wire        cmd_we,
    input  wire        strm_we,
    input  wire [31:0] wdata,
    output wire [31:0] ctrl_rd,
    output wire [31:0] ptr_rd,
    output wire [31:0] status,
    // Окно TX (запись, домен clk)
    input  wire        tx_we,
    input  wire [8:0]  tx_waddr,
    input  wire [31:0] tx_wdata,
    // Окно RX (чтение, домен rd_clk — SCK LINK)
    input  wire        rd_clk,
    input  wire [8:0]  rd_addr,
    output reg  [31:0] rd_q,
    // Поток STREAM (домен clk): слово держится, пока его не возьмут
    output wire        r_valid,
    output wire [31:0] r_data,
    input  wire        r_ready
);
    localparam logic [7:0]  DEF_DIV = (CLK_KHZ + 799) / 800 - 1;   // ≤ 400 кГц
    localparam logic [15:0] CLK_10K = CLK_KHZ / 10;
    localparam logic [2:0]  OP_XFER = 3'd0, OP_FILL = 3'd1, OP_WAIT = 3'd2, OP_READ = 3'd3,
                            OP_BUSY = 3'd4, OP_STRM = 3'd5;
    localparam logic [19:0] WAIT_NEXT = 20'hFFFFF;      // STREAM: маркер следующего блока (~0,45 с)

    // ------------------------------------------------------------------
    // Буферы: TX — запись окна LINK, чтение мостом; RX (BSRAM) — запись мостом, чтение окна LINK
    // ------------------------------------------------------------------
    reg  [31:0] rxm [0:511] /* synthesis syn_ramstyle = "block_ram" */;
    reg  [8:0]  wa;                     // слово буферов текущего байта
    reg  [31:0] tx_q;
    reg         rx_we;
    reg  [8:0]  rx_wa;
    reg  [31:0] rx_wd;

    always @(posedge clk)
        if (rx_we) rxm[rx_wa] <= rx_wd;
    generate if (TX_AW >= 8) begin : g_tx_bsram
        // запись окна — через регистр (BSRAM далеко от LINK)
        reg         tx_we_q;
        reg  [8:0]  tx_wa_q;
        reg  [31:0] tx_wd_q;
        reg [31:0] txm [0:(1 << TX_AW) - 1] /* synthesis syn_ramstyle = "block_ram" */;
        always @(posedge clk) begin
            tx_we_q <= tx_we;
            tx_wa_q <= tx_waddr;
            tx_wd_q <= tx_wdata;
            if (tx_we_q) txm[tx_wa_q[TX_AW-1:0]] <= tx_wd_q;
            tx_q <= txm[wa[TX_AW-1:0]];
        end
    end else begin : g_tx_lut
        reg [31:0] txm [0:(1 << TX_AW) - 1] /* synthesis syn_ramstyle = "distributed_ram" */;
        always @(posedge clk) begin
            if (tx_we) txm[tx_waddr[TX_AW-1:0]] <= tx_wdata;
            tx_q <= txm[wa[TX_AW-1:0]];
        end
    end endgenerate
    always @(posedge rd_clk) rd_q <= rxm[rd_addr];

    // ------------------------------------------------------------------
    // Обмен
    // ------------------------------------------------------------------
    localparam logic [2:0] S_IDLE = 3'd0, S_FETCH = 3'd1, S_FETCH2 = 3'd2, S_PICK = 3'd3,
                           S_BIT = 3'd4, S_BYTE = 3'd5;
    reg  [2:0]  st;
    reg  [7:0]  div, div_cnt;
    reg         cs_on;
    reg  [8:0]  ptr;
    reg  [10:0] blk_len;
    reg  [2:0]  op;
    reg  [19:0] left;                   // байтов осталось (у READ и STREAM после маркера — блока)
    reg         in_blk;                 // READ, STREAM: идёт приём блока
    reg  [1:0]  lane;
    reg  [31:0] rxw;                    // собираемое слово ответа (у STREAM — слово потока)
    reg  [7:0]  tsr, rsr, last;
    reg  [2:0]  bitn;
    reg         busy, tmo;
    reg         miso_q;

    // STREAM: слово собирается в rxw/rx_wd, как ответ в RX, и уходит в поток вместо RX
    reg  [11:0] blocks;                 // блоков осталось
    reg  [15:0] crc;                    // CRC16 блока (по битам, вместе с CRC карты: верно — 0)
    reg         crc_bad;
    reg         rw_v;
    assign r_valid = ROUTE && rw_v;
    assign r_data  = {rx_wd[7:0], rx_wd[15:8], rx_wd[23:16], rx_wd[31:24]};   // первый байт — [7:0]

    wire        from_tx = op == OP_XFER;
    wire        strm    = ROUTE && op == OP_STRM;
    wire        store   = !strm && (op == OP_XFER || op == OP_FILL || in_blk);
    wire [7:0]  tx_byte = tx_q[31 - 8 * lane -: 8];

    assign ctrl_rd = {CLK_10K, 7'd0, cs_on, div};
    assign ptr_rd  = {5'd0, blk_len, 7'd0, ptr};
    assign status  = {16'h5D01, last, 5'd0, ROUTE && crc_bad, tmo, busy};

    always @(posedge clk) miso_q <= sd_miso;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            st <= S_IDLE; div <= DEF_DIV; div_cnt <= '0; cs_on <= 1'b0; ptr <= '0; blk_len <= 11'd514;
            op <= OP_XFER; left <= '0; in_blk <= 1'b0; lane <= '0; rxw <= '1; tsr <= 8'hFF;
            rsr <= 8'hFF; last <= 8'hFF; bitn <= '0; busy <= 1'b0; tmo <= 1'b0; wa <= '0;
            rx_we <= 1'b0; rx_wa <= '0; rx_wd <= '0;
            sd_cs_n <= 1'b1; sd_sck <= 1'b0; sd_mosi <= 1'b1;
            blocks <= '0; crc <= '0; crc_bad <= 1'b0; rw_v <= 1'b0;
        end else begin
            rx_we   <= 1'b0;
            sd_cs_n <= !cs_on;
            if (r_ready) rw_v <= 1'b0;              // слово потока взято (новое ставится ниже)
            if (ctrl_we) begin div <= wdata[7:0] == 8'd0 ? 8'd1 : wdata[7:0]; cs_on <= wdata[8]; end
            if (ptr_we && !busy) begin ptr <= wdata[8:0]; blk_len <= wdata[26:16]; end
            if (ROUTE && strm_we && !busy) blocks <= wdata[11:0];
            case (st)
                S_IDLE: if (cmd_we && !busy) begin
                    op      <= wdata[22:20];
                    left    <= wdata[19:0];
                    in_blk  <= 1'b0;
                    tmo     <= 1'b0;
                    crc_bad <= 1'b0;
                    lane    <= '0;
                    rxw     <= '1;
                    wa      <= ptr;
                    if (wdata[19:0] == 20'd0 || (wdata[22:20] == OP_STRM && (!ROUTE || blocks == 12'd0))) begin
                        tmo <= wdata[22:20] >= OP_WAIT;       // ждать нечего — не дождались
                    end else begin
                        busy <= 1'b1;
                        st   <= S_FETCH;
                    end
                end
                // слово TX по wa: адрес — на этом такте, данные — через такт
                S_FETCH:  st <= S_FETCH2;
                S_FETCH2: st <= S_PICK;
                S_PICK: if (!(ROUTE && rw_v)) begin  // поток не взял слово — SCK ждёт
                    tsr     <= from_tx ? tx_byte : 8'hFF;
                    sd_mosi <= from_tx ? tx_byte[7] : 1'b1;
                    bitn    <= '0;
                    div_cnt <= '0;
                    st      <= S_BIT;
                end
                S_BIT: begin
                    if (div_cnt != div) begin
                        div_cnt <= div_cnt + 8'd1;
                    end else begin
                        div_cnt <= '0;
                        if (!sd_sck) begin
                            sd_sck <= 1'b1;
                            rsr    <= {rsr[6:0], miso_q};
                            // CRC16-CCITT блока STREAM — по биту (данные и CRC карты)
                            if (strm && in_blk)
                                crc <= {crc[14:0], 1'b0} ^ ((crc[15] ^ miso_q) ? 16'h1021 : 16'h0000);
                        end else begin
                            sd_sck <= 1'b0;
                            if (bitn == 3'd7) begin
                                st <= S_BYTE;
                            end else begin
                                bitn    <= bitn + 3'd1;
                                sd_mosi <= tsr[6];
                                tsr     <= {tsr[6:0], 1'b1};
                            end
                        end
                    end
                end
                S_BYTE: begin
                    if (!in_blk) last <= rsr;          // у READ и STREAM остаётся маркер
                    if (strm && in_blk) begin
                        // байт блока: данные (все, кроме двух последних — CRC) — словом в поток
                        if (left > 20'd2) begin
                            rx_wd <= rxw;
                            rx_wd[31 - 8 * lane -: 8] <= rsr;
                            rxw[31 - 8 * lane -: 8] <= rsr;
                            lane <= lane + 2'd1;
                            if (lane == 2'd3) rw_v <= 1'b1;
                        end
                        if (left == 20'd1) begin
                            in_blk <= 1'b0;
                            if (crc != 16'd0) crc_bad <= 1'b1;
                            left   <= WAIT_NEXT;
                            blocks <= blocks - 12'd1;
                            if (blocks == 12'd1) begin
                                busy <= 1'b0;
                                st   <= S_IDLE;
                            end else
                                st   <= S_PICK;
                        end else begin
                            left <= left - 20'd1;
                            st   <= S_PICK;
                        end
                    end else if (store) begin
                        // байт ответа — в слово; слово — в RX, когда собрано или обмен кончился
                        rx_wd <= rxw;
                        rx_wd[31 - 8 * lane -: 8] <= rsr;
                        rxw[31 - 8 * lane -: 8] <= rsr;
                        rx_wa <= wa;
                        if (lane == 2'd3 || left == 20'd1) rx_we <= 1'b1;
                        if (left == 20'd1) begin
                            busy <= 1'b0;
                            st   <= S_IDLE;
                        end else begin
                            left <= left - 20'd1;
                            lane <= lane + 2'd1;
                            if (lane == 2'd3) begin
                                wa  <= wa + 9'd1;
                                rxw <= '1;
                                st  <= S_FETCH;
                            end else begin
                                st  <= S_PICK;
                            end
                        end
                    end else if (op == OP_BUSY ? rsr == 8'hFF : rsr != 8'hFF) begin
                        if ((op == OP_READ || strm) && rsr == 8'hFE && blk_len != 11'd0) begin
                            in_blk <= 1'b1;            // маркер блока: дальше — данные
                            left   <= {9'd0, blk_len};
                            lane   <= '0;
                            rxw    <= '1;
                            wa     <= ptr;
                            crc    <= '0;
                            st     <= S_FETCH;
                        end else begin
                            busy <= 1'b0;              // ответ, маркер ошибки или конец «занята»
                            st   <= S_IDLE;
                        end
                    end else if (left == 20'd1) begin
                        tmo  <= 1'b1;
                        busy <= 1'b0;
                        st   <= S_IDLE;
                    end else begin
                        left <= left - 20'd1;
                        st   <= S_PICK;
                    end
                end
                default: st <= S_IDLE;
            endcase
        end
    end
endmodule
