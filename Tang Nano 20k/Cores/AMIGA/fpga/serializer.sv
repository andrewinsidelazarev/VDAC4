// SPDX-License-Identifier: GPL-3.0-or-later
// Gowin-only TMDS output. Video frame resynchronization must not reset OSER10:
// it is a continuously running 10:1 serializer, with new symbols every PCLK.
// Configuration GSR initializes the primitives. Local RESET is disabled; this
// avoids a frame-domain reset crossing to both 142.5 MHz DDR clock edges.
module serializer #(parameter integer NUM_CHANNELS=3, VIDEO_RATE=32000000)(
    input wire clk_pixel, clk_pixel_x5, reset,
    input wire [9:0] tmds_internal[NUM_CHANNELS-1:0],
    output wire [2:0] tmds, output wire tmds_clock
);
    genvar i;
    generate for (i=0;i<3;i=i+1) begin: channel
        OSER10 #(.GSREN("true"),.LSREN("false")) ser(
            .Q(tmds[i]),.PCLK(clk_pixel),.FCLK(clk_pixel_x5),.RESET(1'b0),
            .D0(tmds_internal[i][0]),.D1(tmds_internal[i][1]),
            .D2(tmds_internal[i][2]),.D3(tmds_internal[i][3]),
            .D4(tmds_internal[i][4]),.D5(tmds_internal[i][5]),
            .D6(tmds_internal[i][6]),.D7(tmds_internal[i][7]),
            .D8(tmds_internal[i][8]),.D9(tmds_internal[i][9]));
    end endgenerate
    // 8 ноября 2026: такт TMDS — через свой OSER10 (1111100000 за точку), тем же путём HCLK, что и
    // данные, как в TS-Config/VIDEO/FT812 (Firmware/rtl/tmds_serializer_gowin.sv), которые на 720p
    // работают без срывов. Прежде такт шёл на вывод прямо с глобальной сети clk_pixel: она проходит
    // через всю ПЛИС (Amiga — 97 % логики, постоянный трафик SDRAM), а приёмник берёт такт за опору.
    OSER10 #(.GSREN("true"),.LSREN("false")) clock_ser(
        .Q(tmds_clock),.PCLK(clk_pixel),.FCLK(clk_pixel_x5),.RESET(1'b0),
        .D0(1'b1),.D1(1'b1),.D2(1'b1),.D3(1'b1),.D4(1'b1),
        .D5(1'b0),.D6(1'b0),.D7(1'b0),.D8(1'b0),.D9(1'b0));
endmodule
