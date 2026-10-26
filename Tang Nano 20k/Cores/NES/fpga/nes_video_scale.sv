// GPL-3.0-or-later. Геометрия NES для 720p: 4:3 (960x720) либо 16:9 (1280x720).
// Как в NESTang, видны центральные 256x224 пикселя исходного кадра.
module nes_video_scale (
    input clk_nes, input clk_pixel, input resetn, input f6,
    input restore, input restore_wide,
    input [10:0] cx, input [9:0] cy,
    output [15:0] address, output active, output [7:0] xx, output [7:0] yy,
    output reg wide = 0, output reg requested = 0
);
    reg f6_prev = 0;
    always @(posedge clk_nes) begin
        if (!resetn) begin f6_prev <= 0; requested <= 0; end
        else begin
            f6_prev <= f6;
            if (restore) requested <= restore_wide;
            else if (f6 && !f6_prev) requested <= ~requested;
        end
    end
    reg [1:0] mode_sync = 0;
    always @(posedge clk_pixel) begin
        mode_sync <= {mode_sync[0], requested};
        if (!resetn) begin mode_sync <= 0; wide <= 0; end
        else if (cx == 0 && cy == 720) wide <= mode_sync[1];
    end
    // BRAM читает за такт, RGB ещё регистрируется. Готовим адрес следующего
    // пикселя; active относится к текущему — данные BRAM приходят именно для него.
    wire [10:0] nx = cx == 1649 ? 11'd0 : cx + 1'b1;
    wire [9:0] ny = cx == 1649 ? (cy == 749 ? 10'd0 : cy + 1'b1) : cy;
    wire [10:0] left_edge = wide ? 11'd0 : 11'd160;
    wire [10:0] right_edge = wide ? 11'd1280 : 11'd1120;
    wire next_active = nx >= left_edge && nx < right_edge && ny < 720;
    reg [7:0] x_index = 0, y_index = 0;
    reg [10:0] x_remainder = 0;
    reg [9:0] y_remainder = 0;
    wire [10:0] width = wide ? 11'd1280 : 11'd960;
    wire [11:0] x_sum = {1'b0, x_remainder} + 12'd256;
    wire [10:0] y_sum = {1'b0, y_remainder} + 11'd224;
    // Индекс — координата следующего чтения BRAM. Строка подготавливается
    // в blanking; для full width первый пиксель читается при cx=1649.
    wire step_x = wide ? (cx == 1649 || cx < 1279) : (cx >= 159 && cx < 1119);
    always @(posedge clk_pixel) begin
        if (!resetn) begin
            x_index <= 0; y_index <= 0; x_remainder <= 0; y_remainder <= 0;
        end else begin
            if (cx == 1648) begin x_index <= 0; x_remainder <= 0; end
            else if (step_x) begin
                if (x_sum >= {1'b0, width}) begin
                    x_remainder <= x_sum - {1'b0, width};
                    x_index <= x_index + 1'b1;
                end else x_remainder <= x_sum[10:0];
            end
            if (cx == 1648) begin
                if (cy == 749) begin y_index <= 0; y_remainder <= 0; end
                else if (cy < 720) begin
                    if (y_sum >= 720) begin
                        y_remainder <= y_sum - 11'd720;
                        y_index <= y_index + 1'b1;
                    end else y_remainder <= y_sum[9:0];
                end
            end
        end
    end
    assign xx = next_active ? x_index : 8'd0;
    assign yy = next_active ? y_index : 8'd0;
    assign address = {yy + 8'd8, xx};
    assign active = cx >= left_edge && cx < right_edge && cy < 720;
endmodule
