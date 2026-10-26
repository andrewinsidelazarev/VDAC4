// SPDX-License-Identifier: Apache-2.0
//
// Память FT812 в BSRAM: 32 бита, запись по байтам (четыре полосы по 8 бит), отдельные
// адреса записи и чтения, чтение с задержкой в такт.
module ft_ram #(
    parameter int AW = 11
) (
    input  wire          clk,
    input  wire          we,
    input  wire [3:0]    be,
    input  wire [AW-1:0] waddr,
    input  wire [31:0]   wdata,
    input  wire [AW-1:0] raddr,
    output reg  [31:0]   rdata
);
    reg [7:0] m0 [0:(1 << AW) - 1];
    reg [7:0] m1 [0:(1 << AW) - 1];
    reg [7:0] m2 [0:(1 << AW) - 1];
    reg [7:0] m3 [0:(1 << AW) - 1];

    always @(posedge clk) begin
        if (we && be[0]) m0[waddr] <= wdata[7:0];
        if (we && be[1]) m1[waddr] <= wdata[15:8];
        if (we && be[2]) m2[waddr] <= wdata[23:16];
        if (we && be[3]) m3[waddr] <= wdata[31:24];
        rdata <= {m3[raddr], m2[raddr], m1[raddr], m0[raddr]};
    end
endmodule
