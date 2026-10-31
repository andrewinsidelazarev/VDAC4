// Exercise the actual AGA CLUT, generated NanoMig video section, Amber and DVI encoder.
module video_top (
    input clk, reset, write_color, loct, ehb, hblank, vblank, hs_in,
    input [7:0] palette_index, write_index,
    input [11:0] palette_data,
    input [31:0] video_probe,
    input [1:0] filter, scanline,
    output [23:0] clut_rgb, rgb,
    output [9:0] tmds_b, tmds_g, tmds_r,
    output reg video_symbol, dark_line, output hs, vs,
    output [11:0] legacy_rgb, output legacy_hs, legacy_vs
);
    denise_colortable palette(.clk(clk),.clk7_en(write_color),
        .reg_address_in({3'b110,write_index[4:0]}),.data_in(palette_data),
        .rdram(1'b0),.select(palette_index),.bplxor(8'd0),.bank(write_index[7:5]),
        .loct(loct),.ehb_en(ehb),.rgb(clut_rgb));
    wire [7:0] r,g,b;
    video_path_from_source path(.clk_sys(clk),.hbl(hblank),.vbl(vblank),
        .hs_in(hs_in),.vs_in(1'b1),.htotal(9'd453),.res(2'd1),
        .video_config({filter,scanline}),.rgb_in(clut_rgb),.r(r),.g(g),.b(b),.hs(hs),.vs(vs));
    assign rgb={r,g,b};
    always @(posedge clk) dark_line<=path.AMBER.scanline_ena;
    wire [3:0] lr,lg,lb;
    Amber4 legacy(.clk28m(clk),.lr_filter(filter),.hr_filter(filter),.scanline(scanline),
        .htotal(8'd226),.hires(1'b1),.dblscan(1'b1),
        .red_in(hblank||vblank ? 4'd0 : clut_rgb[23:20]),
        .green_in(hblank||vblank ? 4'd0 : clut_rgb[15:12]),
        .blue_in(hblank||vblank ? 4'd0 : clut_rgb[7:4]),
        ._hsync_in(hs_in),._vsync_in(1'b1),._csync_in(1'b1),
        .red_out(lr),.green_out(lg),.blue_out(lb),._hsync_out(legacy_hs),._vsync_out(legacy_vs));
    assign legacy_rgb={lr,lg,lb};
    // The actual top-level RGB connection is emitted from amiga_vdac4.sv.
    `include "video_hdmi_from_source.svh"
    assign tmds_b=dvi.tmds_internal[0];
    assign tmds_g=dvi.tmds_internal[1];
    assign tmds_r=dvi.tmds_internal[2];
    always @(posedge clk) video_symbol<=dvi.mode==3'd1;
endmodule

// Test stops at the parallel TMDS symbols. OSER10 electrical serialization is
// checked by Gowin STA and still needs board validation.
module serializer #(parameter NUM_CHANNELS=3, VIDEO_RATE=32000000)(
    input clk_pixel,clk_pixel_x5,reset,input [9:0] tmds_internal[NUM_CHANNELS-1:0],
    output [2:0] tmds,output tmds_clock);
    assign tmds=0;assign tmds_clock=clk_pixel;
endmodule
