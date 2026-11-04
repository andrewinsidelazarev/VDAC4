// Стенд слов TMDS md_video (tmds_check.cpp): VDP нет, меню включено (рамка в кадре), развёртка без захвата.
module tmds_top(input wire hclk, mclk, output wire [9:0] t0, t1, t2, output wire de, hs, vs, output wire [23:0] rgb);
wire cp, cn; wire [2:0] dp, dn;
md_video video(.mclk(mclk), .ce_pix(1'b0), .hblank(1'b1), .vblank(1'b1), .hsync(1'b0), .r(4'd0), .g(4'd0), .b(4'd0),
    .h40(1'b1), .v30(1'b0), .lace(1'b0), .field(1'b0), .pal(1'b0), .blank(1'b1),
    .clk_osd(mclk), .osd_wr(1'b0), .osd_addr(9'd0), .osd_data(32'd0), .osd_show(1'b1), .osd_sel(6'd63),
    .osd_band(30'd0), .osd_rule(30'd0), .osd_bar(32'd0), .picture_size(3'd0),
    .hclk(hclk), .hclk5(hclk), .hreset(1'b0),
    .tmds_clk_p(cp), .tmds_clk_n(cn), .tmds_d_p(dp), .tmds_d_n(dn), .locked(), .hframes());
assign t0 = video.tmds0, t1 = video.tmds1, t2 = video.tmds2;
assign de = video.de, hs = video.hs, vs = video.vs, rgb = video.rgb;
endmodule
