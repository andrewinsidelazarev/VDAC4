// SPDX-License-Identifier: Apache-2.0
module tmds_serializer_gowin (
    input  wire       pixel_clk,
    input  wire       serial_clk,
    input  wire       reset,
    input  wire [9:0] channel0,
    input  wire [9:0] channel1,
    input  wire [9:0] channel2,
    output wire       tmds_clk_p,
    output wire       tmds_clk_n,
    output wire [2:0] tmds_data_p,
    output wire [2:0] tmds_data_n
);
`ifndef SIMULATION
    wire [3:0] serial;
    OSER10 #(.GSREN("false"), .LSREN("true")) ser0 (
        .PCLK(pixel_clk), .FCLK(serial_clk), .RESET(reset), .Q(serial[0]),
        .D0(channel0[0]), .D1(channel0[1]), .D2(channel0[2]), .D3(channel0[3]),
        .D4(channel0[4]), .D5(channel0[5]), .D6(channel0[6]), .D7(channel0[7]),
        .D8(channel0[8]), .D9(channel0[9])
    );
    OSER10 #(.GSREN("false"), .LSREN("true")) ser1 (
        .PCLK(pixel_clk), .FCLK(serial_clk), .RESET(reset), .Q(serial[1]),
        .D0(channel1[0]), .D1(channel1[1]), .D2(channel1[2]), .D3(channel1[3]),
        .D4(channel1[4]), .D5(channel1[5]), .D6(channel1[6]), .D7(channel1[7]),
        .D8(channel1[8]), .D9(channel1[9])
    );
    OSER10 #(.GSREN("false"), .LSREN("true")) ser2 (
        .PCLK(pixel_clk), .FCLK(serial_clk), .RESET(reset), .Q(serial[2]),
        .D0(channel2[0]), .D1(channel2[1]), .D2(channel2[2]), .D3(channel2[3]),
        .D4(channel2[4]), .D5(channel2[5]), .D6(channel2[6]), .D7(channel2[7]),
        .D8(channel2[8]), .D9(channel2[9])
    );
    OSER10 #(.GSREN("false"), .LSREN("true")) serclk (
        .PCLK(pixel_clk), .FCLK(serial_clk), .RESET(reset), .Q(serial[3]),
        .D0(1'b1), .D1(1'b1), .D2(1'b1), .D3(1'b1), .D4(1'b1),
        .D5(1'b0), .D6(1'b0), .D7(1'b0), .D8(1'b0), .D9(1'b0)
    );
    ELVDS_OBUF ob0(.I(serial[0]), .O(tmds_data_p[0]), .OB(tmds_data_n[0]));
    ELVDS_OBUF ob1(.I(serial[1]), .O(tmds_data_p[1]), .OB(tmds_data_n[1]));
    ELVDS_OBUF ob2(.I(serial[2]), .O(tmds_data_p[2]), .OB(tmds_data_n[2]));
    ELVDS_OBUF obc(.I(serial[3]), .O(tmds_clk_p), .OB(tmds_clk_n));
`else
    reg [9:0] sh0, sh1, sh2, shc;
    reg [3:0] count;
    always @(posedge serial_clk) begin
        if (reset) begin
            sh0 <= 0; sh1 <= 0; sh2 <= 0; shc <= 10'b0000011111; count <= 0;
        end else if (count == 0) begin
            sh0 <= channel0; sh1 <= channel1; sh2 <= channel2;
            shc <= 10'b0000011111; count <= 9;
        end else begin
            sh0 <= {1'b0, sh0[9:1]}; sh1 <= {1'b0, sh1[9:1]};
            sh2 <= {1'b0, sh2[9:1]}; shc <= {1'b0, shc[9:1]};
            count <= count - 1'b1;
        end
    end
    assign tmds_data_p = {sh2[0], sh1[0], sh0[0]};
    assign tmds_data_n = ~tmds_data_p;
    assign tmds_clk_p = shc[0];
    assign tmds_clk_n = ~shc[0];
`endif
endmodule
