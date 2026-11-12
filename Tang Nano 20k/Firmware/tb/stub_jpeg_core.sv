// SPDX-License-Identifier: Apache-2.0
//
// Заглушка декодера JPEG (third_party/core_jpeg) для стендов Icarus, которым декодер не нужен
// (tb_video_link: пути данных в медиа-FIFO). Icarus 13 не разбирает jpeg_dht.v (объявление после
// использования); декодирование проверяют стенды Verilator (tb_vpc, vvid). Берёт каждое слово,
// точек не выдаёт, всегда свободен.
module jpeg_core #(
    parameter SUPPORT_WRITABLE_DHT = 0
) (
    input  wire        clk_i,
    input  wire        rst_i,
    input  wire        inport_valid_i,
    input  wire [31:0] inport_data_i,
    input  wire [3:0]  inport_strb_i,
    input  wire        inport_last_i,
    output wire        inport_accept_o,
    input  wire        outport_accept_i,
    output wire        outport_valid_o,
    output wire [15:0] outport_width_o,
    output wire [15:0] outport_height_o,
    output wire [15:0] outport_pixel_x_o,
    output wire [15:0] outport_pixel_y_o,
    output wire [7:0]  outport_pixel_r_o,
    output wire [7:0]  outport_pixel_g_o,
    output wire [7:0]  outport_pixel_b_o,
    output wire        idle_o,
    output wire [1:0]  outport_mode_o,
    output wire [7:0]  outport_qy_o
);
    assign inport_accept_o   = 1'b1;
    assign outport_valid_o   = 1'b0;
    assign outport_width_o   = 16'd0;
    assign outport_height_o  = 16'd0;
    assign outport_pixel_x_o = 16'd0;
    assign outport_pixel_y_o = 16'd0;
    assign outport_pixel_r_o = 8'd0;
    assign outport_pixel_g_o = 8'd0;
    assign outport_pixel_b_o = 8'd0;
    assign idle_o            = 1'b1;
    assign outport_mode_o    = 2'd0;
    assign outport_qy_o      = 8'd0;
endmodule
