// SPDX-License-Identifier: Apache-2.0
//
// Автоопределение растра Evo и выбор окна захвата. Домен clk_cap (56 МГц = 2 такта на пиксель).
//
// ЧТО ИЗМЕРЯЕТСЯ
//   • полярность HS и VS: активен тот уровень, который короче;
//   • длина строки в тактах clk_cap (между стробами начала строки);
//   • число строк в кадре (стробов строки между стробами кадра);
//   • ширина строчного импульса в тактах — по ней различаются TS-Config и Base Conf.
//   Растр считается захваченным после трёх одинаковых кадров подряд и остаётся таким,
//   пока кадры совпадают. Каждый кадр получает оценку frame_ok: он целиком прошёл
//   в захваченном растре. Кадр, во время которого растр сменился, frame_ok не получает
//   и в кадровый буфер не публикуется.
//
// РАСТРЫ (HDL TS-Labs: current/video/video_sync.v, base/video/video_sync_v.v, video_vga_sync_h.v).
//   Строка VGA — в отсчётах 28 МГц, строк — после удвоения:
//     TS-Config Pentagon      896 × 640, HS 104 отсчёта   48,83 Гц
//     TS-Config PENT_312      896 × 624, HS 104           50,08 Гц
//     Base Conf Pentagon      896 × 640, HS 106           48,83 Гц
//     Base Conf 48k           896 × 624, HS 106           50,08 Гц
//     Base Conf 128k          912 × 622, HS 106           49,36 Гц
//   60 Гц (896 × 524) не поддерживается — выводится как неизвестный растр.
//
// ОКНО ЗАХВАТА (H_OFF — отсчёт в строке, V_OFF — строка от кадрового строба)
//   TS-Config Pentagon: 131 / 45 — прежние расчётные значения генлок-тракта.
//   TS-Config PENT_312: видимая часть начинается на 8 ТВ-строк раньше (VBLNK_END 24 против 32)
//                       → V_OFF = 45 − 16 = 29.
//   Base Conf: картинка на 8 отсчётов дальше от HS, чем у TS-Config (начало вывода удвоителя
//                       156 против 148 отсчётов от фронта HS) → H_OFF = 139; начало видимой части
//                       одинаково для всех трёх растров → V_OFF = 45. У 48k/128k видимых строк
//                       меньше 576 — нижние 16–18 строк окна приходятся на гашение (чёрные).
//   ВСЕ СМЕЩЕНИЯ РАСЧЁТНЫЕ и уточняются на железе; к ним прибавляются поправки h_adj/v_adj,
//   которые задаёт ESP. Неизвестный растр захватывается окном по центру.
//
// АВТОПОДСТРОЙКА ОКНА (auto_align=1)
//   active_bbox сообщает границы не чёрной картинки. Если в этом растре видимая область Evo
//   равна окну захвата, а картинка заняла её целиком (режим 360×288 или цветной бордюр),
//   то начало картинки и есть точное смещение окна. Оно принимается, когда:
//     • ширина найденной картинки ровно H_ACTIVE (для вертикали — высота ровно V_ACTIVE);
//     • то же положение повторилось в трёх кадрах подряд;
//     • отличие от профиля не больше ALIGN_RANGE — защита от случайного содержимого.
//   Видимая область равна окну у TS-Config (оба растра) и Base Conf Pentagon; у Base Conf 48k —
//   только по горизонтали (видимых строк 560); у 128k — ни по одной оси (736 × 558).
//   Чёрный бордюр ничего не сбивает: окно остаётся прежним. Выученное смещение
//   сбрасывается при смене растра.
//
module raster_detect #(
    parameter int  H_ACTIVE = 720,
    parameter int  V_ACTIVE = 576,
    parameter bit  TEST_PROFILES = 1'b0,  // добавить уменьшенные растры тестбенча
    parameter int  ALIGN_RANGE   = 16     // на сколько автоподстройка может уйти от профиля
) (
    input  wire               clk_cap,
    input  wire               rst_n,

    input  wire               hs_level,       // синхронизированные уровни как есть
    input  wire               vs_level,
    input  wire               hs_active,      // нормализованные (из ts_mux_capture)
    input  wire               hs_start,
    input  wire               vs_start,

    input  wire signed [7:0]  h_adj,          // поправки окна от ESP
    input  wire signed [7:0]  v_adj,

    // границы картинки (active_bbox) и автоподстройка по ним
    input  wire               auto_align,
    input  wire               bb_new,
    input  wire               bb_valid,
    input  wire [10:0]        bb_x0,
    input  wire [10:0]        bb_x1,
    input  wire [10:0]        bb_y0,
    input  wire [10:0]        bb_y1,
    output reg                aligned_h,      // окно выставлено по картинке
    output reg                aligned_v,

    output reg                hs_active_low,
    output reg                vs_active_low,
    output reg                locked,
    output reg                frame_ok,       // обновляется на vs_start: прошедший кадр целый
    output reg  [2:0]         mode,           // 0 — неизвестный, см. localparam MODE_*
    output reg  [10:0]        h_off,
    output reg  [10:0]        v_off,
    // измеренное (для OSD/ESP и теста)
    output reg  [11:0]        line_cycles,
    output reg  [10:0]        frame_lines,
    output reg  [11:0]        hs_cycles
);
    localparam [2:0] MODE_UNKNOWN = 3'd0, MODE_TS_PENT = 3'd1, MODE_TS_312 = 3'd2,
                     MODE_BASE_PENT = 3'd3, MODE_BASE_48K = 3'd4, MODE_BASE_128K = 3'd5,
                     MODE_TEST_A = 3'd6, MODE_TEST_B = 3'd7;

    // ------------------------------------------------------------------
    // Полярность: длительности последних уровней
    // ------------------------------------------------------------------
    reg        hs_prev_lv, vs_prev_lv;
    reg [11:0] hs_run, hs_len_hi, hs_len_lo;
    reg [20:0] vs_run, vs_len_hi, vs_len_lo;

    always_ff @(posedge clk_cap or negedge rst_n) begin
        if (!rst_n) begin
            hs_prev_lv <= 1'b1; vs_prev_lv <= 1'b1;
            hs_run <= '0; hs_len_hi <= '0; hs_len_lo <= '0;
            vs_run <= '0; vs_len_hi <= '0; vs_len_lo <= '0;
            hs_active_low <= 1'b1;
            vs_active_low <= 1'b1;
        end else begin
            hs_prev_lv <= hs_level;
            vs_prev_lv <= vs_level;
            if (hs_level != hs_prev_lv) begin
                if (hs_prev_lv) hs_len_hi <= hs_run; else hs_len_lo <= hs_run;
                hs_run <= 12'd1;
            end else if (hs_run != 12'hFFF) hs_run <= hs_run + 12'd1;
            if (vs_level != vs_prev_lv) begin
                if (vs_prev_lv) vs_len_hi <= vs_run; else vs_len_lo <= vs_run;
                vs_run <= 21'd1;
            end else if (vs_run != 21'h1FFFFF) vs_run <= vs_run + 21'd1;
            // Решение — только когда измерены оба уровня
            if (hs_len_hi != 0 && hs_len_lo != 0) hs_active_low <= (hs_len_lo < hs_len_hi);
            if (vs_len_hi != 0 && vs_len_lo != 0) vs_active_low <= (vs_len_lo < vs_len_hi);
        end
    end

    // ------------------------------------------------------------------
    // Измерение строки и кадра
    // ------------------------------------------------------------------
    reg [11:0] cyc;              // тактов с начала строки
    reg [11:0] f_line_min, f_line_max;
    reg [10:0] f_lines;
    reg [11:0] hs_w, f_hs;
    reg        hs_act_d;
    reg        seen_line;
    // подпись захваченного растра и кандидата
    reg [11:0] c_line; reg [10:0] c_lines; reg [11:0] c_hs;
    reg [1:0]  stable;

    function automatic bit near(input [11:0] a, input [11:0] b, input [11:0] tol);
        near = (a > b) ? (a - b <= tol) : (b - a <= tol);
    endfunction

    wire [11:0] this_line = f_line_max;
    wire        lines_even = (f_line_max - f_line_min) <= 12'd2;
    wire        same_as_cand = lines_even && near(this_line, c_line, 12'd2) &&
                               (f_lines == c_lines) && near(f_hs, c_hs, 12'd2);

    always_ff @(posedge clk_cap or negedge rst_n) begin
        if (!rst_n) begin
            cyc <= '0; f_line_min <= 12'hFFF; f_line_max <= '0; f_lines <= '0;
            hs_w <= '0; f_hs <= '0; hs_act_d <= 1'b0; seen_line <= 1'b0;
            c_line <= '0; c_lines <= '0; c_hs <= '0; stable <= '0;
            locked <= 1'b0; frame_ok <= 1'b0;
            line_cycles <= '0; frame_lines <= '0; hs_cycles <= '0;
        end else begin
            cyc <= (cyc == 12'hFFF) ? cyc : cyc + 12'd1;
            hs_act_d <= hs_active;
            if (hs_active) hs_w <= hs_w + 12'd1;
            if (hs_act_d && !hs_active) begin
                f_hs <= hs_w;
                hs_w <= '0;
            end
            if (hs_start) begin
                if (seen_line) begin
                    if (cyc < f_line_min) f_line_min <= cyc;
                    if (cyc > f_line_max) f_line_max <= cyc;
                end
                seen_line <= 1'b1;
                cyc <= 12'd1;
                f_lines <= f_lines + 11'd1;
            end
            if (vs_start) begin
                // Итог кадра. f_lines учитывает строку, начавшуюся одновременно с кадром.
                line_cycles <= this_line;
                frame_lines <= f_lines;
                hs_cycles   <= f_hs;
                frame_ok    <= locked && same_as_cand;
                if (same_as_cand) begin
                    if (stable != 2'd3) stable <= stable + 2'd1;
                    if (stable >= 2'd1) locked <= 1'b1;     // третий одинаковый кадр
                end else begin
                    stable  <= '0;
                    locked  <= 1'b0;
                    c_line  <= this_line;
                    c_lines <= f_lines;
                    c_hs    <= f_hs;
                end
                f_line_min <= 12'hFFF;
                f_line_max <= '0;
                f_lines    <= hs_start ? 11'd1 : 11'd0;
            end
        end
    end

    // ------------------------------------------------------------------
    // Профили: растр → режим и окно
    // ------------------------------------------------------------------
    reg [2:0]  p_mode;
    reg [10:0] p_h, p_v;
    wire [10:0] c_px = c_line[11:1];     // отсчётов (пикселей 28 МГц) в строке
    always_comb begin
        p_mode = MODE_UNKNOWN;
        // неизвестный растр — окно по центру
        p_h = (c_px > H_ACTIVE[10:0]) ? (c_px - H_ACTIVE[10:0]) >> 1 : 11'd0;
        p_v = (c_lines > V_ACTIVE[10:0]) ? (c_lines - V_ACTIVE[10:0]) >> 1 : 11'd0;
        if (near(c_line, 12'd1792, 12'd2) && c_lines == 11'd640) begin
            if (c_hs < 12'd210) begin p_mode = MODE_TS_PENT;   p_h = 11'd131; p_v = 11'd45; end
            else                begin p_mode = MODE_BASE_PENT; p_h = 11'd139; p_v = 11'd45; end
        end else if (near(c_line, 12'd1792, 12'd2) && c_lines == 11'd624) begin
            if (c_hs < 12'd210) begin p_mode = MODE_TS_312;    p_h = 11'd131; p_v = 11'd29; end
            else                begin p_mode = MODE_BASE_48K;  p_h = 11'd139; p_v = 11'd45; end
        end else if (near(c_line, 12'd1824, 12'd2) && c_lines == 11'd622) begin
                                      p_mode = MODE_BASE_128K; p_h = 11'd139; p_v = 11'd45;
        end else if (TEST_PROFILES && near(c_line, 12'd200, 12'd2) && c_lines == 11'd44) begin
                                      p_mode = MODE_TEST_A;    p_h = 11'd19;  p_v = 11'd6;
        end else if (TEST_PROFILES && near(c_line, 12'd208, 12'd2) && c_lines == 11'd42) begin
                                      p_mode = MODE_TEST_B;    p_h = 11'd23;  p_v = 11'd5;
        end
    end

    // Где видимая область Evo совпадает с окном захвата (по горизонтали / по вертикали)
    reg can_h, can_v;
    always_comb begin
        case (p_mode)
            MODE_TS_PENT, MODE_TS_312, MODE_BASE_PENT,
            MODE_TEST_A, MODE_TEST_B:   begin can_h = 1'b1; can_v = 1'b1; end
            MODE_BASE_48K:              begin can_h = 1'b1; can_v = 1'b0; end
            default:                    begin can_h = 1'b0; can_v = 1'b0; end
        endcase
    end

    // ------------------------------------------------------------------
    // Автоподстройка по границам картинки
    // ------------------------------------------------------------------
    reg [10:0] learn_h, learn_v, cand_h, cand_v;
    reg [1:0]  cnt_h, cnt_v;

    function automatic bit in_range(input [10:0] a, input [10:0] b);
        in_range = (a > b) ? (a - b <= ALIGN_RANGE[10:0]) : (b - a <= ALIGN_RANGE[10:0]);
    endfunction

    wire fit_h = bb_valid && (bb_x1 - bb_x0 + 11'd1 == H_ACTIVE[10:0]) && in_range(bb_x0, p_h);
    wire fit_v = bb_valid && (bb_y1 - bb_y0 + 11'd1 == V_ACTIVE[10:0]) && in_range(bb_y0, p_v);

    always_ff @(posedge clk_cap or negedge rst_n) begin
        if (!rst_n) begin
            learn_h <= '0; learn_v <= '0; cand_h <= '0; cand_v <= '0;
            cnt_h <= '0; cnt_v <= '0; aligned_h <= 1'b0; aligned_v <= 1'b0;
        end else if (!locked || !auto_align) begin
            cnt_h <= '0; cnt_v <= '0; aligned_h <= 1'b0; aligned_v <= 1'b0;
        end else if (bb_new) begin
            if (can_h && fit_h) begin
                if (bb_x0 == cand_h) begin
                    if (cnt_h != 2'd3) cnt_h <= cnt_h + 2'd1;
                    if (cnt_h >= 2'd1) begin learn_h <= cand_h; aligned_h <= 1'b1; end
                end else begin
                    cand_h <= bb_x0; cnt_h <= '0;
                end
            end
            if (can_v && fit_v) begin
                if (bb_y0 == cand_v) begin
                    if (cnt_v != 2'd3) cnt_v <= cnt_v + 2'd1;
                    if (cnt_v >= 2'd1) begin learn_v <= cand_v; aligned_v <= 1'b1; end
                end else begin
                    cand_v <= bb_y0; cnt_v <= '0;
                end
            end
        end
    end

    always_ff @(posedge clk_cap or negedge rst_n) begin
        if (!rst_n) begin
            mode <= MODE_UNKNOWN; h_off <= '0; v_off <= '0;
        end else begin
            mode  <= p_mode;
            h_off <= (aligned_h ? learn_h : p_h) + {{3{h_adj[7]}}, h_adj};
            v_off <= (aligned_v ? learn_v : p_v) + {{3{v_adj[7]}}, v_adj};
        end
    end
endmodule
