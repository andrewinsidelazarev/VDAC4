// SPDX-License-Identifier: Apache-2.0
//
// Связь с BL616 (сопроцессор FT812, bl616/copro): приёмник SPI, BL616 — ведущий. Режим 0,
// старший бит первым; слова данных — младшим байтом вперёд, как в памяти FT812. Приёмник
// тактируется самим SCK (до 30 МГц, у BL616 — 26,67) и сбрасывается снятием CS.
//
// Посылки (первый байт — код; MiSTle Companion, что стоит в BL616 с завода, начинает
// посылки байтами 0…5 — их приёмник пропускает):
//   C1 a2 a1 a0 w0 w1 …   WRITE: слова подряд с адреса FT812 a (кратен 4);
//   C2 a2 a1 a0 n         READ_REQ: n + 1 слов с адреса a — в буфер чтения (cp_link);
//   C3 xx s0 s1 … s15     STATUS: снимок состояния (cp_link) с третьего байта;
//   C4 xx w0 w1 …         READ_DATA: слова буфера чтения с начала.
// Байт xx — время ядру снять снимок или выставить первое слово.
//
// События для cp_link — переключением tg; поля держатся до следующего события, а оно
// не раньше чем через 8 тактов SCK (EV_SNAP → EV_ADDR) или 32 (слова):
//   EV_SNAP — принят код STATUS; EV_ADDR — адрес WRITE; EV_WORD — слово WRITE;
//   EV_RREQ — {n, адрес} READ_REQ.
// MISO выставляется по фронту SCK на бит вперёд (как в ft_spi): ведущий берёт бит следующим
// фронтом — на это у него целый период SCK, а не половина; удержание — задержка выхода ПЛИС.
// Вне STATUS и READ_DATA — 0.
module cp_spi (
    input  wire         sck,
    input  wire         cs_n,
    input  wire         mosi,
    output wire         miso,

    output reg          tg,
    output reg  [1:0]   ev_kind,
    output reg  [31:0]  ev_data,

    input  wire [127:0] st_vec,      // снимок cp_link: байт k — st_vec[8k +: 8]
    output reg  [8:0]   rb_addr,     // буфер чтения: адрес — по SCK, rb_q — тактом позже
    input  wire [31:0]  rb_q
);
    localparam logic [1:0] EV_SNAP = 2'd0, EV_ADDR = 2'd1, EV_WORD = 2'd2, EV_RREQ = 2'd3;
    localparam logic [7:0] OP_WRITE = 8'hC1, OP_RREQ = 8'hC2, OP_STATUS = 8'hC3, OP_RDATA = 8'hC4;

    reg  [2:0]  bitn;
    reg  [4:0]  nbyte;               // номер байта посылки (стоит на 31)
    reg  [1:0]  lane;                // байт в слове данных
    reg  [6:0]  sh;
    reg  [7:0]  op;
    reg  [23:0] a;
    reg  [23:0] w;                   // собираемое слово WRITE
    reg  [31:0] txw;                 // слово READ_DATA, которое уходит
    reg         miso_r;

    wire [7:0] rx = {sh, mosi};
    wire       last = bitn == 3'd7;

    // счёт битов и байтов, слово READ_DATA
    always @(posedge sck or posedge cs_n) begin
        if (cs_n) begin
            bitn    <= 3'd0;
            nbyte   <= 5'd0;
            lane    <= 2'd0;
            rb_addr <= 9'd0;
        end else begin
            bitn <= bitn + 3'd1;
            if (last) begin
                if (nbyte != 5'd31)
                    nbyte <= nbyte + 5'd1;
                if ((op == OP_WRITE && nbyte >= 5'd4) || (op == OP_RDATA && nbyte >= 5'd2))
                    lane <= lane + 2'd1;
                // READ_DATA: слово — в txw в конце байта 1 и в конце каждого слова
                if (op == OP_RDATA && (nbyte == 5'd1 || (nbyte >= 5'd2 && lane == 2'd3)))
                    rb_addr <= rb_addr + 9'd1;
            end
        end
    end

    // Без сброса: tg обязан сохранять значение между посылками.
    always @(posedge sck) begin
        sh <= rx[6:0];
        if (!cs_n && last) begin
            if (nbyte == 5'd0)
                op <= rx;
            if (nbyte >= 5'd1 && nbyte <= 5'd3)
                a <= {a[15:0], rx};
            if (op == OP_RDATA && (nbyte == 5'd1 || (nbyte >= 5'd2 && lane == 2'd3)))
                txw <= rb_q;
            if (nbyte == 5'd0 && rx == OP_STATUS) begin
                ev_kind <= EV_SNAP;
                tg      <= ~tg;
            end
            if (op == OP_WRITE && nbyte == 5'd3) begin
                ev_kind <= EV_ADDR;
                ev_data <= {8'd0, a[15:0], rx};
                tg      <= ~tg;
            end
            if (op == OP_WRITE && nbyte >= 5'd4) begin
                if (lane == 2'd3) begin
                    ev_kind <= EV_WORD;
                    ev_data <= {rx, w};
                    tg      <= ~tg;
                end else begin
                    w[8 * lane +: 8] <= rx;
                end
            end
            if (op == OP_RREQ && nbyte == 5'd4) begin
                ev_kind <= EV_RREQ;
                ev_data <= {rx, a};
                tg      <= ~tg;
            end
        end
    end

    // MISO — бит, который ведущий возьмёт следующим фронтом: счётчики после этого фронта
    wire [2:0] bt_n   = bitn + 3'd1;
    wire [4:0] nb_n   = last && nbyte != 5'd31 ? nbyte + 5'd1 : nbyte;
    wire       d_byte = (op == OP_WRITE && nbyte >= 5'd4) || (op == OP_RDATA && nbyte >= 5'd2);
    wire [1:0] lane_n = last && d_byte ? lane + 2'd1 : lane;
    wire       w_load = last && op == OP_RDATA && (nbyte == 5'd1 || (nbyte >= 5'd2 && lane == 2'd3));
    wire [31:0] w_n   = w_load ? rb_q : txw;
    wire [4:0] sidx   = nb_n - 5'd2;
    always @(posedge sck or posedge cs_n) begin
        if (cs_n)
            miso_r <= 1'b0;
        else if (op == OP_STATUS && nb_n >= 5'd2 && nb_n < 5'd18)
            miso_r <= st_vec[{sidx[3:0], ~bt_n}];
        else if (op == OP_RDATA && nb_n >= 5'd2)
            miso_r <= w_n[{lane_n, ~bt_n}];
        else
            miso_r <= 1'b0;
    end

    // как у триггеров ПЛИС после загрузки (CS при этом высокий)
    initial begin
        tg = 1'b0; bitn = 3'd0; nbyte = 5'd0; lane = 2'd0; miso_r = 1'b0; op = 8'd0;
        rb_addr = 9'd0;
    end

    assign miso = miso_r;
endmodule
