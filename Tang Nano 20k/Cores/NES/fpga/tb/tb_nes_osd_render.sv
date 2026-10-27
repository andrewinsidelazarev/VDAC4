// Кадр окна меню NES 0.10 (nes_osd.sv) — 1280×720, по строке RRGGBB на пиксель (в PNG — osd_png.py
// из claude-scripts): снимок окна из esp/tests/menu_ui_test.c (--dump):
// 512 слов текста, затем CTRL (показ, подсветка), подложки, разделители, полоса хода. Смешивание с картинкой —
// как в nes2hdmi_vdac4.sv (текст, подсветка, подложка, затемнённая картинка); картинка игры — условная
// (небо, холмы, земля) в окне 4:3. Запуск: vvp tb.vvp +dump=osd_root.hex +out=osd_root.rgb
`timescale 1ns/1ps
module tb;
    reg clk_pixel = 0, clk = 0;
    always #6.7 clk_pixel = ~clk_pixel;
    always #23.1 clk = ~clk;
    reg [10:0] cx = 0;
    reg [9:0] cy = 0;
    always @(posedge clk_pixel) begin
        if (cx == 1649) begin cx <= 0; cy <= cy == 749 ? 10'd0 : cy + 1'b1; end
        else cx <= cx + 1'b1;
    end
    reg [31:0] snap [0:515];
    reg show = 0;
    reg [5:0] sel = 63;
    reg [29:0] band = 0, rule = 0;
    reg [31:0] bar = 0;
    wire box, hl, bnd, text;
    wire [23:0] text_rgb;
    nes_osd dut(.clk(clk), .wr(1'b0), .wr_addr(9'd0), .wr_data(32'd0), .show(show), .sel(sel),
        .band_rows(band), .rule_rows(rule), .bar_cfg(bar), .clk_pixel(clk_pixel), .cx(cx), .cy(cy),
        .box(box), .hl(hl), .band(bnd), .text(text), .text_rgb(text_rgb));

    // условная картинка игры (4:3 — x 160…1119): небо, холмы, земля
    function automatic [23:0] scene(input [10:0] x, input [9:0] y);
        integer hill;
        hill = 470 + ((x * 7) % 90) - 45;
        if (x < 160 || x >= 1120) scene = 24'h000000;
        else if (y < hill) scene = {8'd60 + y[9:2], 8'd120 + y[9:3], 8'd230};
        else if (y < 560) scene = 24'h3C9C3C;
        else scene = {8'd150, 8'd90 + {x[4:0], 1'b0}, 8'd40};
    endfunction
    wire [23:0] nes_rgb = scene(cx, cy);
    wire active = cx < 1280 && cy < 720;
    reg [23:0] rgb;
    always @* begin
        if (!active) rgb = 0;
        else if (text) rgb = text_rgb;
        else if (hl) rgb = 24'h2C4C9C;
        else if (bnd) rgb = 24'h1E3266;
        else if (box) rgb = {3'b0, nes_rgb[23:19], 3'b0, nes_rgb[15:11], 3'b0, nes_rgb[7:3]} + 24'h080C28;
        else rgb = nes_rgb;
    end

    integer f, i;
    reg [8*256-1:0] dump_name, out_name;
    initial begin
        if (!$value$plusargs("dump=%s", dump_name)) dump_name = "osd_root.hex";
        if (!$value$plusargs("out=%s", out_name)) out_name = "osd_root.rgb";
        $readmemh(dump_name, snap);
        for (i = 0; i < 512; i = i + 1) dut.tram[i] = snap[i];
        show = snap[512][0]; sel = snap[512][13:8];
        band = snap[513][29:0]; rule = snap[514][29:0]; bar = snap[515];
        // два кадра: показ и оформление берутся с начала кадра
        repeat (2) begin do @(posedge clk_pixel); while (!(cx == 0 && cy == 0)); @(posedge clk_pixel); end
        do @(posedge clk_pixel); while (!(cx == 1649 && cy == 749));
        f = $fopen(out_name, "w");
        repeat (1650 * 750) begin
            @(posedge clk_pixel);
            #1;
            if (cx < 1280 && cy < 720) $fwrite(f, "%06h\n", rgb);
        end
        $fclose(f);
        $display("кадр — %0s", out_name);
        $finish;
    end
endmodule
