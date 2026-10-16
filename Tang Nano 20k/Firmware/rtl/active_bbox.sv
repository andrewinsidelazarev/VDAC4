// SPDX-License-Identifier: Apache-2.0
//
// Границы не чёрной картинки во входном растре — для выравнивания окна захвата.
// Домен clk_cap.
//
// Координаты те же, что у окна захвата fb_capture: x — номер собранного пикселя от
// строба начала строки, y — номер строки от строба кадра. Поэтому если видимая область
// Evo целиком не чёрная (режим 360×288 или любой режим с цветным бордюром), то
// {bb_x0, bb_y0} — это ровно то смещение окна, при котором картинка встаёт точно.
//
// Итог кадра защёлкивается на стробе кадра; bb_new — импульс на такт позже.
// bb_valid = 0, если за кадр не нашлось ни одной не чёрной точки.
//
module active_bbox (
    input  wire        clk_cap,
    input  wire        rst_n,

    input  wire        pix_valid,
    input  wire [4:0]  pix_r,
    input  wire [4:0]  pix_g,
    input  wire [4:0]  pix_b,
    input  wire        hs_start,
    input  wire        vs_start,

    output reg         bb_new,
    output reg         bb_valid,
    output reg  [10:0] bb_x0,
    output reg  [10:0] bb_x1,
    output reg  [10:0] bb_y0,
    output reg  [10:0] bb_y1
);
    reg [10:0] in_x, in_line;
    reg [10:0] x0, x1, y0, y1;
    reg        any;

    wire lit = (pix_r != 5'd0) || (pix_g != 5'd0) || (pix_b != 5'd0);

    always_ff @(posedge clk_cap or negedge rst_n) begin
        if (!rst_n) begin
            in_x <= '0; in_line <= '0;
            x0 <= 11'h7FF; x1 <= '0; y0 <= 11'h7FF; y1 <= '0; any <= 1'b0;
            bb_new <= 1'b0; bb_valid <= 1'b0;
            bb_x0 <= '0; bb_x1 <= '0; bb_y0 <= '0; bb_y1 <= '0;
        end else begin
            bb_new <= 1'b0;
            // Счётчики — точь-в-точь как в fb_capture
            if (vs_start) begin
                in_x <= '0; in_line <= '0;
                bb_new   <= 1'b1;
                bb_valid <= any;
                bb_x0 <= x0; bb_x1 <= x1; bb_y0 <= y0; bb_y1 <= y1;
                x0 <= 11'h7FF; x1 <= '0; y0 <= 11'h7FF; y1 <= '0; any <= 1'b0;
            end else if (hs_start) begin
                in_x    <= '0;
                in_line <= in_line + 11'd1;
            end else if (pix_valid && in_x != 11'h7FF) begin
                in_x <= in_x + 11'd1;
                if (lit) begin
                    any <= 1'b1;
                    if (in_x < x0) x0 <= in_x;
                    if (in_x > x1) x1 <= in_x;
                    if (in_line < y0) y0 <= in_line;
                    if (in_line > y1) y1 <= in_line;
                end
            end
        end
    end
endmodule
