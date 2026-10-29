// SPDX-License-Identifier: Apache-2.0
//
// Стенд Verilator: декодер JPEG (jpeg_core) → запись кадра (vp_fbwrite) → клиент SDRAM наружу
// (модель контроллера и памяти — в tb_vp.cpp). Начало кадра даёт стенд (frame_start перед SOI),
// конец — idle декодера.
module vp_tb_top #(
    parameter int DEBLOCK = 2,
    parameter int DITHER  = 1,
    parameter int DB_HALF = 1
) (
    input  wire         clk,
    input  wire         rst_n,
    input  wire         in_valid,
    input  wire [31:0]  in_data,
    input  wire [3:0]   in_strb,
    output wire         in_accept,
    input  wire         frame_start,

    output wire         s_req,
    output wire [20:0]  s_addr,
    output wire [2:0]   s_len,
    input  wire         s_ack,
    input  wire         s_wr_take,
    output wire [31:0]  s_wdata,

    input  wire         fb_showing,
    input  wire [1:0]   fb_front,
    output wire         pub_v,
    output wire [1:0]   pub_buf,
    output wire [9:0]   pub_words,
    output wire [10:0]  pub_w,
    output wire [9:0]   pub_h,
    output wire         frame_ready,
    input  wire         pub_go,
    output wire         busy,
    output wire         dec_idle,
    output wire [15:0]  dbg,             // отладка: состояния vp_fbwrite
    output wire [63:0]  dbg2             // отладка: точка декодера, указатели кольца плиток
);
    wire        px_valid, px_ready;
    wire [15:0] px_x, px_y, w, h;
    wire [7:0]  r, g, b;
    wire [1:0]  mode;
    wire [7:0]  qy;
    wire        s_we;

    jpeg_core #(.SUPPORT_WRITABLE_DHT(1)) dec (
        .clk_i(clk), .rst_i(!rst_n),
        .inport_valid_i(in_valid), .inport_data_i(in_data), .inport_strb_i(in_strb),
        .inport_last_i(1'b0), .inport_accept_o(in_accept),
        .outport_accept_i(px_ready), .outport_valid_o(px_valid),
        .outport_width_o(w), .outport_height_o(h),
        .outport_pixel_x_o(px_x), .outport_pixel_y_o(px_y),
        .outport_pixel_r_o(r), .outport_pixel_g_o(g), .outport_pixel_b_o(b),
        .idle_o(dec_idle), .outport_mode_o(mode), .outport_qy_o(qy)
    );

    vp_fbwrite #(.DEBLOCK(DEBLOCK), .DITHER(DITHER), .DB_HALF(DB_HALF)) wr (
        .clk(clk), .rst_n(rst_n),
        .frame_start(frame_start), .frame_end(dec_idle), .abort(1'b0), .mode(mode), .img_w(w), .img_h(h), .qy(qy),
        .px_valid(px_valid), .px_ready(px_ready), .px_x(px_x), .px_y(px_y),
        .px_r(r), .px_g(g), .px_b(b),
        .s_req(s_req), .s_we(s_we), .s_addr(s_addr), .s_len(s_len), .s_ack(s_ack),
        .s_wr_take(s_wr_take), .s_wdata(s_wdata),
        .fb_showing(fb_showing), .fb_front(fb_front),
        .pub_v(pub_v), .pub_buf(pub_buf), .pub_words(pub_words), .pub_w(pub_w), .pub_h(pub_h),
        .frame_ready(frame_ready), .pub_go(pub_go), .busy(busy)
    );
    // {fst, состояния плиток 0…3 (младшие 2 бита), wst[1:0], est[0], px_valid, idle}
    assign dbg = {wr.fst, wr.tst[0][1:0], wr.tst[1][1:0], wr.tst[2][1:0], wr.tst[3][1:0], wr.wst[1:0],
                  wr.est[0], px_valid, dec_idle};
    assign dbg2 = {px_x, px_y, 6'd0, mode, wr.f_open, wr.fill_p, wr.eng_p, wr.fl_p, wr.fill_cnt, wr.geo_ok, wr.half, wr.th, 4'd0};
endmodule
