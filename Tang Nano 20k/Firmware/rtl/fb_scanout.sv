// SPDX-License-Identifier: Apache-2.0
//
// Вывод кадрового буфера стандартной развёрткой. По умолчанию — 720×576p50
// (CEA-861, VIC 17/18): пиксельный клок 27,000 МГц, строка 864, кадр 625,
// ровно 50,000 Гц и 31,25 кГц; синхроимпульсы отрицательные; кадр Evo H_ACTIVE×V_ACTIVE
// стоит окном с (IMG_X, IMG_Y) точка в точку.
//
// МАСШТАБ (SCALE = 1, 1280×720p50: VIC 19, 74,25 МГц, строка 1980, кадр 750,
// синхроимпульсы положительные). Кадр Evo 720×576 растягивается по высоте на долю кадра
// size и по ширине по aspect, по центру, вокруг — чёрное поле:
//
//   size \ aspect      0 — 4:3 (пропорции)        1 — 16:9 (на всю ширину)
//   0 — 100 %          960×720   (x 160, y 0)     1280×720 (x 0,  y 0)
//   1 — 95 %           912×684   (x 184, y 18)    1216×684 (x 32, y 18)
//   2 — 90 %           864×648   (x 208, y 36)    1152×648 (x 64, y 36)
//
// 95 и 90 % — для телевизоров, которые обрезают края кадра (overscan; у старых LG Just Scan
// для 720p нет). Все размеры делят 720 и 576 нацело.
// Точка вывода берёт точку источника x = ⌊k·N/D⌋ (k — точка вывода в картинке, N/D =
// 720/ширина или 576/высота, несократимая дробь) — счётчиками (DDA), без умножений.
// Множители дробные, поэтому строки и точки Evo занимают неодинаковое число строк и точек
// вывода. aspect и size защёлкиваются за 4 строки до конца кадра (в гашении, до первых
// запросов строк нового кадра) — смена не рвёт кадр.
//
// КАДР FT812 (ft = 1, M_SEL; только SCALE). Источник — кадр растеризатора src_w×src_h
// (RGB565; 1024×768 и 640×480 — основные режимы VDAC2) в те же окна вывода; кадр другого
// размера масштабируется как ближайший из двух (ширина ≤ 640 — как 640×480). 1024×768 в
// 960×720 — уменьшение (N/D = 16/15): точка и строка источника за точку вывода растут на
// 1 или 2, строке вывода может понадобиться две новые строки источника — они просятся одним
// запросом (req_two). Режим и размер защёлкиваются вместе с геометрией; режим уходит
// в домен SDRAM с запросами (req_ft), поэтому смена M_SEL тоже не рвёт кадр.
//
// Домен pixel_clk. Строки берутся из двухстрочного буфера fb_line_ram, который
// заполняет fb_core. Строка картинки запрашивается за две строки вывода до показа,
// в начале гашения, — если той строке вывода нужна новая строка источника: каждая
// строка Evo читается из SDRAM один раз за кадр. Её половина буфера к этому времени
// свободна: строка источника s − 2 последний раз показывалась не позже текущей строки.
// У SDRAM есть больше строки, а строка читается за ~10–20 мкс.
//
// Задержка тракта: адрес памяти → данные (1 такт) → палитра и регистр (1 такт).
// Синхроимпульсы и DE задерживаются так же, поэтому картинка не сдвигается.
//
// Проверка испытательной таблицы: каждый прочитанный из SDRAM пиксель сравнивается
// с fb_pattern_px (кроме полосы с движущимся квадратом). Число расхождений за кадр
// выдаётся в chk_err с импульсом chk_tick после последней активной строки. Значение
// осмысленно, когда выводится таблица; по нему калибруется выборка чтения SDRAM.
//
module fb_scanout #(
    parameter int H_ACTIVE = 720,        // картинка (кадр Evo)
    parameter int V_ACTIVE = 576,
    parameter int OUT_W    = H_ACTIVE,   // активная часть выходного кадра
    parameter int OUT_H    = V_ACTIVE,
    parameter int IMG_X    = 0,          // где в нём картинка
    parameter int IMG_Y    = 0,
    parameter int H_FP     = 12,
    parameter int H_SYNC   = 64,
    parameter int H_BP     = 68,
    parameter int V_FP     = 5,
    parameter int V_SYNC   = 5,
    parameter int V_BP     = 39,
    parameter bit HS_NEG   = 1'b1,
    parameter bit VS_NEG   = 1'b1,
    parameter bit SCALE    = 1'b0    // 1 — на всю высоту (только 720×576 → 1280×720)
) (
    input  wire        pixel_clk,
    input  wire        rst_n,
    input  wire        blank,        // 1 — выводить чёрное поле (синхроимпульсы идут)
    input  wire        aspect,       // при SCALE: 0 — 4:3, 1 — 16:9 (на всю ширину)
    input  wire [1:0]  size,         // при SCALE: 0 — 100 %, 1 — 95 %, 2 — 90 % (3 — как 0)
    input  wire        ft,           // при SCALE: выводить кадр FT812
    input  wire [10:0] src_w,        // его размер (меняется редко): ширина 16…1024
    input  wire [9:0]  src_h,        //   высота 2…768

    // Запрос строки в домен SDRAM
    output reg         req_toggle,
    output reg  [9:0]  req_line,
    output reg         req_first,
    output reg         req_two,      // сначала строка req_line − 1, потом req_line
    output reg         req_ft,       // кадр FT812

    // Чтение четырёхстрочного буфера (fb_line_ram): {строка[1], слово[8:0]} каждого блока
    output wire [9:0]  lb_ra_e,
    output wire [9:0]  lb_ra_o,
    output wire [9:0]  lb_rb_e,
    output wire [9:0]  lb_rb_o,
    input  wire [15:0] lb_qa_e,
    input  wire [15:0] lb_qa_o,
    input  wire [15:0] lb_qb_e,
    input  wire [15:0] lb_qb_o,

    output reg  [7:0]  out_r,
    output reg  [7:0]  out_g,
    output reg  [7:0]  out_b,
    output reg         out_de,
    output reg         out_hs,
    output reg         out_vs,

    // Проверка испытательной таблицы
    output reg  [15:0] chk_err,
    output reg         chk_tick
);
    `include "fb_pattern_fn.svh"

    localparam int H_TOTAL = OUT_W + H_FP + H_SYNC + H_BP;
    localparam int V_TOTAL = OUT_H + V_FP + V_SYNC + V_BP;
    localparam [11:0] H_LAST     = H_TOTAL - 1;
    localparam [11:0] V_LAST     = V_TOTAL - 1;
    localparam [11:0] OUT_W_C    = OUT_W;
    localparam [11:0] OUT_H_C    = OUT_H;
    localparam [11:0] IMG_X0     = IMG_X;
    localparam [11:0] IMG_X1     = IMG_X + H_ACTIVE;
    localparam [11:0] IMG_Y0     = IMG_Y;
    localparam [11:0] IMG_Y1     = IMG_Y + V_ACTIVE;
    localparam [11:0] HS_BEGIN   = OUT_W + H_FP;
    localparam [11:0] HS_END     = OUT_W + H_FP + H_SYNC;
    localparam [11:0] VS_BEGIN   = OUT_H + V_FP;
    localparam [11:0] VS_END     = OUT_H + V_FP + V_SYNC;

    // Строка, в конце которой защёлкивается геометрия следующего кадра
    localparam [11:0] V_LATCH    = V_TOTAL - 5;

    initial begin
        if (IMG_X + H_ACTIVE > OUT_W || IMG_Y + V_ACTIVE > OUT_H)
            $error("fb_scanout: картинка не помещается в выходной кадр");
        if (SCALE && (H_ACTIVE != 720 || V_ACTIVE != 576 || OUT_W != 1280 || OUT_H != 720))
            $error("fb_scanout: масштаб рассчитан только на 720×576 → 1280×720");
    end

    // Окно картинки в кадре вывода и шаг по источнику: N/D точки источника на точку вывода.
    // gxs — h такта перед первой точкой картинки (для x = 0 — последний такт строки).
    reg  [11:0] gx0, gx1, gxs, gy0, gy1;
    reg  [6:0]  gnx, gdx, gny, gdy;
    reg  [16:0] grx, gry;           // 65536 / N, для весов сглаживания
    reg         gft;                // кадр FT812
    reg  [9:0]  glast;              // последняя строка источника

    // 65536 / N с округлением — для N из таблицы ниже (константы, без делителя)
    function automatic [16:0] recip(input [6:0] n);
        case (n)
            7'd1: recip = 17'd65536; 7'd2: recip = 17'd32768; 7'd3: recip = 17'd21845;
            7'd4: recip = 17'd16384; 7'd5: recip = 17'd13107; 7'd8: recip = 17'd8192;
            7'd9: recip = 17'd7282;  7'd10: recip = 17'd6554; 7'd15: recip = 17'd4369;
            7'd16: recip = 17'd4096; 7'd20: recip = 17'd3277; 7'd32: recip = 17'd2048;
            7'd40: recip = 17'd1638; 7'd45: recip = 17'd1456; 7'd64: recip = 17'd1024;
            default: recip = 17'd0;
        endcase
    endfunction

    // Окно вывода по ASPECT/SIZE: {x0, x1, y0, y1}
    function automatic [47:0] window(input asp, input [1:0] sz);
        case ({asp, sz})
            3'b0_01: window = {12'd184, 12'd1096, 12'd18, 12'd702};
            3'b0_10: window = {12'd208, 12'd1072, 12'd36, 12'd684};
            3'b1_00, 3'b1_11:
                     window = {12'd0,   12'd1280, 12'd0,  12'd720};
            3'b1_01: window = {12'd32,  12'd1248, 12'd18, 12'd702};
            3'b1_10: window = {12'd64,  12'd1216, 12'd36, 12'd684};
            default: window = {12'd160, 12'd1120, 12'd0,  12'd720};
        endcase
    endfunction

    // Шаг по источнику {nx, dx, ny, dy}, несократимые дроби: кадр Evo 720×576 и два режима
    // FT812 — 1024×768 и 640×480 (src_w ≤ 640 — как 640×480, иначе как 1024×768)
    function automatic [27:0] fracs(input f, input narrow, input asp, input [1:0] sz);
        if (!f) case ({asp, sz})
            3'b0_01: fracs = {7'd15, 7'd19, 7'd16, 7'd19};
            3'b0_10: fracs = {7'd5,  7'd6,  7'd8,  7'd9};
            3'b1_00, 3'b1_11:
                     fracs = {7'd9,  7'd16, 7'd4,  7'd5};
            3'b1_01: fracs = {7'd45, 7'd76, 7'd16, 7'd19};
            3'b1_10: fracs = {7'd5,  7'd8,  7'd8,  7'd9};
            default: fracs = {7'd3,  7'd4,  7'd4,  7'd5};
        endcase
        else if (!narrow) case ({asp, sz})
            3'b0_01: fracs = {7'd64, 7'd57, 7'd64, 7'd57};
            3'b0_10: fracs = {7'd32, 7'd27, 7'd32, 7'd27};
            3'b1_00, 3'b1_11:
                     fracs = {7'd4,  7'd5,  7'd16, 7'd15};
            3'b1_01: fracs = {7'd16, 7'd19, 7'd64, 7'd57};
            3'b1_10: fracs = {7'd8,  7'd9,  7'd32, 7'd27};
            default: fracs = {7'd16, 7'd15, 7'd16, 7'd15};
        endcase
        else case ({asp, sz})
            3'b0_01: fracs = {7'd40, 7'd57, 7'd40, 7'd57};
            3'b0_10: fracs = {7'd20, 7'd27, 7'd20, 7'd27};
            3'b1_00, 3'b1_11:
                     fracs = {7'd1,  7'd2,  7'd2,  7'd3};
            3'b1_01: fracs = {7'd10, 7'd19, 7'd40, 7'd57};
            3'b1_10: fracs = {7'd5,  7'd9,  7'd20, 7'd27};
            default: fracs = {7'd2,  7'd3,  7'd2,  7'd3};
        endcase
    endfunction

    reg [11:0] h, v;
    reg        vs_level;             // активен ли кадровый импульс (меняется с началом HSYNC)
    // Строка источника: для запроса (строка вывода v+2) и для показа (строка v)
    reg [9:0]  rq_s, sy;
    reg [6:0]  rq_acc, sy_acc;
    reg [9:0]  rq_max;               // старшая уже запрошенная строка кадра
    reg [11:0] rq_want;              // шаг 1 запроса: строка вывода v+2 и её строки источника
    reg [9:0]  rq_ns, rq_need;
    reg [6:0]  rq_nacc;
    // Точка источника в строке
    reg [9:0]  sx;
    reg [6:0]  sx_acc;

    // Шаг счётчика N/D: acc + N, минус D один или два раза (уменьшение — до двух за шаг)
    function automatic [8:0] dda(input [6:0] acc, input [6:0] n, input [6:0] d);
        reg [7:0] s;
        begin
            s = {1'b0, acc} + {1'b0, n};
            if (s >= {d, 1'b0})       dda = {2'd2, 7'(s - {d, 1'b0})};
            else if (s >= {1'b0, d})  dda = {2'd1, 7'(s - {1'b0, d})};
            else                      dda = {2'd0, 7'(s)};
        end
    endfunction

    wire [11:0] v_next = (v == V_LAST) ? 12'd0 : v + 12'd1;
    wire [8:0]  rq_st  = dda(rq_acc, gny, gdy);
    wire [8:0]  sy_st  = dda(sy_acc, gny, gdy);
    wire [8:0]  sx_st  = dda(sx_acc, gnx, gdx);
    wire [47:0] w_new  = window(aspect, size);
    wire [27:0] f_new  = fracs(ft, src_w <= 11'd640, aspect, size);
    wire [9:0]  last_new = ft ? src_h - 10'd1 : 10'(V_ACTIVE - 1);

    always_ff @(posedge pixel_clk or negedge rst_n) begin
        if (!rst_n) begin
            h          <= '0;
            v          <= '0;
            vs_level   <= 1'b0;
            req_toggle <= 1'b0;
            req_line   <= '0;
            req_first  <= 1'b0;
            req_two    <= 1'b0;
            req_ft     <= 1'b0;
            rq_s <= '0; rq_acc <= '0; sy <= '0; sy_acc <= '0; sx <= '0; sx_acc <= '0;
            rq_max <= '0; rq_want <= '0; rq_ns <= '0; rq_need <= '0; rq_nacc <= '0;
            gft <= 1'b0; glast <= 10'(V_ACTIVE - 1);
            if (SCALE) begin
                // как window(0, 0) и fracs(кадр Evo, 4:3, 100 %): константы — без логики сброса
                gx0 <= 12'd160; gx1 <= 12'd1120; gy0 <= 12'd0; gy1 <= 12'd720;
                gnx <= 7'd3; gdx <= 7'd4; gny <= 7'd4; gdy <= 7'd5;
                gxs <= 12'd159;
                grx <= 17'd21845; gry <= 17'd16384;
            end else begin
                gx0 <= IMG_X0; gx1 <= IMG_X1; gy0 <= IMG_Y0; gy1 <= IMG_Y1;
                gxs <= (IMG_X == 0) ? H_LAST : IMG_X0 - 12'd1;
                gnx <= 7'd1; gdx <= 7'd1; gny <= 7'd1; gdy <= 7'd1;
                grx <= 17'd65536; gry <= 17'd65536;
            end
        end else begin
            if (h == H_LAST) begin
                h <= '0;
                v <= v_next;
                // Строка источника для показа следующей строки
                if (v_next == gy0) begin
                    sy <= '0; sy_acc <= '0;
                end else if (v_next > gy0 && v_next < gy1) begin
                    sy <= sy + 10'(sy_st[8:7]); sy_acc <= sy_st[6:0];
                end
                // Геометрия следующего кадра — в гашении, до первых запросов его строк
                if (SCALE && v == V_LATCH) begin
                    {gx0, gx1, gy0, gy1} <= w_new;
                    {gnx, gdx, gny, gdy} <= f_new;
                    gxs <= (w_new[47:36] == 12'd0) ? H_LAST : w_new[47:36] - 12'd1;
                    grx <= recip(f_new[27:21]); gry <= recip(f_new[13:7]);
                    gft <= ft;
                    glast <= last_new;
                end
            end else begin
                h <= h + 12'd1;
            end

            // Точка источника: с нуля в начале картинки, дальше шаг N/D за такт
            if (h == gxs) begin
                sx <= '0; sx_acc <= '0;
            end else begin
                sx <= sx + 10'(sx_st[8:7]); sx_acc <= sx_st[6:0];
            end

            // Кадровый импульс переключается вместе с фронтом строчного, как в CEA-861
            if (h == HS_BEGIN)
                vs_level <= (v >= VS_BEGIN) && (v < VS_END);

            // Запрос строки картинки для строки вывода v+2 в начале гашения текущей строки.
            // Строке вывода нужны строка источника s и, если она заходит на следующую, s+1;
            // запрашивается старшая из них, если её ещё нет. При увеличении старшая растёт не
            // больше чем на 1 за строку вывода (строка, кончившаяся ровно на границе,
            // следующую не задевает), при уменьшении (N/D < 1,5) — на 1 или 2: тогда одним
            // запросом две строки (req_two). Запрос — не чаще раза за строку. Первый запрос
            // кадра — сразу строки 0 и 1: при уменьшении строка 1 нужна уже первой строке вывода.
            // Шаг 1 (за два такта до конца активной части): следующее состояние счётчика строк.
            if (h == OUT_W_C - 12'd2) begin : request_next
                logic [11:0] want;
                want = (v + 12'd2 >= V_TOTAL[11:0]) ? v + 12'd2 - V_TOTAL[11:0] : v + 12'd2;
                rq_want <= want;
                rq_ns   <= rq_s + 10'(rq_st[8:7]);
                rq_nacc <= rq_st[6:0];
            end
            // Шаг 1б (такт до конца): нужна ли строке вывода и следующая строка источника
            if (h == OUT_W_C - 12'd1)
                rq_need <= ({1'b0, rq_nacc} + {1'b0, gny} > {1'b0, gdy} && rq_ns < glast) ?
                           rq_ns + 10'd1 : rq_ns;
            // Шаг 2 (начало гашения): запрос
            if (h == OUT_W_C) begin : request
                if (rq_want == gy0) begin
                    rq_s <= '0; rq_acc <= '0; rq_max <= 10'd1;
                    req_line   <= 10'd1;
                    req_first  <= 1'b1;
                    req_two    <= 1'b1;
                    req_ft     <= gft;
                    req_toggle <= ~req_toggle;
                end else if (rq_want > gy0 && rq_want < gy1) begin
                    rq_s <= rq_ns; rq_acc <= rq_nacc;
                    if (rq_need > rq_max) begin
                        rq_max     <= rq_need;
                        req_line   <= rq_need;
                        req_first  <= 1'b0;
                        req_two    <= rq_need > rq_max + 10'd1;
                        req_toggle <= ~req_toggle;
                    end
                end
            end
        end
    end

    wire de0  = (h < OUT_W_C) && (v < OUT_H_C);
    wire img0 = (h >= gx0) && (h < gx1) && (v >= gy0) && (v < gy1);
    wire hs0  = (h >= HS_BEGIN) && (h < HS_END);

    // ------------------------------------------------------------------
    // Ступень 0: адреса четырёх точек источника — (sx, sy), (sx+1, sy), (sx, sy+1), (sx+1, sy+1)
    // ------------------------------------------------------------------
    // Буфер — четыре блока (fb_line_ram): A — строки с чётным номером, B — с нечётным;
    // e — чётные точки, o — нечётные. Соседние точки и строки лежат в разных блоках.
    wire [9:0] sx_r  = sx + 10'd1;
    wire [9:0] e_idx = sx[0] ? sx_r : sx;           // чётная из двух точек
    wire [9:0] o_idx = sx[0] ? sx : sx_r;           // нечётная
    wire [9:0] sy_b  = (sy >= glast) ? sy : sy + 10'd1;
    wire [9:0] line_a = sy[0] ? sy_b : sy;          // строка блока A (чётная)
    wire [9:0] line_b = sy[0] ? sy : sy_b;          // строка блока B (нечётная)
    assign lb_ra_e = {line_a[1], e_idx[9:1]};
    assign lb_ra_o = {line_a[1], o_idx[9:1]};
    assign lb_rb_e = {line_b[1], e_idx[9:1]};
    assign lb_rb_o = {line_b[1], o_idx[9:1]};

    // Веса правой и нижней точки, 1/16: доля точки вывода за границей точки источника.
    // Точка вывода занимает [acc, acc+N) в 1/D точки источника; за границу она заходит,
    // если acc + N > D, на (acc + N − D)/N своей ширины.
    // Два шага, чтобы уложиться в такт: доля за границей (acc + N − D, в 1/D) и вес из неё.
    // При уменьшении точка вывода всегда заходит за границу (acc + N > D); вес — по той её
    // части, что за последней границей, двухточечное сглаживание.
    function automatic [6:0] over_part(input [6:0] acc, input [6:0] n, input [6:0] d);
        reg [7:0] over;
        begin
            over = {1'b0, acc} + {1'b0, n};
            if (over >= {d, 1'b0})          over = over - {d, 1'b0};
            else if (over > {1'b0, d})      over = over - {1'b0, d};
            else                            over = 8'd0;
            over_part = over[6:0];
        end
    endfunction
    function automatic [4:0] weight(input [6:0] part, input [16:0] recip);
        reg [24:0] w;
        begin
            w = part * recip + 25'd2048;
            weight = (part == 7'd0) ? 5'd0 : (w[24:12] > 13'd16) ? 5'd16 : w[16:12];
        end
    endfunction
    wire [6:0] px0 = over_part(sx_acc, gnx, gdx);
    // Вес по высоте — на всю строку; считается за такты до первой точки строки
    reg  [6:0] py_r;
    reg  [4:0] wy;
    always_ff @(posedge pixel_clk) begin
        py_r <= over_part(sy_acc, gny, gdy);
        wy   <= weight(py_r, gry);
    end

    // ------------------------------------------------------------------
    // Ступень 1: данные памяти пришли — четыре точки
    // ------------------------------------------------------------------
    reg de1, img1, hs1, vs1, odd_x1, odd_y1;
    reg [6:0]  px1;
    reg [10:0] x1, y1;
    always_ff @(posedge pixel_clk or negedge rst_n) begin
        if (!rst_n) begin
            de1 <= 1'b0; img1 <= 1'b0; hs1 <= 1'b0; vs1 <= 1'b0; odd_x1 <= 1'b0; odd_y1 <= 1'b0;
            px1 <= '0; x1 <= '0; y1 <= '0;
        end else begin
            de1 <= de0; img1 <= img0; hs1 <= hs0; vs1 <= vs_level;
            odd_x1 <= sx[0]; odd_y1 <= sy[0]; px1 <= px0;
            x1 <= {1'b0, sx}; y1 <= {1'b0, sy};
        end
    end

    wire [15:0] top_e = odd_y1 ? lb_qb_e : lb_qa_e, top_o = odd_y1 ? lb_qb_o : lb_qa_o;
    wire [15:0] bot_e = odd_y1 ? lb_qa_e : lb_qb_e, bot_o = odd_y1 ? lb_qa_o : lb_qb_o;
    wire [15:0] p_tl = odd_x1 ? top_o : top_e, p_tr = odd_x1 ? top_e : top_o;
    wire [15:0] p_bl = odd_x1 ? bot_o : bot_e, p_br = odd_x1 ? bot_e : bot_o;
    wire [15:0] pix  = p_tl;                        // точка источника (sx, sy) — для сверки

    // Сверка с испытательной таблицей (полоса движущегося квадрата пропускается)
    localparam [10:0] PAT_W = H_ACTIVE, PAT_H = V_ACTIVE;
    localparam [10:0] BAND0 = FB_PAT_BAND_Y0, BAND1 = FB_PAT_BAND_Y1;
    wire [15:0] pat_exp = fb_pattern_px(x1, y1, 8'd0, PAT_W, PAT_H);
    wire        pat_skip = (y1 >= BAND0) && (y1 < BAND1);
    reg  [15:0] err_cnt;
    always_ff @(posedge pixel_clk or negedge rst_n) begin
        if (!rst_n) begin
            err_cnt <= '0; chk_err <= '0; chk_tick <= 1'b0;
        end else begin
            chk_tick <= 1'b0;
            if (img1 && !gft && !pat_skip && pix != pat_exp && err_cnt != 16'hFFFF)
                err_cnt <= err_cnt + 16'd1;
            // Конец последней строки картинки: итог кадра
            if (h == 12'd0 && v == gy1) begin
                chk_err  <= err_cnt;
                chk_tick <= 1'b1;
                err_cnt  <= '0;
            end
        end
    end

    // Палитра — у каждой из четырёх точек; у кадра FT812 — RGB565 с повтором старших разрядов
    wire [7:0] el_r [0:3], el_g [0:3], el_b [0:3];
    wire [15:0] p4 [0:3];
    assign p4[0] = p_tl; assign p4[1] = p_tr; assign p4[2] = p_bl; assign p4[3] = p_br;
    wire [7:0] tl_r, tl_g, tl_b, tr_r, tr_g, tr_b, bl_r, bl_g, bl_b, br_r, br_g, br_b;
    genvar gk;
    for (gk = 0; gk < 4; gk = gk + 1) begin : g_pal
        legacy_palette pal (.pal_sel(p4[gk][15]), .r5(p4[gk][4:0]), .g5(p4[gk][9:5]),
                            .b5(p4[gk][14:10]), .r8(el_r[gk]), .g8(el_g[gk]), .b8(el_b[gk]));
    end
    function automatic [23:0] rgb565(input [15:0] q);
        rgb565 = {q[15:11], q[15:13], q[10:5], q[10:9], q[4:0], q[4:2]};
    endfunction
    wire [23:0] f_tl = rgb565(p_tl), f_tr = rgb565(p_tr), f_bl = rgb565(p_bl), f_br = rgb565(p_br);
    assign {tl_r, tl_g, tl_b} = gft ? f_tl : {el_r[0], el_g[0], el_b[0]};
    assign {tr_r, tr_g, tr_b} = gft ? f_tr : {el_r[1], el_g[1], el_b[1]};
    assign {bl_r, bl_g, bl_b} = gft ? f_bl : {el_r[2], el_g[2], el_b[2]};
    assign {br_r, br_g, br_b} = gft ? f_br : {el_r[3], el_g[3], el_b[3]};

    // ------------------------------------------------------------------
    // Ступень 2: цвета четырёх точек
    // ------------------------------------------------------------------
    reg de2, img2, hs2, vs2;
    reg [4:0] wx2;
    reg [7:0] c_tl [0:2], c_tr [0:2], c_bl [0:2], c_br [0:2];
    always_ff @(posedge pixel_clk or negedge rst_n) begin
        if (!rst_n) begin
            de2 <= 1'b0; img2 <= 1'b0; hs2 <= 1'b0; vs2 <= 1'b0; wx2 <= '0;
            for (int c = 0; c < 3; c = c + 1) begin
                c_tl[c] <= '0; c_tr[c] <= '0; c_bl[c] <= '0; c_br[c] <= '0;
            end
        end else begin
            de2 <= de1; img2 <= img1; hs2 <= hs1; vs2 <= vs1; wx2 <= weight(px1, grx);
            c_tl[0] <= tl_r; c_tl[1] <= tl_g; c_tl[2] <= tl_b;
            c_tr[0] <= tr_r; c_tr[1] <= tr_g; c_tr[2] <= tr_b;
            c_bl[0] <= bl_r; c_bl[1] <= bl_g; c_bl[2] <= bl_b;
            c_br[0] <= br_r; c_br[1] <= br_g; c_br[2] <= br_b;
        end
    end

    // ------------------------------------------------------------------
    // Ступень 3: по ширине — top = 16·tl + (tr − tl)·wx, так же снизу (0…4080)
    // ------------------------------------------------------------------
    function automatic [11:0] hmix(input [7:0] a, input [7:0] b, input [4:0] w);
        reg signed [9:0]  d;
        reg signed [15:0] m, sum;
        begin
            d    = $signed({2'b00, b}) - $signed({2'b00, a});      // −255…255
            m    = d * $signed({1'b0, w});
            sum  = $signed({4'b0000, a, 4'b0000}) + m;          // 0…4080
            hmix = sum[11:0];
        end
    endfunction

    reg de3, img3, hs3, vs3;
    reg [11:0] c_top [0:2], c_bot [0:2];
    always_ff @(posedge pixel_clk or negedge rst_n) begin
        if (!rst_n) begin
            de3 <= 1'b0; img3 <= 1'b0; hs3 <= 1'b0; vs3 <= 1'b0;
            for (int c = 0; c < 3; c = c + 1) begin c_top[c] <= '0; c_bot[c] <= '0; end
        end else begin
            de3 <= de2; img3 <= img2; hs3 <= hs2; vs3 <= vs2;
            for (int c = 0; c < 3; c = c + 1) begin
                c_top[c] <= hmix(c_tl[c], c_tr[c], wx2);
                c_bot[c] <= hmix(c_bl[c], c_br[c], wx2);
            end
        end
    end

    // ------------------------------------------------------------------
    // Ступень 4: по высоте и выход — (16·top + (bot − top)·wy + 128) / 256
    // ------------------------------------------------------------------
    function automatic [7:0] vmix(input [11:0] top, input [11:0] bot, input [4:0] w);
        reg signed [13:0] d;
        reg signed [19:0] m, sum;
        begin
            d    = $signed({2'b00, bot}) - $signed({2'b00, top});  // −4080…4080
            m    = d * $signed({1'b0, w});
            sum  = $signed({4'b0000, top, 4'b0000}) + m + 20'sd128;   // 0…65408
            vmix = sum[15:8];
        end
    endfunction

    always_ff @(posedge pixel_clk or negedge rst_n) begin
        if (!rst_n) begin
            out_r <= '0; out_g <= '0; out_b <= '0;
            out_de <= 1'b0;
            out_hs <= HS_NEG;
            out_vs <= VS_NEG;
        end else begin
            out_de <= de3;
            out_hs <= hs3 ^ HS_NEG;
            out_vs <= vs3 ^ VS_NEG;
            out_r  <= (img3 && !blank) ? vmix(c_top[0], c_bot[0], wy) : 8'h00;
            out_g  <= (img3 && !blank) ? vmix(c_top[1], c_bot[1], wy) : 8'h00;
            out_b  <= (img3 && !blank) ? vmix(c_top[2], c_bot[2], wy) : 8'h00;
        end
    end
endmodule
