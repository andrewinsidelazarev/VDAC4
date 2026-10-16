// SPDX-License-Identifier: Apache-2.0
//
// Строчный буфер между доменом приёма (56 МГц) и доменом вывода HDMI (28 МГц).
//
// Зачем он нужен. Выход генлочен со входом: обе частоты получены из одного PCLK,
// поэтому кадры не расходятся и полный кадровый буфер в SDRAM не требуется.
// Но фаза между доменами произвольна, а активное окно на выходе сдвинуто
// относительно входного HSYNC. Достаточно хранить две строки и читать ту,
// которая уже дописана: пока пишется строка N, выводится строка N-1.
//
// Размер: 2 строки × 1024 отсчёта × 16 бит = 32 Кбит, одна-две ячейки BSRAM.
// 1024 выбрано с запасом: во входной строке 896 отсчётов PCLK (28 МГц / 31,25 кГц).
//
// Хранится ровно то, что пришло с порта: {PAL_SEL, B[4:0], G[4:0], R[4:0]}.
// Расширение до 8 бит на компоненту делается уже на выходе (legacy_palette),
// иначе буфер вырос бы втрое без выигрыша в качестве.
//
module ts_line_buffer #(
    parameter int ADDR_BITS = 10   // 1024 отсчёта на строку
) (
    // Запись: домен приёма
    input  wire                 wr_clk,
    input  wire                 wr_en,
    input  wire                 wr_line,           // в какую из двух строк пишем
    input  wire [ADDR_BITS-1:0] wr_addr,
    input  wire [15:0]          wr_data,

    // Чтение: домен пикселя HDMI
    input  wire                 rd_clk,
    input  wire                 rd_line,
    input  wire [ADDR_BITS-1:0] rd_addr,
    output reg  [15:0]          rd_data
);

    // Двухпортовая память: один порт на запись, другой на чтение.
    // Синтезатор Gowin выводит из этого описания BSRAM в режиме SDPB.
    (* ram_style = "block" *)
    reg [15:0] mem [0:(2 << ADDR_BITS) - 1];

    always_ff @(posedge wr_clk) begin
        if (wr_en)
            mem[{wr_line, wr_addr}] <= wr_data;
    end

    always_ff @(posedge rd_clk) begin
        rd_data <= mem[{rd_line, rd_addr}];
    end

endmodule
