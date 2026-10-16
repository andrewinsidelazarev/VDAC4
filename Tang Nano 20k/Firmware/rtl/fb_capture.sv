// SPDX-License-Identifier: Apache-2.0
//
// Упаковка принятого видео TS-Config для записи в кадровый буфер.
//
// Домен clk_cap (56 МГц). На входе — собранные ts_mux_capture пиксели и стробы
// начала строки/кадра. Модуль выделяет активное окно H_ACTIVE×V_ACTIVE со смещением
// h_off/v_off (их выбирает raster_detect по растру) и пишет в FIFO перехода в домен
// SDRAM записи по 33 бита:
//
//   {1, …, trailer=0, first, y[9:0]}   — заголовок строки; first=1 у строки 0;
//   {1, …, trailer=1, …, ok}           — конец кадра: ok=1, если кадр целиком прошёл
//                                        в захваченном растре (raster_detect.frame_ok);
//   {0, p1[15:0], p0[15:0]}            — два соседних пикселя, p0 — чётный.
//
// Окно и разрешение захвата фиксируются в начале кадра: смена растра посреди кадра
// не может дать кадр, склеенный из двух окон, — такой кадр получит ok=0.
// Пиксель хранится как в строчном буфере генлок-тракта: {PAL_SEL, B5, G5, R5}.
//
module fb_capture #(
    parameter int H_ACTIVE    = 720,
    parameter int V_ACTIVE    = 576
) (
    input  wire        clk_cap,
    input  wire        rst_n,

    input  wire        pix_valid,
    input  wire [4:0]  pix_r,
    input  wire [4:0]  pix_g,
    input  wire [4:0]  pix_b,
    input  wire        pix_pal,
    input  wire        hs_start,
    input  wire        vs_start,

    input  wire        enable,       // растр захвачен
    input  wire [10:0] h_off,
    input  wire [10:0] v_off,
    input  wire        frame_ok,     // от raster_detect, обновляется на vs_start

    output reg         fifo_wr,
    output reg  [32:0] fifo_data,
    input  wire        fifo_full,
    output reg         overflow      // липкий: FIFO переполнялся (для отладки и теста)
);
    reg [10:0] in_x;
    reg [10:0] in_line;
    reg        line_start;           // такт после hs_start: номер строки уже новый
    reg        frame_end;            // такт после vs_start: frame_ok уже обновлён
    reg [15:0] even_pix;
    reg        en_q;                 // захват в текущем кадре
    reg        en_prev;              // захват шёл в прошлом кадре (нужен итог)
    reg [10:0] h_q, v_q;

    wire [10:0] rel_x = in_x - h_q;
    wire [10:0] rel_y = in_line - v_q;
    wire line_active = en_q && (in_line >= v_q) && (in_line < v_q + V_ACTIVE[10:0]);
    wire x_active    = (in_x >= h_q) && (in_x < h_q + H_ACTIVE[10:0]);

    always_ff @(posedge clk_cap or negedge rst_n) begin
        if (!rst_n) begin
            in_x       <= '0;
            in_line    <= '0;
            line_start <= 1'b0;
            frame_end  <= 1'b0;
            even_pix   <= '0;
            en_q       <= 1'b0;
            en_prev    <= 1'b0;
            h_q        <= '0;
            v_q        <= '0;
            fifo_wr    <= 1'b0;
            fifo_data  <= '0;
            overflow   <= 1'b0;
        end else begin
            fifo_wr    <= 1'b0;
            line_start <= 1'b0;
            frame_end  <= 1'b0;

            // Счётчики — как в генлок-тракте (ts_video_chain): кадр от VSYNC, строка от HSYNC,
            // отсчёт в строке — по собранным пикселям.
            if (vs_start) begin
                in_x      <= '0;
                in_line   <= '0;
                frame_end <= 1'b1;
                en_prev   <= en_q;
                en_q      <= enable;
                h_q       <= h_off;
                v_q       <= v_off;
            end else if (hs_start) begin
                in_x       <= '0;
                in_line    <= in_line + 11'd1;
                line_start <= 1'b1;
            end else if (pix_valid && in_x != 11'h7FF) begin
                in_x <= in_x + 11'd1;
            end

            if (frame_end && en_prev) begin
                fifo_wr   <= 1'b1;
                fifo_data <= {1'b1, 20'd0, 1'b1, 10'd0, frame_ok};
            end else if (line_start && line_active) begin
                fifo_wr   <= 1'b1;
                fifo_data <= {1'b1, 20'd0, 1'b0, (rel_y == 11'd0), rel_y[9:0]};
            end else if (pix_valid && line_active && x_active) begin
                if (!rel_x[0]) begin
                    even_pix <= {pix_pal, pix_b, pix_g, pix_r};
                end else begin
                    fifo_wr   <= 1'b1;
                    fifo_data <= {1'b0, pix_pal, pix_b, pix_g, pix_r, even_pix};
                end
            end

            if (fifo_wr && fifo_full)
                overflow <= 1'b1;
        end
    end
endmodule
