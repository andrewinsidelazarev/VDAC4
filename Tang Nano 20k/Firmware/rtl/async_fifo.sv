// SPDX-License-Identifier: Apache-2.0
// Small dual-clock FIFO used at the TS-Config SPI clock boundary.
module async_fifo #(
    parameter integer WIDTH = 32,
    parameter integer ADDR_BITS = 4
) (
    input  wire                 wr_clk,
    input  wire                 wr_reset_n,
    input  wire                 wr_en,
    input  wire [WIDTH-1:0]     wr_data,
    output wire                 wr_full,

    input  wire                 rd_clk,
    input  wire                 rd_reset_n,
    input  wire                 rd_en,
    output reg  [WIDTH-1:0]     rd_data,
    output wire                 rd_empty
);
    localparam integer DEPTH = 1 << ADDR_BITS;

    reg [WIDTH-1:0] mem [0:DEPTH-1];
    reg [ADDR_BITS:0] wr_bin;
    reg [ADDR_BITS:0] wr_gray;
    reg [ADDR_BITS:0] rd_bin;
    reg [ADDR_BITS:0] rd_gray;
    reg [ADDR_BITS:0] rd_gray_wr1 /* synthesis syn_srlstyle = "registers" */;
    reg [ADDR_BITS:0] rd_gray_wr2 /* synthesis syn_srlstyle = "registers" */;
    reg [ADDR_BITS:0] wr_gray_rd1 /* synthesis syn_srlstyle = "registers" */;
    reg [ADDR_BITS:0] wr_gray_rd2 /* synthesis syn_srlstyle = "registers" */;

    // Признак «полон» — регистр (схема Каммингса): он зависит от следующего указателя
    // записи, а тот — от самого признака. Комбинационно это петля, которую синтез
    // Gowin справедливо отмечает (AG0100).
    reg wr_full_r;
    wire [ADDR_BITS:0] wr_bin_next = wr_bin + ((wr_en && !wr_full_r) ? 1'b1 : 1'b0);
    wire [ADDR_BITS:0] rd_bin_next = rd_bin + ((rd_en && !rd_empty) ? 1'b1 : 1'b0);
    wire [ADDR_BITS:0] wr_gray_next = (wr_bin_next >> 1) ^ wr_bin_next;
    wire [ADDR_BITS:0] rd_gray_next = (rd_bin_next >> 1) ^ rd_bin_next;

    assign rd_empty = (rd_gray == wr_gray_rd2);
    assign wr_full  = wr_full_r;

    always @(posedge wr_clk or negedge wr_reset_n) begin
        if (!wr_reset_n) begin
            wr_bin <= 0;
            wr_gray <= 0;
            rd_gray_wr1 <= 0;
            rd_gray_wr2 <= 0;
            wr_full_r <= 1'b0;
        end else begin
            rd_gray_wr1 <= rd_gray;
            rd_gray_wr2 <= rd_gray_wr1;
            wr_full_r <= (wr_gray_next ==
                {~rd_gray_wr2[ADDR_BITS:ADDR_BITS-1], rd_gray_wr2[ADDR_BITS-2:0]});
            if (wr_en && !wr_full_r) begin
                mem[wr_bin[ADDR_BITS-1:0]] <= wr_data;
                wr_bin <= wr_bin_next;
                wr_gray <= wr_gray_next;
            end
        end
    end

    always @(posedge rd_clk or negedge rd_reset_n) begin
        if (!rd_reset_n) begin
            rd_bin <= 0;
            rd_gray <= 0;
            wr_gray_rd1 <= 0;
            wr_gray_rd2 <= 0;
            rd_data <= 0;
        end else begin
            wr_gray_rd1 <= wr_gray;
            wr_gray_rd2 <= wr_gray_rd1;
            if (rd_en && !rd_empty) begin
                rd_data <= mem[rd_bin[ADDR_BITS-1:0]];
                rd_bin <= rd_bin_next;
                rd_gray <= rd_gray_next;
            end
        end
    end
endmodule
