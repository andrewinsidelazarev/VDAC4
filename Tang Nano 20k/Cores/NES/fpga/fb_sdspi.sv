// SPDX-License-Identifier: Apache-2.0
//
// Мост к microSD самой Tang Nano 20K для ESP (ядро TS-Config, слот 0; docs/next-core.md): карта
// в режиме SPI на выводах слота CS 81, MOSI 82, SCK 83, MISO 84. DAT1 и DAT2 слота (85, 80) на
// VDAC4 — LINK IRQ и PCLK Evo; в режиме SPI карта их не ведёт. Протокол SD (CMD0, CMD8, ACMD41,
// CMD17, CMD24 …) и FatFs — на ESP. Мост делает побайтный обмен, ожидание ответа карты и приём
// блока после маркера: блок 512 байтов ESP читает за ~18 обменов LINK (запись — за ~22), а не
// побайтно (тест прошивки ESP tests/test_sd_bridge_host.py).
//
// БУФЕРЫ — по 2 КБ (512 слов; байт 0 — старший байт слова, как у моста к flash):
//   TX пишет ESP (окно LINK), читает мост; RX пишет мост, читает ESP (окно LINK, домен SCK LINK).
// РЕГИСТРЫ:
//   CTRL  [7:0] div: полупериод SCK — div + 1 тактов clk (SCK = clk / (2·(div + 1))); div 0 — как
//         1 (SCK не быстрее clk / 4): MISO берётся из регистра входа, а карта меняет его по спаду
//         SCK с задержкой до 14 нс;
//         8 — CS активен (CS = 0). Чтение — то же и [31:16] такт моста, десятки кГц.
//         После сброса — не быстрее 400 кГц, CS неактивен.
//   PTR   [8:0] слово буферов, с которого идёт обмен (TX и RX — с одного места);
//         [26:16] байтов блока для READ (после маркера 0xFE: 512 данных и 2 CRC — 514).
//   CMD   (запись запускает операцию; пока занят — не принимается) [19:0] N, [22:20] операция:
//         0 XFER — N байтов из TX, ответ карты — в RX;
//         1 FILL — N байтов 0xFF, ответ — в RX;
//         2 WAIT — 0xFF, пока карта отвечает 0xFF, не больше N байтов (R1, маркер данных);
//         3 READ — как WAIT; пришёл маркер 0xFE — ещё PTR[26:16] байтов 0xFF, ответ — в RX;
//         4 BUSY — 0xFF, пока карта не ответит 0xFF (держит 0, пока пишет), не больше N байтов.
//   STAT  0 — занят; 1 — WAIT, READ, BUSY: за N байтов не дождались; [15:8] последний принятый
//         байт (у WAIT и READ — тот, что не 0xFF: R1, маркер или ошибка данных); [31:16] 0x5D01.
// Ответ в RX ложится словами: слово пишется, когда собраны его 4 байта или кончился обмен; байты
// слова за последним — 0xFF.
//
// SPI режим 0: MOSI меняется при SCK = 0, MISO берётся по фронту SCK (из регистра входа — значение
// за такт до фронта: карта выставила его по спаду, полупериод назад).
//
module fb_sdspi #(
    parameter int CLK_KHZ = 74_250
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
    output reg  [31:0] rd_q
);
    localparam logic [7:0]  DEF_DIV = (CLK_KHZ + 799) / 800 - 1;   // ≤ 400 кГц
    localparam logic [15:0] CLK_10K = CLK_KHZ / 10;
    localparam logic [2:0]  OP_XFER = 3'd0, OP_FILL = 3'd1, OP_WAIT = 3'd2, OP_READ = 3'd3,
                            OP_BUSY = 3'd4;

    // ------------------------------------------------------------------
    // Буферы (BSRAM): TX — запись окна LINK, чтение мостом; RX — запись мостом, чтение окна LINK
    // ------------------------------------------------------------------
    reg  [31:0] txm [0:511] /* synthesis syn_ramstyle = "block_ram" */;
    reg  [31:0] rxm [0:511] /* synthesis syn_ramstyle = "block_ram" */;
    reg         tx_we_q;
    reg  [8:0]  tx_wa_q;
    reg  [31:0] tx_wd_q;
    reg  [8:0]  wa;                     // слово буферов текущего байта
    reg  [31:0] tx_q;
    reg         rx_we;
    reg  [8:0]  rx_wa;
    reg  [31:0] rx_wd;

    always @(posedge clk) begin
        tx_we_q <= tx_we;
        tx_wa_q <= tx_waddr;
        tx_wd_q <= tx_wdata;
        if (tx_we_q) txm[tx_wa_q] <= tx_wd_q;
        tx_q <= txm[wa];
        if (rx_we) rxm[rx_wa] <= rx_wd;
    end
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
    reg  [19:0] left;                   // байтов осталось (у READ после маркера — блока)
    reg         in_blk;                 // READ: идёт приём блока
    reg  [1:0]  lane;
    reg  [31:0] rxw;                    // собираемое слово ответа
    reg  [7:0]  tsr, rsr, last;
    reg  [2:0]  bitn;
    reg         busy, tmo;
    reg         miso_q;

    wire        from_tx = op == OP_XFER;
    wire        store   = op == OP_XFER || op == OP_FILL || in_blk;
    wire [7:0]  tx_byte = tx_q[31 - 8 * lane -: 8];

    assign ctrl_rd = {CLK_10K, 7'd0, cs_on, div};
    assign ptr_rd  = {5'd0, blk_len, 7'd0, ptr};
    assign status  = {16'h5D01, last, 6'd0, tmo, busy};

    always @(posedge clk) miso_q <= sd_miso;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            st <= S_IDLE; div <= DEF_DIV; div_cnt <= '0; cs_on <= 1'b0; ptr <= '0; blk_len <= 11'd514;
            op <= OP_XFER; left <= '0; in_blk <= 1'b0; lane <= '0; rxw <= '1; tsr <= 8'hFF;
            rsr <= 8'hFF; last <= 8'hFF; bitn <= '0; busy <= 1'b0; tmo <= 1'b0; wa <= '0;
            rx_we <= 1'b0; rx_wa <= '0; rx_wd <= '0;
            sd_cs_n <= 1'b1; sd_sck <= 1'b0; sd_mosi <= 1'b1;
        end else begin
            rx_we   <= 1'b0;
            sd_cs_n <= !cs_on;
            if (ctrl_we) begin div <= wdata[7:0] == 8'd0 ? 8'd1 : wdata[7:0]; cs_on <= wdata[8]; end
            if (ptr_we && !busy) begin ptr <= wdata[8:0]; blk_len <= wdata[26:16]; end
            case (st)
                S_IDLE: if (cmd_we && !busy) begin
                    op     <= wdata[22:20];
                    left   <= wdata[19:0];
                    in_blk <= 1'b0;
                    tmo    <= 1'b0;
                    lane   <= '0;
                    rxw    <= '1;
                    wa     <= ptr;
                    if (wdata[19:0] == 20'd0) begin
                        tmo <= wdata[22:20] >= OP_WAIT;       // ждать нечего — не дождались
                    end else begin
                        busy <= 1'b1;
                        st   <= S_FETCH;
                    end
                end
                // слово TX по wa: адрес — на этом такте, данные — через такт
                S_FETCH:  st <= S_FETCH2;
                S_FETCH2: st <= S_PICK;
                S_PICK: begin
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
                    if (!in_blk) last <= rsr;          // у READ остаётся маркер
                    if (store) begin
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
                        if (op == OP_READ && rsr == 8'hFE && blk_len != 11'd0) begin
                            in_blk <= 1'b1;            // маркер блока: дальше — данные в RX
                            left   <= {9'd0, blk_len};
                            lane   <= '0;
                            rxw    <= '1;
                            wa     <= ptr;
                            st     <= S_FETCH;
                        end else begin
                            busy <= 1'b0;
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
