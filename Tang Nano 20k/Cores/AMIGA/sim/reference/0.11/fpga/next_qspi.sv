// SPDX-License-Identifier: Apache-2.0
//
// LINK ядра ZX Next на VDAC4: ведомый QSPI для ESP32-S3 — копия fb_link ядер VDAC4
// (fpga/vdac2/rtl/fb_link.sv, протокол тот же), но чтение — до слова 0x1FFF (адреса до 0x7FFC):
// регистры 0…255 и окна колец звука и UART (next_link.sv). ESP — ведущий, режим SPI 0: линии
// читаются по фронту LINK_SCK, ответ Tang меняется по спаду.
//
// ТРАНЗАКЦИЯ (CS = 0):
//   команда 8 бит по IO0, старшим вперёд;
//   адрес 32 бита по IO0…IO3 (8 тактов, тетрада старшими вперёд, IO3 — старший бит);
//   для чтения — DUMMY тактов разворота шины;
//   данные по IO0…IO3 словами по 32 бита (старший байт вперёд), адрес слова +4 за слово.
// Команды: 0x01 — запись регистров, 0x02 — чтение регистров. Остальные игнорируются.
// Адрес — байтовый, младшие два бита не учитываются; регистров 256 (адреса 0…0x3FC).
// Запись — по номеру слова до 18 бит (адреса до 0xFFFFF); чтение — номера слов до 0x1FFF:
// слово по rd_addr даёт next_link (регистры — как есть, окна — из BSRAM по фронту SCK: номер
// меняется за 8 тактов SCK до выдачи, первое слово — за DUMMY тактов).
//
// ДОМЕНЫ. Сдвиговые регистры тактируются самим LINK_SCK (вывод GCLKT_1) и
// сбрасываются по CS. Записанное слово переходит в домен пикселей переключателем:
// слово и его адрес держатся до конца следующего слова (8 тактов SCK), домену пикселей
// на приём нужно 4 такта 27 МГц — при SCK до 40 МГц хватает.
// Чтение берёт значения домена пикселей как есть: номер следующего слова выставляется
// за 8 тактов SCK до выдачи, первое слово — за DUMMY тактов. Значения меняются редко;
// счётчик, пойманный посреди смены, ESP прочитает повторно.
//
module next_qspi #(
    parameter int DUMMY = 8
) (
    // LINK (домен LINK_SCK)
    input  wire        sck,
    input  wire        cs_n,
    input  wire [3:0]  io_in,
    output reg  [3:0]  io_out,
    output reg         io_oe,

    // Регистры (домен пикселей)
    input  wire        clk,
    input  wire        rst_n,
    output reg         wr_stb,         // такт: записано слово wr_data по адресу wr_addr
    output reg  [17:0] wr_addr,        // номер 32-битного слова (регистр — 0…255)
    output reg  [31:0] wr_data,
    output wire [12:0] rd_addr,        // какое слово выдать следующим (домен SCK): 0…255 — регистры, дальше окна
    input  wire [31:0] rd_data         // его значение (комбинационно от rd_addr)
);
    localparam [7:0] CMD_WRITE = 8'h01, CMD_READ = 8'h02;
    localparam [7:0] ADDR_END  = 8'd16;                 // такт, с которого идут данные
    localparam [7:0] RD_START  = ADDR_END + DUMMY[7:0];

    // ------------------------------------------------------------------
    // Приём по фронту SCK
    // ------------------------------------------------------------------
    // Начальные значения — как у триггеров ПЛИС после загрузки (CS при этом высокий).
    reg [7:0]  cyc     = '0; // принятых фронтов с начала транзакции (насыщается)
    reg [7:0]  cmd     = '0;
    reg [31:0] addr_sr = '0;
    reg [2:0]  wnib    = '0; // тетрад текущего слова записи принято

    always @(posedge sck or posedge cs_n) begin
        if (cs_n) begin
            cyc <= '0; cmd <= '0; addr_sr <= '0; wnib <= '0;
        end else begin
            if (cyc != 8'hFF) cyc <= cyc + 8'd1;
            if (cyc < 8'd8) cmd <= {cmd[6:0], io_in[0]};
            else if (cyc < ADDR_END) addr_sr <= {addr_sr[27:0], io_in};
            else if (cmd == CMD_WRITE) wnib <= wnib + 3'd1;
        end
    end

    // Слово записи. Готовое слово и его адрес держатся до конца следующего слова;
    // CS их не сбрасывает — домен пикселей может забрать последнее слово уже после CS.
    reg [31:0] wsr      = '0;
    reg [17:0] waddr    = '0;    // номер слова текущей записи
    reg        w_tog    = 1'b0;
    reg [17:0] w_addr_q = '0;
    reg [31:0] w_data_q = '0;
    always @(posedge sck) begin
        if (!cs_n && cmd == CMD_WRITE && cyc >= ADDR_END) begin
            wsr <= {wsr[27:0], io_in};
            if (cyc == ADDR_END) waddr <= addr_sr[19:2];
            if (wnib == 3'd7) begin
                w_data_q <= {wsr[27:0], io_in};
                w_addr_q <= waddr;
                w_tog    <= ~w_tog;
                waddr    <= waddr + 18'd1;
            end
        end
    end

    // ------------------------------------------------------------------
    // Выдача по спаду SCK
    // ------------------------------------------------------------------
    reg [12:0] raddr = '0;   // номер слова, выдаваемого следующим (адрес / 4, до 0x7FFC)
    reg [31:0] rsr   = '0;
    reg [2:0]  rnib  = '0;
    initial begin io_oe = 1'b0; io_out = '0; end
    assign rd_addr = raddr;

    always @(negedge sck or posedge cs_n) begin
        if (cs_n) begin
            io_oe <= 1'b0; io_out <= '0; raddr <= '0; rsr <= '0; rnib <= '0;
        end else if (cmd == CMD_READ) begin
            if (cyc == ADDR_END) raddr <= addr_sr[14:2];
            if (cyc >= RD_START) begin
                io_oe <= 1'b1;
                if (rnib == 3'd0) begin
                    // Новое слово: значение выставлено ещё при смене raddr
                    io_out <= rd_data[31:28];
                    rsr    <= {rd_data[27:0], 4'h0};
                    raddr  <= raddr + 13'd1;
                end else begin
                    io_out <= rsr[31:28];
                    rsr    <= {rsr[27:0], 4'h0};
                end
                rnib <= rnib + 3'd1;
            end
        end
    end

    // ------------------------------------------------------------------
    // Слово записи → домен пикселей
    // ------------------------------------------------------------------
    reg [2:0] tog_s /* synthesis syn_srlstyle = "registers" */;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            tog_s <= '0; wr_stb <= 1'b0; wr_addr <= '0; wr_data <= '0;
        end else begin
            tog_s  <= {tog_s[1:0], w_tog};
            wr_stb <= 1'b0;
            if (tog_s[2] ^ tog_s[1]) begin
                wr_stb  <= 1'b1;
                wr_addr <= w_addr_q;
                wr_data <= w_data_q;
            end
        end
    end
endmodule
