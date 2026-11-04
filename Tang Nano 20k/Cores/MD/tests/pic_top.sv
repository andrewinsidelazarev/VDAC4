// Стенд окна и масштаба md_video (03.11.2026, ядро 0.2: 16:9): «VDP» из pic_check.cpp выдаёт строки, цвет точки
// кодирует её координаты (R — x[3:0], G — x[7:4], B — {x[8], y[2:0]}); pic_check сверяет каждую точку HDMI.
module pic_top(input wire hclk, mclk, input wire vblank, hblank, ce_pix, hsync, input wire [3:0] r, g, b,
               input wire [2:0] picture_size, input wire pal, v30,
               output wire locked, sync_stall, output wire de, output wire [23:0] rgb, output wire [10:0] hx, output wire [9:0] vy);
wire cp, cn; wire [2:0] dp, dn;
md_video video(.mclk(mclk), .ce_pix(ce_pix), .hblank(hblank), .vblank(vblank), .hsync(hsync), .r(r), .g(g), .b(b),
    .h40(1'b1), .v30(v30), .lace(1'b0), .field(1'b0), .pal(pal), .blank(1'b0), .sync_stall(sync_stall),
    .clk_osd(mclk), .osd_wr(1'b0), .osd_addr(9'd0), .osd_data(32'd0), .osd_show(1'b0), .osd_sel(6'd63),
    .osd_band(30'd0), .osd_rule(30'd0), .osd_bar(32'd0), .picture_size(picture_size),
    .hclk(hclk), .hclk5(hclk), .hreset(1'b0),
    .tmds_clk_p(cp), .tmds_clk_n(cn), .tmds_d_p(dp), .tmds_d_n(dn), .locked(locked), .hframes(), .hres_cnt(),
    .jump_cnt());
assign de = video.de, rgb = video.rgb, hx = video.hx, vy = video.vy;   // ядро 0.8: «VDP» стоит при sync_stall
endmodule
