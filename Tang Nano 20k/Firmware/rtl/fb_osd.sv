// SPDX-License-Identifier: Apache-2.0
//
// Экранное меню поверх выходного видео. Домен пиксельного клока.
//
// Стоит между fb_scanout и кодерами TMDS: берёт готовый поток (цвет, DE, HS, VS) и
// отдаёт его на ROWS_LAT тактов позже вместе с синхроимпульсами — развёртка не меняется.
// Панель 40 × 21 символ шрифта 5×7 в ячейке 8×8, увеличенного вдвое (16×16 точек),
// с левого верхнего угла (OX, OY) активной области. Фон панели — на одно знакоместо шире
// текста со всех сторон; картинка под ним затемняется до 3/16 и чуть подсинивается.
//
// Текст — шаблон из ПЗУ (fb_osd_rom.sv, генерирует scripts/gen_osd_rom.py). Код шаблона:
//   0x000…0x03F  постоянный символ;
//   0x100 | F<<3 | d   разряд d числа F (d = 0 — единицы; старшие нули — пробелы);
//   0x180 | F<<3 | i   символ i поля состояния F (названия, флаги, курсор).
// Числа переводятся в десятичные последовательным преобразователем во время кадрового
// гашения — за активную часть кадра цифры не меняются.
//
// Конвейер (такт = точка): 0 — координаты; 1 — код шаблона; 2 — код символа;
// 3 — строка шрифта; 4 — выходной регистр. Видео задержано на 4 такта.
//
module fb_osd #(
    parameter int  OX     = 40,
    parameter int  OY     = 24,
    parameter bit  VS_NEG = 1'b1
) (
    input  wire              clk,
    input  wire              rst_n,
    input  wire              show,

    // Видео
    input  wire [7:0]        in_r,
    input  wire [7:0]        in_g,
    input  wire [7:0]        in_b,
    input  wire              in_de,
    input  wire              in_hs,
    input  wire              in_vs,
    output reg  [7:0]        out_r,
    output reg  [7:0]        out_g,
    output reg  [7:0]        out_b,
    output reg               out_de,
    output reg               out_hs,
    output reg               out_vs,

    // Вход Evo (домен пикселей, уже синхронизировано)
    input  wire [15:0]       pclk_khz,
    input  wire              locked,
    input  wire [2:0]        mode,
    input  wire [11:0]       line_cycles,
    input  wire [10:0]       frame_lines,
    input  wire [11:0]       hs_cycles,
    input  wire              hs_low,
    input  wire              vs_low,
    input  wire [10:0]       h_off,
    input  wire [10:0]       v_off,
    input  wire              aligned_h,
    input  wire              aligned_v,
    input  wire              bb_valid,
    input  wire [10:0]       bb_x0,
    input  wire [10:0]       bb_x1,
    input  wire [10:0]       bb_y0,
    input  wire [10:0]       bb_y1,
    input  wire              cap_overflow,
    input  wire              m_sel,
    // Кадровый буфер
    input  wire [15:0]       frames_written,
    input  wire [15:0]       frames_rejected,
    input  wire              fetch_late,
    input  wire              mem_ready,
    input  wire              pattern_on,
    input  wire [15:0]       chk_err,
    // Меню
    input  wire [1:0]        set_src,
    input  wire              cfg_phase,
    input  wire [1:0]        cfg_delay,
    input  wire [3:0]        cfg_clock,
    input  wire signed [7:0] cfg_h_adj,
    input  wire signed [7:0] cfg_v_adj,
    input  wire              cfg_align,
    input  wire              cfg_aspect,
    input  wire [1:0]        cfg_size,
    input  wire [1:0]        cfg_rd,
    input  wire [3:0]        cursor,
    input  wire              cal_busy,
    input  wire [1:0]        cal_point,
    input  wire [3:0]        cal_ok,
    input  wire              cal_done,
    input  wire              cal_fail
);
    localparam int ROWS = 21;              // как в шаблоне scripts/gen_osd_rom.py
    localparam int COLS = 40;
    localparam [10:0] X0 = OX, Y0 = OY;
    localparam [10:0] X1 = OX + COLS * 16, Y1 = OY + ROWS * 16;
    localparam [10:0] BX0 = OX - 16, BY0 = OY - 16, BX1 = X1 + 16, BY1 = Y1 + 16;   // фон
    localparam [5:0]  SPACE = 6'h00;
    localparam int    NFIELDS = 15;

    // ------------------------------------------------------------------
    // Координаты точки на входе
    // ------------------------------------------------------------------
    wire       vs_act = in_vs ^ VS_NEG;
    reg [10:0] x, y;
    reg        de_q, vs_q;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            x <= '0; y <= '0; de_q <= 1'b0; vs_q <= 1'b0;
        end else begin
            de_q <= in_de;
            vs_q <= vs_act;
            x    <= in_de ? x + 11'd1 : 11'd0;
            if (vs_act) y <= '0;
            else if (de_q && !in_de) y <= y + 11'd1;
        end
    end
    wire frame_start = vs_act && !vs_q;

    wire        in0  = show && in_de && x >= X0 && x < X1 && y >= Y0 && y < Y1;
    wire        bg0  = show && in_de && x >= BX0 && x < BX1 && y >= BY0 && y < BY1;
    wire [10:0] px0  = x - X0;
    wire [10:0] py0  = y - Y0;
    wire [5:0]  col0 = px0[9:4];
    wire [4:0]  row0 = py0[8:4];
    wire [9:0]  addr0 = {row0, 5'b0} + {2'b0, row0, 3'b0} + {4'b0, col0};

    // ------------------------------------------------------------------
    // Числа → BCD во время гашения
    // ------------------------------------------------------------------
    reg [19:0] bcd_arr [0:15];
    reg [15:0] bin;
    reg [19:0] bcd;
    reg [3:0]  fidx;
    reg [4:0]  bitn;
    reg        conv;

    wire [7:0] h_abs = cfg_h_adj[7] ? -cfg_h_adj : cfg_h_adj;
    wire [7:0] v_abs = cfg_v_adj[7] ? -cfg_v_adj : cfg_v_adj;

    function automatic [15:0] field_value(input [3:0] f);
        case (f)
            4'd0:  field_value = pclk_khz;
            4'd1:  field_value = {5'd0, line_cycles[11:1]};
            4'd2:  field_value = {5'd0, frame_lines};
            4'd3:  field_value = {5'd0, hs_cycles[11:1]};
            4'd4:  field_value = frames_written;
            4'd5:  field_value = frames_rejected;
            4'd6:  field_value = {5'd0, h_off};
            4'd7:  field_value = {5'd0, v_off};
            4'd8:  field_value = bb_valid ? {5'd0, bb_x0} : 16'd0;
            4'd9:  field_value = bb_valid ? {5'd0, bb_x1} : 16'd0;
            4'd10: field_value = bb_valid ? {5'd0, bb_y0} : 16'd0;
            4'd11: field_value = bb_valid ? {5'd0, bb_y1} : 16'd0;
            4'd12: field_value = chk_err;
            4'd13: field_value = {8'd0, h_abs};
            4'd14: field_value = {8'd0, v_abs};
            default: field_value = 16'd0;
        endcase
    endfunction

    // Поправка «+3» для каждой тетрады ≥ 5 перед сдвигом (double dabble)
    function automatic [19:0] dabble(input [19:0] v);
        integer k;
        begin
            for (k = 0; k < 5; k = k + 1)
                dabble[k*4 +: 4] = (v[k*4 +: 4] >= 4'd5) ? v[k*4 +: 4] + 4'd3 : v[k*4 +: 4];
        end
    endfunction

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            conv <= 1'b0; fidx <= '0; bitn <= '0; bin <= '0; bcd <= '0;
        end else if (frame_start) begin
            conv <= 1'b1; fidx <= '0; bitn <= 5'd16; bin <= field_value(4'd0); bcd <= '0;
        end else if (conv) begin
            if (bitn != 0) begin
                {bcd, bin} <= {dabble(bcd), bin} << 1;
                bitn <= bitn - 5'd1;
            end else begin
                bcd_arr[fidx] <= bcd;
                if (fidx == NFIELDS[3:0] - 4'd1) conv <= 1'b0;
                fidx <= fidx + 4'd1;
                bin  <= field_value(fidx + 4'd1);
                bcd  <= '0;
                bitn <= 5'd16;
            end
        end
    end

    // ------------------------------------------------------------------
    // Поля состояния: символ ASCII
    // ------------------------------------------------------------------
    function automatic [7:0] str8(input [63:0] s, input [2:0] i);
        str8 = s[(7 - i) * 8 +: 8];
    endfunction
    function automatic [7:0] yn(input b);
        yn = b ? "Y" : "N";
    endfunction

    wire pclk_ok = pclk_khz > 16'd1000;

    function automatic [7:0] char_field(input [3:0] f, input [2:0] i);
        reg [63:0] s;
        begin
            char_field = " ";
            case (f)
            4'd0: begin
                if (!pclk_ok)     s = "NO PCLK ";
                else if (!locked) s = "SEARCH  ";
                else case (mode)
                    3'd1: s = "TS PENT ";
                    3'd2: s = "TS 312  ";
                    3'd3: s = "BC PENT ";
                    3'd4: s = "BC 48K  ";
                    3'd5: s = "BC 128K ";
                    3'd6: s = "TEST A  ";
                    3'd7: s = "TEST B  ";
                    default: s = "UNKNOWN ";
                endcase
                char_field = str8(s, i);
            end
            4'd1: case (i)
                3'd0: char_field = yn(locked);
                3'd1: char_field = hs_low ? "-" : "+";
                3'd2: char_field = vs_low ? "-" : "+";
                3'd3: char_field = aligned_h ? "A" : "-";
                3'd4: char_field = aligned_v ? "A" : "-";
                3'd5: char_field = yn(fetch_late);
                3'd6: char_field = yn(cap_overflow);
                default: char_field = m_sel ? "1" : "0";
            endcase
            4'd2: begin
                case (set_src)
                    2'd0: s = "AUTO    ";
                    2'd1: s = "EVO     ";
                    default: s = "TEST    ";
                endcase
                char_field = str8(s, i);
            end
            4'd3: char_field = str8(cfg_align ? "ON      " : "OFF     ", i);
            4'd4: char_field = (cursor == {1'b0, i}) ? ">" : " ";
            4'd9: char_field = (i < 3'd3 && cursor == 4'd8 + {2'd0, i[1:0]}) ? ">" : " ";
            4'd10: char_field = str8(cfg_aspect ? "16:9    " : "4:3     ", i);
            4'd11: case (cfg_size)
                2'd1:    char_field = str8("95%     ", i);
                2'd2:    char_field = str8("90%     ", i);
                default: char_field = str8("100%    ", i);
            endcase
            4'd5: case (i)
                3'd0: char_field = "0" + {7'd0, cfg_phase};
                3'd1: char_field = "1" + {6'd0, cfg_delay};
                3'd2: char_field = "0" + {6'd0, cfg_rd};
                3'd3: char_field = cfg_h_adj[7] ? "-" : "+";
                3'd4: char_field = cfg_v_adj[7] ? "-" : "+";
                default: char_field = (cfg_clock < 4'd10) ? "0" + {4'd0, cfg_clock}
                                                          : "A" + {4'd0, cfg_clock} - 8'd10;
            endcase
            4'd6: char_field = str8(mem_ready ? "OK      " : "INIT    ", i);
            4'd7: char_field = str8(pattern_on ? "TEST    " : "EVO     ", i);
            4'd8: begin
                if (!i[2]) begin
                    if (cal_busy)
                        char_field = (i[1:0] > cal_point) ? "." : (i[1:0] == cal_point) ? "*" :
                                     cal_ok[i[1:0]] ? "O" : "X";
                    else if (cal_done) char_field = cal_ok[i[1:0]] ? "O" : "X";
                    else               char_field = "-";
                end else begin
                    s = cal_busy ? "    RUN " : cal_fail ? "    FAIL" : cal_done ? "    DONE" : "        ";
                    char_field = str8(s, i);
                end
            end
            default: ;
            endcase
        end
    endfunction

    function automatic [5:0] resolve(input [8:0] t);
        reg [19:0] v;
        reg [19:0] hi;
        reg [3:0]  nib;
        reg [7:0]  a;
        begin
            if (!t[8]) resolve = t[5:0];
            else if (!t[7]) begin
                v   = bcd_arr[t[6:3]];
                hi  = v >> (t[2:0] * 4);
                nib = hi[3:0];
                resolve = (t[2:0] != 3'd0 && hi == 20'd0) ? SPACE : 6'h10 + {2'd0, nib};
            end else begin
                a = char_field(t[6:3], t[2:0]);
                a = a - 8'h20;
                resolve = (a < 8'h40) ? a[5:0] : SPACE;
            end
        end
    endfunction

    // ------------------------------------------------------------------
    // Конвейер текста
    // ------------------------------------------------------------------
    wire [8:0] tq;
    fb_osd_text_rom text_rom (.clk(clk), .addr(addr0), .q(tq));

    reg        in1, in2, in3, bg1, bg2, bg3;
    reg [2:0]  gx1, gx2, gx3;
    reg [2:0]  gy1, gy2;
    reg [4:0]  row1, row2, row3;
    reg [5:0]  glyph2;
    wire [7:0] font_q;
    fb_osd_font_rom font_rom (.clk(clk), .addr({glyph2, gy2}), .q(font_q));

    reg [7:0] r1, g1, b1, r2, g2, b2, r3, g3, b3;
    reg       de1, hs1, vs1, de2, hs2, vs2, de3, hs3, vs3;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            in1 <= 1'b0; in2 <= 1'b0; in3 <= 1'b0; bg1 <= 1'b0; bg2 <= 1'b0; bg3 <= 1'b0;
            gx1 <= '0; gx2 <= '0; gx3 <= '0; gy1 <= '0; gy2 <= '0;
            row1 <= '0; row2 <= '0; row3 <= '0; glyph2 <= '0;
            {r1, g1, b1, r2, g2, b2, r3, g3, b3} <= '0;
            {de1, de2, de3} <= '0;
            {hs1, hs2, hs3} <= '1;
            {vs1, vs2, vs3} <= {3{VS_NEG}};
            out_r <= '0; out_g <= '0; out_b <= '0;
            out_de <= 1'b0; out_hs <= 1'b1; out_vs <= VS_NEG;
        end else begin
            // 1: код шаблона
            in1 <= in0; bg1 <= bg0; gx1 <= px0[3:1]; gy1 <= py0[3:1]; row1 <= row0;
            r1 <= in_r; g1 <= in_g; b1 <= in_b; de1 <= in_de; hs1 <= in_hs; vs1 <= in_vs;
            // 2: код символа
            in2 <= in1; bg2 <= bg1; gx2 <= gx1; gy2 <= gy1; row2 <= row1;
            glyph2 <= resolve(tq);
            r2 <= r1; g2 <= g1; b2 <= b1; de2 <= de1; hs2 <= hs1; vs2 <= vs1;
            // 3: строка шрифта
            in3 <= in2; bg3 <= bg2; gx3 <= gx2; row3 <= row2;
            r3 <= r2; g3 <= g2; b3 <= b2; de3 <= de2; hs3 <= hs2; vs3 <= vs2;
            // 4: выход
            out_de <= de3; out_hs <= hs3; out_vs <= vs3;
            if (in3 && font_q[3'd7 - gx3]) begin
                if (row3 == 5'd0)                          {out_r, out_g, out_b} <= {8'h40, 8'hFF, 8'hFF};
                else if (row3 == 5'd9 + {1'b0, cursor})    {out_r, out_g, out_b} <= {8'hFF, 8'hFF, 8'h40};
                else                                       {out_r, out_g, out_b} <= {8'hFF, 8'hFF, 8'hFF};
            end else if (bg3) begin
                // фон: 3/16 картинки
                out_r <= {3'b000, r3[7:3]} + {4'b0000, r3[7:4]};
                out_g <= {3'b000, g3[7:3]} + {4'b0000, g3[7:4]};
                out_b <= {3'b000, b3[7:3]} + {4'b0000, b3[7:4]} + 8'h18;
            end else begin
                out_r <= r3; out_g <= g3; out_b <= b3;
            end
        end
    end
endmodule
