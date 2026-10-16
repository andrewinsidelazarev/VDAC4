// SPDX-License-Identifier: Apache-2.0
//
// Испытательная таблица кадрового буфера: пиксель {PAL_SEL, B5, G5, R5} в точке (x, y).
// Одна функция на три места: генератор fb_pattern (пишет таблицу в SDRAM), проверка
// на выходе fb_scanout (сверяет прочитанное из SDRAM, отсюда калибровка выборки чтения)
// и тестбенч. Подключается `include внутри модуля.
//
// Разметка для окна 720×576 (в уменьшенном окне тестбенча видны рамка и часть полос):
//   рамка в 1 точку по краю окна — белая: видно, весь ли кадр показывает монитор;
//   y   8…231  — восемь цветных полос по 88 точек (белый, жёлтый, голубой, зелёный,
//                пурпурный, красный, синий, серый);
//   y 240…303  — 32 ступени серого по 16 точек (x 96…607), PAL_SEL=1 (линейное расширение);
//   y 312…343  — те же ступени с PAL_SEL=0: палитра VDAC2 (выше 24 — насыщение);
//   y 352…383  — чередование белой и чёрной точки: соседние пиксели слова не перепутаны;
//   y 392…567  — сетка 16×16 по границам пакетов SDRAM на тёмно-синем; сдвиг слов при
//                неверной выборке чтения рвёт вертикальные линии;
//   y 456…487  — в полосе сетки едет жёлтый квадрат 32×32 (номер кадра f): видно, что
//                кадры обновляются. Проверка на выходе эту полосу пропускает.
//
localparam int FB_PAT_BAND_Y0 = 456;
localparam int FB_PAT_BAND_Y1 = 488;
localparam int FB_PAT_FRAMES  = 168;    // f = 0…167: квадрат проходит 8…711

function automatic [15:0] fb_pattern_px(input [10:0] x, input [10:0] y, input [7:0] f,
                                       input [10:0] w, input [10:0] h);
    reg [4:0]  r, g, b, v;
    reg        pal;
    reg [10:0] sx;
    begin
        r = 5'd0; g = 5'd0; b = 5'd0; pal = 1'b1;
        v = x[8:4] - 5'd6;                  // ступень (x − 96) / 16 по модулю 32
        sx = 11'd8 + {1'b0, f, 2'b00};
        if (x == 11'd0 || y == 11'd0 || x == w - 11'd1 || y == h - 11'd1) begin
            r = 5'd31; g = 5'd31; b = 5'd31;
        end else if (x < 11'd8 || x >= 11'd712) begin
            // поле слева и справа — чёрное
        end else if (y >= 11'd8 && y < 11'd232) begin
            if      (x < 11'd96)  begin r = 5'd31; g = 5'd31; b = 5'd31; end
            else if (x < 11'd184) begin r = 5'd31; g = 5'd31; b = 5'd0;  end
            else if (x < 11'd272) begin r = 5'd0;  g = 5'd31; b = 5'd31; end
            else if (x < 11'd360) begin r = 5'd0;  g = 5'd31; b = 5'd0;  end
            else if (x < 11'd448) begin r = 5'd31; g = 5'd0;  b = 5'd31; end
            else if (x < 11'd536) begin r = 5'd31; g = 5'd0;  b = 5'd0;  end
            else if (x < 11'd624) begin r = 5'd0;  g = 5'd0;  b = 5'd31; end
            else                  begin r = 5'd8;  g = 5'd8;  b = 5'd8;  end
        end else if (y >= 11'd240 && y < 11'd344 && (y < 11'd304 || y >= 11'd312)) begin
            if (x >= 11'd96 && x < 11'd608) begin r = v; g = v; b = v; end
            pal = (y < 11'd304);
        end else if (y >= 11'd352 && y < 11'd384) begin
            if (!x[0]) begin r = 5'd31; g = 5'd31; b = 5'd31; end
        end else if (y >= 11'd392 && y < 11'd568) begin
            if (y >= FB_PAT_BAND_Y0 && y < FB_PAT_BAND_Y1 && x >= sx && x < sx + 11'd32) begin
                r = 5'd31; g = 5'd31; b = 5'd0;
            end else if (x[3:0] == 4'd0 || y[3:0] == 4'd8) begin
                r = 5'd24; g = 5'd24; b = 5'd24;
            end else begin
                b = 5'd10;
            end
        end
        fb_pattern_px = {pal, b, g, r};
    end
endfunction
