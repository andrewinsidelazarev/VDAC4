// SPDX-License-Identifier: Apache-2.0
//
// Четырёхстрочный буфер вывода: запись словами по 32 бита (две точки) в домене SDRAM,
// чтение в пиксельном домене с одним тактом задержки — сразу четырёх точек 2×2 для
// сглаживания (fb_scanout).
//
// Четыре блока по 1024 × 16 бит (по блоку BSRAM): A — строки с чётным номером, B — с
// нечётным (в каждом две строки: адрес {строка[1], слово[8:0]}); e — чётные точки
// (младшая половина слова), o — нечётные. Соседние по горизонтали и вертикали точки лежат
// в разных блоках и читаются одновременно.
//
module fb_line_ram (
    input  wire        wr_clk,
    input  wire        we,
    input  wire [10:0] waddr,          // {строка[1:0], слово[8:0]}
    input  wire [31:0] wdata,          // [15:0] — чётная точка, [31:16] — нечётная

    input  wire        rd_clk,
    input  wire [9:0]  ra_e,
    input  wire [9:0]  ra_o,
    input  wire [9:0]  rb_e,
    input  wire [9:0]  rb_o,
    output reg  [15:0] qa_e,
    output reg  [15:0] qa_o,
    output reg  [15:0] qb_e,
    output reg  [15:0] qb_o
);
    (* ram_style = "block" *) reg [15:0] mem_ae [0:1023];
    (* ram_style = "block" *) reg [15:0] mem_ao [0:1023];
    (* ram_style = "block" *) reg [15:0] mem_be [0:1023];
    (* ram_style = "block" *) reg [15:0] mem_bo [0:1023];

    wire [9:0] wa = {waddr[10], waddr[8:0]};

    always_ff @(posedge wr_clk)
        if (we && !waddr[9]) begin mem_ae[wa] <= wdata[15:0]; mem_ao[wa] <= wdata[31:16]; end

    always_ff @(posedge wr_clk)
        if (we && waddr[9]) begin mem_be[wa] <= wdata[15:0]; mem_bo[wa] <= wdata[31:16]; end

    always_ff @(posedge rd_clk) begin
        qa_e <= mem_ae[ra_e];
        qa_o <= mem_ao[ra_o];
        qb_e <= mem_be[rb_e];
        qb_o <= mem_bo[rb_o];
    end
endmodule
