// Стенд развёртки md_video (ядро 0.8, 04.11.2026: HDMI по стандарту, Mega Drive стоит под развёртку): «VDP»
// из raster_check.cpp — строки с гашением, синхроимпульсом и точками, идёт, только пока sync_stall = 0.
module raster_top(input wire hclk, mclk, input wire vblank, hblank, hsync, ce_pix, pal, v30,
                  output wire locked, sync_stall, output wire [8:0] trim, output wire [1:0] jumps,
                  output wire [9:0] vy, output wire [10:0] hx, output wire de, hs, vs);
wire cp, cn; wire [2:0] dp, dn;
md_video video(.mclk(mclk), .ce_pix(ce_pix), .hblank(hblank), .vblank(vblank), .hsync(hsync), .r(4'd0), .g(4'd0), .b(4'd0),
    .h40(1'b1), .v30(v30), .lace(1'b0), .field(1'b0), .pal(pal), .blank(1'b0), .sync_stall(sync_stall),
    .clk_osd(mclk), .osd_wr(1'b0), .osd_addr(9'd0), .osd_data(32'd0), .osd_show(1'b0), .osd_sel(6'd63),
    .osd_band(30'd0), .osd_rule(30'd0), .osd_bar(32'd0), .picture_size(3'd0),
    .hclk(hclk), .hclk5(hclk), .hreset(1'b0),
    .tmds_clk_p(cp), .tmds_clk_n(cn), .tmds_d_p(dp), .tmds_d_n(dn), .locked(locked), .hframes(), .hres_cnt(),
    .jump_cnt(jumps));
assign trim = video.trim, vy = video.vy, hx = video.hx, de = video.de, hs = video.hs, vs = video.vs;
endmodule
