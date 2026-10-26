// SPDX-License-Identifier: Apache-2.0
//
// Растеризатор FT812, исполнитель (ft_render): задания обходчика (ft_rd_walk) → фрагменты
// строки для ft_rd_pix. Арифметика повторяет модель scripts/ft_render_model.py, а та —
// bt8xxemu (замеры scripts/ft_probe_*.py) поверх eve-emu (alfishe/unreal-ng, MIT).
//
// Контекст исполнителя — цвет, альфа, CLEAR_*, ALPHA_FUNC, трафарет, смешение, COLOR_MASK,
// ножницы, BITMAP_TRANSFORM, PALETTE_SOURCE; SAVE/RESTORE — стек из 4 мест в памяти (по
// 36 бит за такт). Задание сначала готовится (пролёт по строке, умножения преобразования,
// квадраты радиусов), потом, когда конвейер точек пуст от прошлого задания, переносится
// в регистры задания, и по нему идут точки — по одной за такт.
//
// Конвейер точки (P0…P5), один для всех видов:
//   BITMAPS: P0 тексель (sx, sy) → P1 строка·stride → P2 адрес байта → P3 слово SDRAM →
//            P4 кэш (промах — остановка и пакет из SDRAM) и биты текселя → P5 цвет текселя,
//            на выходе — умножение на COLOR_RGB/COLOR_A (·/255 с отбрасыванием).
//   RECTS, POINTS: P0 расстояние по x → P1 его квадрат → P2 сравнения с квадратами
//            радиусов → P4 покрытие (в полосе сглаживания — корень, ~20 тактов остановки) →
//            на выходе альфа COLOR_A·покрытие/256.
//   CLEAR: точки с цветом очистки.
// Кадр идёт плитками 128×8 (ft_rd_walk): у задания — строки плитки [первая, после последней)
// (их режут и ножницы по y), пролёт режется краями плитки. BITMAPS и CLEAR идут свои строки
// подряд, без слива конвейера: пролёт по x тот же, тексель начала следующей строки — начало
// этой + (B, E) преобразования (у модели — тот же ⌊(A·rel + B·rely)/16⌋ с rely + 16).
// RECTS и POINTS — по заданию на строку. Строка плитки едет по стадиям с точкой; точка в
// буфере ft_rd_pix — {строка плитки, x[5:0]}.
//
// Кэш текселей — 512 строк по 32 байта (пакет SDRAM, 16 КБ; до 1.25 — 256, 8 КБ: место дали
// оверлеи ESP, которых в ядре FT812 нет), прямого отображения, метки — в
// BSRAM (чтение — вместе с данными, на стадии P3); сбрасывается в начале кадра. Рампа CMD_GRADIENT ПЗУ (0x200065, 512 байтов, clamp(i − 128)) — своя:
// образа ПЗУ Bridgetek в SDRAM нет.
//
// BILINEAR (с 1.25): точка — за 4 прохода конвейера, тексели (tx + k[0], ty + k[1]), k = 0…3,
// каждый со своим повтором и раскладкой; веса — как у модели (замер bt8xxemu): w11 = fx·fy >> 8,
// (256 − fx − fy + w11, fx − w11, fy − w11, w11); слагаемые ⌊тексель·w / 256⌋ копятся на P5,
// фрагмент уходит на последнем проходе (дальше — как у NEAREST: × COLOR_RGB / COLOR_A).
// С 1.26 — обычно за 2 прохода: в проходе строки (k = 0 и 2) — пара текселей (tx, tx + 1) путём
// пары точек NEAREST (второй тексель — сдвиг байта в окне кэша, своя запись палитры и раскрытие),
// у второго — свой вес и умножители. Пара — когда D = 0 (строка текстуры у строки задания одна),
// без REPEAT по y, формат не меньше байта, источник не в ПЗУ (рампа CMD_GRADIENT — одно слово),
// оба текселя в раскладке и в окне кэша (слово текселя или он и начало следующего слова той же
// строки кэша); иначе — 4 прохода. Первая точка строки — всегда 4 прохода: на ней берётся
// выравнивание строк текстуры.
// Пока не сделано (флаги unsup): REPEAT (как BORDER),
// PALETTED, TEXT8X8/TEXTVGA, BARGRAPH (прозрачные).
//
// Первый CLEAR на ещё пустой плитке с ножницами во всю плитку — без записи: значение
// плитки по умолчанию (impl_*) держит ft_rd_pix, а точки, которых не касались, берут его.
module ft_rd_exec #(
    parameter int TW_LOG = 7,               // плитка: 2^TW_LOG × 2^TH_LOG точек (1024)
    parameter int TH_LOG = 3
) (
    input  wire         clk,
    input  wire         rst_n,
    input  wire         abort,
    input  wire         start,          // такт: новый кадр
    input  wire [9:0]   lines,          // строк в кадре
    input  wire [10:0]  hsize,          // точек в строке (1…1024)
    input  wire         ramg_3mb,

    // очередь заданий
    input  wire         q_v,
    input  wire [135:0] q_data,
    input  wire [34:0]  q_head,         // q_data[34:0] из регистра (вид и слово)
    output wire         q_pop,

    // текстуры: пакет SDRAM (8 слов)
    output reg          tx_req,
    output reg  [20:0]  tx_addr,
    input  wire         tx_ack,
    input  wire         tx_rd_valid,
    input  wire [2:0]   tx_rd_idx,
    input  wire [31:0]  tx_rd_data,

    // фрагменты — в ft_rd_pix
    output wire         f_v,
    output wire [9:0]   f_x,            // точка в буфере плитки: {строка, x[5:0]}
    // {A, R, G, B}; f_raw — битмап: {тексель, COLOR} по 32 разряда, произведение и ⌊/255⌋ берёт
    // ft_rd_pix на такт позже (1.31: в P5 остаются умножение BILINEAR и сумма, умножение на цвет —
    // за регистром стадии A ft_rd_pix; оба умножения подряд стояли на худшем пути clk_mem)
    output wire [63:0]  f_rgba,
    output wire         f_raw,
    output wire         f_slot,
    output wire         f_clr,          // точка CLEAR
    output wire [9:0]   f_xn,           // f_x следующего такта, если конвейер продвинется (P4)
    output wire         f_adv,          // конвейер продвигается
    output wire         f_p2,           // пара: точка f_x + 1 — без смешения (альфа 0 или 255)
    output wire         f_w2,           // её альфа 255: записать f_rgba2 (0 — не трогать)
    output wire [31:0]  f_rgba2,
    output wire         f_p2n,          // у точки следующего такта (f_xn) может быть пара
    input  wire         f_ready,
    output reg          slot_we,
    output reg          slot_sel,
    output reg  [63:0]  slot_data,
    input  wire [1:0]   slot_busy,      // фрагменты этого места ещё в ft_rd_pix
    output reg          impl_we,        // значение строки по умолчанию (буфер cur_buf)
    output reg  [31:0]  impl_rgba,
    output reg  [7:0]   impl_st,
    output reg          cur_buf,
    output reg          tile_go,        // такт: плитка в буфере tile_buf готова
    output reg          tile_buf,
    output reg  [3:0]   tile_x,         // её левая точка / ширину плитки
    output reg  [9:0]   tile_y,         // верхняя строка
    input  wire [1:0]   buf_free,
    input  wire         be_empty,

    output reg          busy,           // кадр идёт
    output reg  [3:0]   unsup           // липкие: 0 REPEAT, 1 BILINEAR, 2 формат
);
    localparam logic [2:0] K_STATE = 3'd0, K_BMP = 3'd1, K_RECT = 3'd2, K_POINT = 3'd3,
                           K_EOL = 3'd4;
    localparam logic [1:0] J_BMP = 2'd0, J_RECT = 2'd1, J_POINT = 2'd2, J_CLR = 2'd3;

    // =========================================================================================
    // Контекст: один вектор, поля — срезы
    // =========================================================================================
    localparam int O_CRGB = 0,   O_CA = 24,   O_CST = 32,  O_RGB = 40,  O_A = 64;
    localparam int O_AF = 72,    O_AREF = 75, O_SF = 83,   O_SREF = 86, O_SMASK = 94;
    localparam int O_SWM = 102,  O_SFAIL = 110, O_SPASS = 113, O_BSRC = 116, O_BDST = 119;
    localparam int O_SCX = 122,  O_SCY = 133, O_SCW = 144, O_SCH = 156, O_CM = 168;
    localparam int O_TA = 172,   O_TB = 189,  O_TC = 206,  O_TD = 230,  O_TE = 247, O_TF = 264;
    localparam int O_PAL = 288,  CXW = 310;
    localparam int CX_WORDS = 9;                         // 9 × 36 бит в стеке

    function automatic logic [CXW-1:0] cx_default();
        logic [CXW-1:0] c;
        c = '0;
        c[O_RGB +: 24] = 24'hFFFFFF;  c[O_A +: 8] = 8'd255;
        c[O_AF +: 3] = 3'd7;          c[O_SF +: 3] = 3'd7;
        c[O_SMASK +: 8] = 8'hFF;      c[O_SWM +: 8] = 8'hFF;
        c[O_SFAIL +: 3] = 3'd1;       c[O_SPASS +: 3] = 3'd1;
        c[O_BSRC +: 3] = 3'd2;        c[O_BDST +: 3] = 3'd4;
        c[O_SCW +: 12] = 12'd2048;    c[O_SCH +: 12] = 12'd2048;
        c[O_CM +: 4] = 4'hF;
        c[O_TA +: 17] = 17'd256;      c[O_TE +: 17] = 17'd256;
        cx_default = c;
    endfunction

    reg  [CXW-1:0] cx;
    wire [23:0] c_rgb   = cx[O_RGB +: 24];
    wire [7:0]  c_a     = cx[O_A +: 8];
    wire [10:0] c_scx   = cx[O_SCX +: 11];
    wire [10:0] c_scy   = cx[O_SCY +: 11];
    wire [11:0] c_scw   = cx[O_SCW +: 12];
    wire [11:0] c_sch   = cx[O_SCH +: 12];
    wire [3:0]  c_cm    = cx[O_CM +: 4];
    wire signed [16:0] c_ta = cx[O_TA +: 17], c_tb = cx[O_TB +: 17];
    wire signed [16:0] c_td = cx[O_TD +: 17], c_te = cx[O_TE +: 17];
    wire signed [23:0] c_tc = cx[O_TC +: 24], c_tf = cx[O_TF +: 24];

    // стек SAVE/RESTORE: 4 места × 9 слов по 36 бит, по кругу (BSRAM: чтение — по регистру адреса)
    (* ram_style = "block" *) reg [35:0] sv_mem [0:63];
    reg  [35:0] sv_q;
    reg  [5:0]  sv_raddr;
    reg  [1:0]  sv_sp;
    reg  [2:0]  sv_cnt;
    reg  [3:0]  sv_k;
    wire [36*CX_WORDS-1:0] cx_ext = {{(36*CX_WORDS-CXW){1'b0}}, cx};
    wire        sv_we;
    wire [5:0]  sv_waddr = {sv_sp, sv_k};
    wire [35:0] sv_wdata = cx_ext[36 * sv_k +: 36];
    always_ff @(posedge clk) begin
        sv_q <= sv_mem[sv_raddr];
        if (sv_we) sv_mem[sv_waddr] <= sv_wdata;
    end

    // =========================================================================================
    // Состояние
    // =========================================================================================
    typedef enum logic [3:0] {
        X_IDLE, X_TILE, X_FETCH, X_SAVE, X_REST, X_SET0, X_SET0B, X_SET0C, X_SET1, X_SET2, X_SET3,
        X_XFER,
        X_RUN, X_EOL, X_SET4, X_SET5
    } xstate_t;
    xstate_t xs_q;
    assign sv_we = xs_q == X_SAVE;

    reg  [9:0]  y, n_lines;             // y — строка задания: верх плитки + строка в плитке
    reg  [9:0]  tx0, ty0;               // плитка: левая точка, верхняя строка
    reg  [10:0] h_n;                    // точек в строке
    reg         untouched;              // на плитке ещё ничего не рисовалось
    reg  [31:0] impl_c;                 // значение по умолчанию текущей плитки
    reg  [7:0]  impl_s;
    reg         slot_nx;                // место для следующего задания
    reg  [135:0] it;                    // задание в подготовке
    reg  [4:0]  it_r0, it_r1;           // его строки плитки [it_r0, it_r1) с ножницами по y
    reg         it_rall;                // это все строки плитки
    reg         n_tfull;                // пролёт — во всю плитку (CLEAR без записи)
    reg  [3:0]  j_row;                  // строка плитки, которая выдаётся
    reg  [4:0]  j_rend;                 // после последней строки задания
    reg  [2:0]  clr_mask;

    // плитка: строк в ней, конец по x, последняя ли в полосе и в кадре (порядок — как у ft_rd_walk)
    wire [10:0] t_left   = {1'b0, n_lines} - {1'b0, ty0};
    localparam int TW = 1 << TW_LOG, TH = 1 << TH_LOG;
    wire [4:0]  t_rows   = t_left >= 11'(TH) ? 5'(TH) : t_left[4:0];
    wire [10:0] t_xw     = {1'b0, tx0} + 11'(TW);
    wire [10:0] t_x1     = t_xw > h_n ? h_n : t_xw;
    wire        t_last_x = t_xw >= h_n;
    wire        t_last_y = {1'b0, ty0} + 11'(TH) >= {1'b0, n_lines};

    wire [20:0] y16 = {7'd0, y, 4'd0};
    wire [12:0] sc_x1 = {2'b0, c_scx} + {1'b0, c_scw};
    // строки задания в выборке (X_FETCH): [первая, после последней) ∩ ножницы по y; контекст в
    // этом такте — уже тот, что для задания (слова состояния прошли раньше)
    wire [3:0]  q_rlo = q_data[130:127];
    wire [4:0]  q_rhi = q_data[135:131];
    wire signed [13:0] q_sc0 = $signed({3'b0, c_scy}) - $signed({4'b0, ty0});
    wire signed [13:0] q_sc1 = $signed({1'b0, {2'b0, c_scy} + {1'b0, c_sch}}) - $signed({4'b0, ty0});
    wire [4:0]  q_r0 = q_sc0 > $signed({10'd0, q_rlo}) ? (q_sc0 > 14'(TH) ? 5'(TH) : q_sc0[4:0]) : {1'b0, q_rlo};
    wire [4:0]  q_r1 = q_sc1 < $signed({9'd0, q_rhi}) ? (q_sc1 < 14'sd0 ? 5'd0 : q_sc1[4:0]) : q_rhi;
    // Ножницы по X (вместе с краями плитки) — заранее, регистрами: контекст меняется в X_FETCH
    // (слово состояния) или X_REST, а читается в X_SET0C не раньше чем через два такта; так
    // сумматор конца ножниц не стоит на пути обрезки пролёта.
    reg  [10:0] scx_r;                  // max(начало ножниц, левый край плитки)
    reg  [12:0] scend_r;                // min(конец ножниц, правый край плитки, ширина кадра)
    always_ff @(posedge clk) begin
        scx_r   <= c_scx > {1'b0, tx0} ? c_scx : {1'b0, tx0};
        scend_r <= sc_x1 > {2'b0, t_x1} ? {2'b0, t_x1} : sc_x1;
    end

    // обрезка пролёта [first, last) ножницами и строкой; first/last — точки, со знаком
    function automatic logic [21:0] clip(input logic signed [20:0] first,
                                         input logic signed [20:0] last,
                                         input logic [10:0] scx, input logic [12:0] scx1,
                                         input logic [10:0] hn);
        logic signed [20:0] a, b;
        a = first;
        if (a < $signed({10'd0, scx})) a = $signed({10'd0, scx});
        if (a < 0) a = 0;
        b = last;
        if (b > $signed({8'd0, scx1})) b = $signed({8'd0, scx1});
        if (b > $signed({10'd0, hn})) b = $signed({10'd0, hn});
        if (b < a) b = a;
        clip = {a[10:0], b[10:0]};
    endfunction

    // Покрытие по оси (1/256): lo = a − r − p, hi = b + r − p — края фигуры [a − r, b + r]
    // относительно центра точки p (1/16), s = min(r, 16). В два шага (такт): перекрытие
    // полосы точки (16) с фигурой — не больше 16 — и признак «фигура целиком в полосе»;
    // затем покрытие: длина × 16 или перекрытие × s (умножение 5 × 5 бит).
    function automatic logic [5:0] axis_ov(input logic signed [21:0] lo, input logic signed [21:0] hi);
        logic signed [21:0] l, h, ov;
        logic [4:0] o5;
        h  = hi < 22'sd8 ? hi : 22'sd8;
        l  = lo > -22'sd8 ? lo : -22'sd8;
        ov = h - l;
        o5 = ov <= 0 ? 5'd0 : (ov >= 22'sd16 ? 5'd16 : ov[4:0]);
        axis_ov = {lo >= -22'sd8 && hi <= 22'sd8, o5};
    endfunction
    function automatic logic [8:0] axis_cov2(input logic [5:0] io, input logic [4:0] s);
        logic [9:0] m;
        m = io[5] ? {1'b0, io[4:0], 4'd0} : io[4:0] * s;
        axis_cov2 = m > 10'd256 ? 9'd256 : m[8:0];
    endfunction

    // ⌈4096 / s⌉ для s = 1…16 — порог полного покрытия угла
    function automatic logic [12:0] vfull(input logic [4:0] s);
        case (s)
            5'd1: vfull = 13'd4096;  5'd2: vfull = 13'd2048;  5'd3: vfull = 13'd1366;
            5'd4: vfull = 13'd1024;  5'd5: vfull = 13'd820;   5'd6: vfull = 13'd683;
            5'd7: vfull = 13'd586;   5'd8: vfull = 13'd512;   5'd9: vfull = 13'd456;
            5'd10: vfull = 13'd410;  5'd11: vfull = 13'd373;  5'd12: vfull = 13'd342;
            5'd13: vfull = 13'd316;  5'd14: vfull = 13'd293;  5'd15: vfull = 13'd274;
            default: vfull = 13'd256;
        endcase
    endfunction

    // =========================================================================================
    // Подготовка задания (n_*) и регистры задания (j_*)
    // =========================================================================================
    reg  [1:0]  n_kind;
    reg  [10:0] n_xs, n_xe;
    reg         n_skip;
    reg  signed [17:0] n_rel, n_rely;
    reg  signed [35:0] m0, m1, m2;      // произведения подготовки (m0, m2 — суммы двух)
    reg  [35:0] mk;
    reg  [22:0] m_sl;                   // stride·высота раскладки
    reg  [29:0] m_cell;
    reg  signed [32:0] n_sx, n_sy;
    reg  [13:0] n_dy;                   // RECT: расстояние строки до ядра; POINT: |dy|
    reg         n_dy0;                  // POINT: dy = 0
    reg         n_corner;               // RECT: строка угла
    reg  [8:0]  n_cy;
    reg  [4:0]  n_s;
    reg  signed [17:0] n_k1;            // K + 1 (K — порог полного покрытия)
    reg  [13:0] n_r8, n_o;              // r + 8, r + 16
    reg  signed [20:0] n_lo, n_hi;      // пролёт до округления (1/16)
    reg  signed [20:0] n_first, n_last; // пролёт в точках до ножниц
    reg  signed [21:0] n_cylo, n_cyhi;  // RECT: края ядра по y с радиусом относительно строки
    reg  [5:0]  n_cyio;                 // их перекрытие с полосой строки

    reg  [1:0]  j_kind;
    reg  [10:0] j_x, j_xe, j_xs;        // точка, конец и начало пролёта
    reg  signed [32:0] j_sx, j_sy;
    reg  signed [32:0] j_sx0, j_sy0;    // тексель начала пролёта этой строки
    reg  signed [16:0] j_a, j_d, j_b, j_e;
    reg  [21:0] j_base;
    reg  [11:0] j_stride;
    reg  [14:0] j_lw;                   // ширина раскладки в текселях
    reg  [10:0] j_lh;
    reg  [4:0]  j_fmt;
    reg  [4:0]  j_dfmt;                 // формат раскрытия каналов (PALETTED565/4444 — как 565/4444)
    reg         j_p8;                   // PALETTED8
    reg  signed [20:0] j_x0, j_x1;      // RECT: ядро по x; POINT: j_x0 = vx
    reg  [11:0] j_r;
    reg  [13:0] j_r8, j_o;
    reg  [27:0] j_dy2;
    reg  [27:0] j_r8sq, j_osq;
    reg  [27:0] j_kq;                   // ⌊(K+1)² − 1)/256⌋: полное покрытие при q ≤ j_kq
    reg         j_kok;                  // K ≥ 0: полное покрытие возможно
    reg         j_rz;                   // радиус 0: покрытие угла/точки 0
    reg         j_corner, j_dy0;
    reg  [8:0]  j_cy;
    reg  [5:0]  j_cyio;                 // RECT r < 16: {целиком, перекрытие} строки
    reg         j_thin;                 // RECT: r < 16 — без скругления
    reg  [4:0]  j_s;
    reg         j_wx, j_wy;             // REPEAT по x / y
    reg  [14:0] j_lwm;                  // ширина раскладки − 1 (маска REPEAT)
    reg  [10:0] j_lhm;
    reg  [21:0] j_pal;                  // PALETTE_SOURCE задания
    reg  [23:0] j_rgb;                  // COLOR_RGB (у CLEAR — цвет очистки)
    reg  [7:0]  j_ca;                   // COLOR_A (у CLEAR — CLEAR_COLOR_A)
    reg         j_slot;

    // =========================================================================================
    // Конвейер точек
    // =========================================================================================
    reg         p0_v, p1_v, p2_v, p3_v, p4_v, p5_v;
    reg  [9:0]  p0_x, p1_x, p2_x, p3_x, p4_x, p5_x;
    reg  [3:0]  p0_r, p1_r, p2_r, p3_r, p4_r, p5_r;  // строка плитки точки
    // BITMAPS
    reg         p0_in, p1_in, p2_in, p3_in, p4_in;
    reg  [10:0] p0_ty;
    reg  [11:0] p0_col;
    reg  [2:0]  p0_sub, p1_sub, p2_sub, p3_sub, p4_sub;
    reg  [21:0] p1_bc;
    reg  [22:0] p1_ro;                  // строка·stride
    // пара (две точки за такт): задание годно (j_dual), на стадии — пара и сдвиг байта текселя
    // второй точки от первой (−2…2)
    reg         p0_p2, p1_p2, p2_p2, p3_p2, p4_p2, q5_p2, q6_p2, p5_p2;
    reg  signed [2:0] p0_d2, p1_d2, p2_d2, p3_d2, p4_d2;
    reg  [15:0] q5_raw2, q6_raw2;
    reg  [19:0] q5_pw2, q6_pw2;
    reg         q5_ph2, q6_ph2;
    reg         q6_hit2;
    reg  [31:0] p5_t2;
    // BILINEAR: вес и проход на стадиях
    reg  [8:0]  p0_w, p1_w, p2_w, p3_w, p4_w, q5_w, q6_w;
    reg  [1:0]  p0_k, p1_k, p2_k, p3_k, p4_k, q5_k, q6_k, p5_k;
    reg  [8:0]  p0_w2, p1_w2, p2_w2, p3_w2, p4_w2, q5_w2, q6_w2;   // вес второго текселя пары (0 — нет)
    reg  [1:0]  p3_ew, p4_ew;           // слово чётной половины кэша у слова стадии
    reg  [21:0] p2_a, p3_a, p4_a;
    reg         p2_oob, p3_oob, p4_oob; // вне RAM_G и ПЗУ: тексель 0
    reg         p2_ramp, p3_ramp, p4_ramp;
    reg         p2_rom;
    reg  [20:0] p3_wa, p4_wa;
    reg  [31:0] p5_t;                   // тексель RGBA {A, R, G, B}
    // RECTS/POINTS
    reg         p0_cn, p1_cn, p2_cn, p3_cn, p4_cn;  // угол / точка: покрытие по расстоянию
    reg  [13:0] p0_ix;
    reg         p0_ax, p1_ax;           // POINT: на оси x на расстоянии r + 16 — не фрагмент
    reg  [5:0]  p1_aio;                 // перекрытие по оси (P1)
    reg  [8:0]  p2_cnc, p3_cnc, p4_cnc; // покрытие вне угла (с P2)
    reg  [27:0] p1_ix2;
    reg  [28:0] p2_q, p3_q, p4_q;
    reg         p2_frag, p3_frag, p4_frag;
    reg  [1:0]  p2_cls, p3_cls, p4_cls; // 0 — нуль, 1 — полное, 2 — полоса
    reg  [8:0]  p2_m;                   // RECT r < 16: ox·oy
    reg         p2_k16;                 // RECT r < 16: по одной из осей фигура целиком в точке
    reg  [12:0] p3_num, p4_num;         // покрытие фигуры, 1/4096
    // Q5 — адрес записи палитры; Q6 — запись палитры (кэш) и раскрытие каналов
    reg         q5_v, q6_v;
    reg  [9:0]  q5_x, q6_x;
    reg  [3:0]  q5_r, q6_r;
    reg  [15:0] q5_raw, q6_raw;         // разряды текселя (у PALETTED — индекс)
    reg         q5_inok, q6_inok;       // тексель в раскладке и в RAM_G
    reg  [19:0] q5_pw, q6_pw;           // слово записи палитры
    reg         q5_ph, q6_ph;           // её половина (565/4444)
    reg  [12:0] q5_num, q6_num;
    reg         q6_hit;                 // запись палитры в кэше (на входе в Q6)
    reg  [12:0] p5_num;                 // покрытие фигуры на выходе, 1/4096
    reg         p5_kind_bmp, p5_kind_clr;

    wire        k_bmp   = j_kind == J_BMP;
    wire        k_pal   = j_fmt == 5'd14 || j_fmt == 5'd15 || j_fmt == 5'd16;   // PALETTED565/4444/8
    wire        k_shape = j_kind == J_RECT || j_kind == J_POINT;

    // --- кэш текселей: 512 строк по 8 слов --------------------------------------------------------
    // Две половины по чётности слова строки (c_me — слова 0, 2, 4, 6, c_mo — 1, 3, 5, 7): за такт
    // читаются слово P3/P4 и следующее (у слова 7 — слово 0 той же строки, пара его не берёт).
    (* ram_style = "block" *) reg [31:0] c_me [0:2047];
    (* ram_style = "block" *) reg [31:0] c_mo [0:2047];
    reg  [31:0] cqe, cqo;
    // метка строки — {действительна, адрес строки [17:9]} (индекс — [8:0] с примесью [16:9],
    // так что по метке и индексу строка однозначна); копия — проверка строки предвыборки.
    // Обе — в BSRAM: читаются, как и данные, по адресу стадии P3 (при остановке — P4) и
    // приходят на такт позже — к самой стадии P4.
    (* ram_style = "block" *) reg [9:0] c_tag [0:511];
    (* ram_style = "block" *) reg [9:0] c_tag2 [0:511];
    reg  [9:0]  p4_tag, t2_tag;
    reg  [17:0] t2_rline;               // строка, чья метка сейчас в t2_tag
    // индекс строки кэша — младшие разряды адреса строки с примесью следующих: тайлы, лежащие
    // через размер кэша, иначе попадают в одни индексы и вытесняют друг друга (карта HMM2 с
    // предвыборкой начала пролёта, кэш 64 строки: 37,1 мс без примеси, 26,9 мс с ней)
    function automatic logic [8:0] cidx(input logic [17:0] ln);
        cidx = ln[8:0] ^ {1'b0, ln[16:9]};
    endfunction
    wire [8:0]  p4_idx = cidx(p4_wa[20:3]);
    wire        c_hit  = p4_tag[9] && p4_tag[8:0] == p4_wa[20:12];
    wire        p4_need = p4_v && k_bmp && p4_in && !p4_oob && !p4_ramp;
    wire        p4_miss = p4_need && !c_hit;

    // --- кэш палитры: 64 строки по 32 Б (как кэш текселей), строка — 8 слов ------------------
    reg  [31:0] pl_mem [0:511];
    reg  [31:0] plq;
    reg  [11:0] pl_tag [0:63];          // {действительна, адрес строки >> 6}
    wire [11:0] q5_tag  = pl_tag[q5_pw[8:3]];
    wire        q5_hit  = q5_tag[11] && q5_tag[10:0] == q5_pw[19:9];
    wire [11:0] q5_tag2 = pl_tag[q5_pw2[8:3]];
    wire        q5_hit2 = q5_tag2[11] && q5_tag2[10:0] == q5_pw2[19:9];
    wire        q6_miss2 = q6_v && q6_p2 && k_bmp && k_pal && q6_inok && !q6_hit2;
    wire        q6_miss = (q6_v && k_bmp && k_pal && q6_inok && !q6_hit) || q6_miss2;
    // строка палитры заказа: первой точки, если у неё промах, иначе второй
    wire [16:0] q6_pl  = (q6_v && k_bmp && k_pal && q6_inok && !q6_hit) ? q6_pw[19:3] : q6_pw2[19:3];

    // Заполнение: пакет из SDRAM. Мест два: заявки подаются по порядку мест, слова приходят в
    // том же порядке (мост SDRAM, своя очередь текселей), так что слова — старшему месту. Пока
    // одна строка едет, вторая уже заказана (предвыборка или промах). Порядок: промах палитры
    // (Q6), промах текселя (P4), предвыборка. Строку не заказать, если она уже едет или только
    // что пришла (метка BSRAM читается на такт позже), и если её индекс кэша занят другой едущей
    // строкой (метку и слова индекса пишет одно заполнение).
    reg         fs_v   [0:1];           // место занято (данные ещё не все)
    reg         fs_pal [0:1];           // строка палитры
    reg  [17:0] fs_line [0:1];          // адрес строки (слово >> 3)
    reg         fs_d;                   // место, чьи слова придут следующими
    reg         fs_s;                   // место, чья заявка подаётся (tx_req)
    reg         fs_a;                   // место для следующей заявки
    reg  [1:0]  fs_ns;                  // заказано и не подано
    reg  [17:0] rc_line;                // пришедшая в последние три такта (у палитры — тоже):
    reg  [1:0]  rc_n;                   // строки приходят не чаще раза в 8 тактов — хватает одной
    // строка пришла (последнее слово записано) — метка пишется тактом позже: память палитры
    // (и слово 7 текселей) к чтению в такт метки уже записана, как у прежнего F_DONE
    reg         dn_v, dn_pal;
    reg  [17:0] dn_line;
    // предвыборка: точка P4 попала в новую строку L — строку L + 1 загрузить заранее, если её нет
    reg         pf_v;
    reg  [17:0] pf_line, pf_src;        // адрес строки (слово >> 3)
    reg         sp_v;                   // заявка начала пролёта (от подготовки)
    reg  [17:0] sp_line;
    reg         nr_v;                   // строка той же точки строкой плитки ниже (P2N)
    reg  [17:0] nr_line;
    wire [17:0] t2_line = sp_v ? sp_line : nr_v ? nr_line : pf_line;
    wire        t2_ok   = t2_rline == t2_line;          // метка прочитана для этой строки
    wire        t2_hit  = t2_tag[9] && t2_tag[8:0] == t2_line[17:9];
    // очистка меток в начале кадра (RAM_G могла измениться): 256 тактов, плитки ждут
    reg  [9:0]  clr_n;
    wire        clr_on  = clr_n != 10'd0;
    // строка ln: едет / пришла только что / её индекс занят другой едущей строкой
    function automatic logic f_busy(input logic [17:0] ln, input logic pal);
        f_busy = 1'b0;
        for (int k = 0; k < 2; k++) begin
            if (fs_v[k] && fs_pal[k] == pal && fs_line[k] == ln) f_busy = 1'b1;
            if (fs_v[k] && !fs_pal[k] && !pal && cidx(fs_line[k]) == cidx(ln)) f_busy = 1'b1;
        end
        if (rc_n != 2'd0 && rc_line == ln) f_busy = 1'b1;
    endfunction
    wire [17:0] p4_line  = p4_wa[20:3];
    wire        fs_free  = !fs_v[fs_a];
    wire        fs_done  = tx_rd_valid && tx_rd_idx == 3'd7;        // последнее слово старшего места
    // заказ в этом такте: палитра, промах текселя, предвыборка (не в такт метки пришедшей
    // строки — у меток один порт)
    // предвыборка решена тактом раньше (pf_q): место и сброс метки — в этом такте, прочим ждать
    reg         pf_q;
    reg  [17:0] pf_lq;
    wire        f_ok_any = !abort && !clr_on && fs_free && !dn_v && !pf_q;
    // кандидат по старшинству — одна проверка занятости
    // (занятость считается для каждого сразу, промахи только выбирают — промах P4 идёт с выхода
    // меток, и сравнения за ним были бы длинным путём)
    wire        fb_pal   = f_busy({1'b0, q6_pl}, 1'b1);
    wire        fb_dem   = f_busy(p4_line, 1'b0);
    wire        fb_t2    = f_busy(t2_line, 1'b0);
    wire        fc_busy  = q6_miss ? fb_pal : p4_miss ? fb_dem : fb_t2;
    wire        f_go_pal = f_ok_any && q6_miss && !fc_busy;
    wire        f_go_dem = f_ok_any && !q6_miss && p4_miss && !fc_busy;
    // решение о предвыборке: метка в следующем такте свободна (строка не приходит — fs_done)
    wire        f_go_pfq = !abort && !clr_on && !q6_miss && !p4_miss && !pf_q && (sp_v || nr_v || pf_v) && t2_ok &&
                           (t2_hit || (fs_free && !dn_v && !(fs_done && fs_v[fs_d]) && !fc_busy));
    wire        f_pf_dec = f_go_pfq && !t2_hit;
    wire        f_go_pf  = pf_q && !abort && !clr_on;   // заказ — тактом позже решения
    wire        f_dpal   = fs_pal[fs_d];
    wire [17:0] f_dline  = fs_line[fs_d];
    wire        c_we    = tx_rd_valid && fs_v[fs_d] && !f_dpal;
    wire        pl_we   = tx_rd_valid && fs_v[fs_d] && f_dpal;
    wire [8:0]  tx_idx  = cidx(f_dline);
    wire [11:0] c_waddr = {tx_idx, tx_rd_idx};
    wire [8:0]  pl_waddr = {f_dline[5:0], tx_rd_idx};
    wire        tag_we  = dn_v && !dn_pal;
    wire        pl_tag_we = dn_v && dn_pal;

    // --- корень полосы сглаживания -------------------------------------------------------------
    reg         sq_run, sq_done;
    reg  [4:0]  sq_i;
    reg  [37:0] sq_n;                   // подкоренное, сдвигается
    reg  [19:0] sq_rem;
    reg  [18:0] sq_root;
    wire        p4_band = p4_v && k_shape && p4_cn && p4_frag && p4_cls == 2'd2;

    // --- остановка и продвижение --------------------------------------------------------------
    // пара на P5 разделяется (вторая с частичной альфой): такт стоим (sp_need — у выхода)
    reg         j_bil;                  // BITMAPS с BILINEAR: точка — за 4 прохода (или 2)
    reg         j_bilp;                 // у задания BILINEAR бывают пары текселей
    reg  [1:0]  j_k;                    // проход выдаваемой точки
    // пара текселей у точки в проходе верхней (bq_t) и нижней (bq_b) строки — из регистров,
    // считаются на последнем проходе прошлой точки; выравнивание строк — на первой точке строки
    reg         bq_t, bq_b;
    reg  [4:0]  bq_rt, bq_rb;           // байт начала строки текстуры в строке кэша (верх, низ)
    reg         p0_bt, p1_bt;           // проход k = 0 первой точки строки: на P1 — bq_rt, bq_rb
    wire        bp_now = j_bil && (j_k == 2'd0 ? bq_t : j_k == 2'd2 && bq_b);
    wire        b_last = !j_bil || j_k == 2'd3 || (j_k == 2'd2 && bq_b);   // последний проход — шаг j_x
    reg  sp2_q;
    wire sp_need_w = p5_v && p5_p2 && !j_bil && !(p5_t2[31:24] == 8'd0 || p5_t2[31:24] == 8'd255) && !sp2_q;
    // фрагмент на выходе: у BILINEAR — только последний проход точки
    wire p5_emit = p5_v && (!j_bil || p5_k == 2'd3 || (p5_k == 2'd2 && p5_p2));
    wire p4_stall = p4_miss || (p4_band && !sq_done);
    wire fe_adv   = !p4_stall && !q6_miss && !(p5_emit && !f_ready) && !sp_need_w;
    wire front_empty = !p0_v && !p1_v && !p2_v && !p3_v && !p4_v && !q5_v && !q6_v && !p5_v;

    // чтение кэша: адрес — стадия P3, при остановке — P4 (данные для неё после заполнения);
    // метка — по тому же индексу в том же такте, так что метка и слово у P4 всегда — одной поры
    wire [8:0]  c_ridx  = fe_adv ? cidx(p3_wa[20:3]) : p4_idx;
    // слово в половине: нечётной — wa[2:1], чётной — (wa + 1)[2:1] у нечётного wa (p3_ew, p4_ew)
    wire [1:0]  c_rwo   = fe_adv ? p3_wa[2:1] : p4_wa[2:1];
    wire [1:0]  c_rwe   = fe_adv ? p3_ew : p4_ew;
    // один порт записи меток: очистка, готовая строка, сброс строки перед заполнением
    // (пока пишется, чтение конвейера по её индексу — промах, а не смесь старых и новых слов;
    // сброс опережает первое слово заполнения не меньше чем на такт)
    reg         t_we;
    reg  [8:0]  t_wa;
    reg  [9:0]  t_wd;
    // Адрес и данные — только от регистров (и когда записи нет): предвыборка заказывается
    // только при pf_q, промах — только без него (f_ok_any), так что выход меток (промах P4)
    // идёт лишь в разрешение записи
    always_comb begin
        t_wa = clr_on ? clr_n[8:0] - 9'd1 : tag_we ? cidx(dn_line) : pf_q ? cidx(pf_lq) : p4_idx;
        t_wd = (!clr_on && tag_we) ? {1'b1, dn_line[17:9]} : 10'd0;
        t_we = clr_on || tag_we || f_go_dem || f_go_pf;
    end
    always_ff @(posedge clk) begin
        cqe      <= c_me[{c_ridx, c_rwe}];
        cqo      <= c_mo[{c_ridx, c_rwo}];
        p4_tag   <= c_tag[c_ridx];
        t2_tag   <= c_tag2[cidx(t2_line)];
        t2_rline <= t2_line;
        if (c_we && !c_waddr[0]) c_me[c_waddr[11:1]] <= tx_rd_data;
        if (c_we && c_waddr[0])  c_mo[c_waddr[11:1]] <= tx_rd_data;
        if (t_we) begin
            c_tag[t_wa]  <= t_wd;
            c_tag2[t_wa] <= t_wd;
        end
    end
    // палитра: адрес — Q5, при остановке — Q6
    wire [8:0]  pl_raddr = fe_adv ? q5_pw[8:0] : q6_pw[8:0];
    wire [8:0]  pl_raddr2 = fe_adv ? q5_pw2[8:0] : q6_pw2[8:0];
    reg  [31:0] plq2;
    always_ff @(posedge clk) begin
        plq <= pl_mem[pl_raddr];
        plq2 <= pl_mem[pl_raddr2];
        if (pl_we) pl_mem[pl_waddr] <= tx_rd_data;
        if (clr_on)         pl_tag[clr_n[5:0] - 6'd1] <= '0;
        else if (pl_tag_we) pl_tag[dn_line[5:0]] <= {1'b1, dn_line[16:6]};
    end

    // =========================================================================================
    // Главный автомат
    // =========================================================================================
    wire [2:0]  q_kind = q_head[2:0];
    wire [31:0] q_word = q_head[34:3];
    assign q_pop = xs_q == X_FETCH && q_v && !abort;

    // выдача точки: перебор пролёта
    // задание выдаёт точки, пока j_run; автомат тем временем готовит следующее
    reg  j_run;
    wire issue = j_run && fe_adv && j_x < j_xe;
    // Пара: задание BITMAPS без смешения (j_dual: NEAREST, D = 0, |A| ≤ 256, тексель не меньше
    // байта, COLOR белый и 255, смешение SRC_ALPHA/ONE_MINUS_SRC_ALPHA, ALPHA_FUNC ALWAYS,
    // трафарет не пишется, COLOR_MASK 1111), x чётный и точка x + 1 в пролёте, оба текселя в
    // раскладке и в одном 32-битном слове (строка текстуры у пары одна: D = 0; байт в слове —
    // по младшим разрядам начала строки base + ty·stride и столбца). Тогда тексель × 255 и
    // смешение с альфой 255 или 0 дают сам тексель или прежнюю точку — без умножителей.
    reg         j_dual;
    reg  signed [17:0] j_a2;            // 2·A
    // Решение о паре — из регистров (короткий путь в шаг j_x/j_sx того же такта). При D = 0 строка
    // текстуры у всей строки задания одна: «y в раскладке» и выравнивание её начала (p2_yin, p2_rb)
    // ставятся на первой точке строки (на ней пары нет — p2_first); «x в раскладке» и «tx < ширина
    // − 1» следующей точки (p2_xin, p2_lt) считаются заранее для обоих шагов (+A, +2A).
    reg         p2_first, p2_yin, p2_xin, p2_lt;
    reg  [1:0]  p2_rb;
    // считаются ниже, у P0 (нужны шагу j_x/j_sx)
    wire [1:0]  i_rb, xk1, xk2;
    wire signed [32:0] s_a1, s_a2;
    wire [10:0] j_xn1, j_xn2;
    wire        i_yin, j_e1, j_e2;
    wire        bq_okt, bq_okb;         // тексели tx, tx + 1 следующей точки — в окне кэша (верх, низ)
    reg         p2_go;                  // выдаётся пара (комбинационно ниже, у P0)
    // предвыборка начала пролёта (X_SET4, X_SET5): строка кэша первого текселя
    reg  [22:0] sp_m;                   // ty0 · stride
    reg  [11:0] sp_col;                 // столбец первого текселя, байты
    reg         sp_ok;                  // первый тексель в раскладке
    reg         sp_req;                 // такт: заявка (строка sp_req_line)
    reg  [17:0] sp_req_line;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            xs_q <= X_IDLE;
            cx <= cx_default();
            sv_sp <= '0; sv_cnt <= '0; sv_k <= '0; sv_raddr <= '0;
            y <= '0; n_lines <= '0; tx0 <= '0; ty0 <= '0; h_n <= 11'd1024; untouched <= 1'b0;
            it_r0 <= '0; it_r1 <= '0; it_rall <= 1'b0; n_tfull <= 1'b0; j_row <= '0; j_rend <= '0; j_xs <= '0;
            j_sx0 <= '0; j_sy0 <= '0; j_b <= '0; j_e <= '0;
            impl_c <= '0; impl_s <= '0; slot_nx <= 1'b0; it <= '0; clr_mask <= '0;
            slot_we <= 1'b0; slot_sel <= 1'b0; slot_data <= '0;
            impl_we <= 1'b0; impl_rgba <= '0; impl_st <= '0;
            cur_buf <= 1'b0; tile_go <= 1'b0; tile_buf <= 1'b0; tile_x <= '0; tile_y <= '0;
            busy <= 1'b0; unsup <= '0; j_run <= 1'b0;
            sp_m <= '0; sp_col <= '0; sp_ok <= 1'b0; sp_req <= 1'b0; sp_req_line <= '0;
            n_kind <= '0; n_xs <= '0; n_xe <= '0; n_skip <= 1'b0; n_rel <= '0; n_rely <= '0;
            m0 <= '0; m1 <= '0; m2 <= '0; mk <= '0; m_sl <= '0; m_cell <= '0;
            n_sx <= '0; n_sy <= '0; n_dy <= '0; n_dy0 <= 1'b0; n_corner <= 1'b0; n_cy <= '0;
            n_s <= '0; n_k1 <= '0; n_r8 <= '0; n_o <= '0;
            n_lo <= '0; n_hi <= '0; n_cylo <= '0; n_cyhi <= '0; n_cyio <= '0;
            n_first <= '0; n_last <= '0;
            j_kind <= '0; j_x <= '0; j_xe <= '0; j_sx <= '0; j_sy <= '0; j_a <= '0; j_d <= '0;
            j_k <= '0; j_bil <= 1'b0; j_bilp <= 1'b0;
            bq_t <= 1'b0; bq_b <= 1'b0;
            p2_first <= 1'b1; p2_yin <= 1'b0; p2_xin <= 1'b0; p2_lt <= 1'b0; p2_rb <= '0;
            j_base <= '0; j_stride <= '0; j_lw <= '0; j_lh <= '0; j_fmt <= '0; j_dfmt <= '0; j_p8 <= 1'b0;
            j_x0 <= '0; j_x1 <= '0; j_r <= '0; j_r8 <= '0; j_o <= '0; j_dy2 <= '0;
            j_r8sq <= '0; j_osq <= '0; j_kq <= '0; j_kok <= 1'b0; j_rz <= 1'b0;
            j_corner <= 1'b0; j_dy0 <= 1'b0; j_cy <= '0; j_s <= '0; j_rgb <= '0; j_ca <= '0;
            j_slot <= 1'b0;
        end else begin
            slot_we <= 1'b0;
            impl_we <= 1'b0;
            tile_go <= 1'b0;

            sp_req <= 1'b0;
            if (issue) begin
                j_k <= b_last ? 2'd0 : (j_k == 2'd0 && bq_t) ? 2'd2 : j_k + 2'd1;
                if (b_last) begin
                    bq_t <= j_bilp && xk1 == 2'b11 && bq_okt;
                    bq_b <= j_bilp && xk1 == 2'b11 && bq_okb;
                    j_x  <= p2_go ? j_xn2 : j_xn1;
                    j_sx <= p2_go ? s_a2 : s_a1;
                    j_sy <= j_sy + {{16{j_d[16]}}, j_d};   // у пары D = 0
                    // проверки следующей точки (для пары) и строки — на первой точке строки
                    p2_xin <= p2_go ? xk2[1] : xk1[1];
                    p2_lt  <= p2_go ? xk2[0] : xk1[0];
                    if (p2_first) begin
                        p2_first <= 1'b0;
                        p2_yin   <= i_yin;
                        p2_rb    <= i_rb;
                    end
                end
            end
            // строка выдана: следующая строка плитки — тот же пролёт, тексель начала + (B, E)
            if (j_run && (j_x >= j_xe || (issue && b_last && (p2_go ? j_e2 : j_e1)))) begin
                p2_first <= 1'b1;
                bq_t     <= 1'b0;
                bq_b     <= 1'b0;
                if ({1'b0, j_row} + 5'd1 < j_rend) begin
                    j_row <= j_row + 4'd1;
                    j_x   <= j_xs;
                    j_sx  <= j_sx0 + {{16{j_b[16]}}, j_b};
                    j_sy  <= j_sy0 + {{16{j_e[16]}}, j_e};
                    j_sx0 <= j_sx0 + {{16{j_b[16]}}, j_b};
                    j_sy0 <= j_sy0 + {{16{j_e[16]}}, j_e};
                end else j_run <= 1'b0;
            end

            if (abort) begin
                xs_q  <= X_IDLE;
                busy  <= 1'b0;
                j_run <= 1'b0;
            end else case (xs_q)
            X_IDLE: if (start) begin
                busy    <= 1'b1;
                tx0     <= '0;
                ty0     <= '0;
                n_lines <= lines;
                h_n     <= hsize;
                cur_buf <= 1'b0;
                xs_q    <= X_TILE;
            end

            // начало плитки: буфер свободен — значение по умолчанию 0, контекст по умолчанию
            X_TILE: if (buf_free[cur_buf] && !clr_on) begin
                cx        <= cx_default();
                sv_sp     <= '0;
                sv_cnt    <= '0;
                untouched <= 1'b1;
                impl_c    <= '0;
                impl_s    <= '0;
                impl_we   <= 1'b1;
                impl_rgba <= '0;
                impl_st   <= '0;
                xs_q      <= X_FETCH;
            end

            X_FETCH: if (q_v) begin
                it    <= q_data;
                it_r0 <= q_r0;
                it_r1 <= q_r1;
                it_rall <= q_r0 == 5'd0 && q_r1 == t_rows;
                y     <= ty0 + {5'd0, q_r0};
                case (q_kind)
                    K_STATE: case (q_word[31:24])
                        8'h02: cx[O_CRGB +: 24] <= q_word[23:0];
                        8'h04: cx[O_RGB +: 24]  <= q_word[23:0];
                        8'h09: begin cx[O_AF +: 3] <= q_word[10:8]; cx[O_AREF +: 8] <= q_word[7:0]; end
                        8'h0A: begin
                            cx[O_SF +: 3]    <= q_word[18:16];
                            cx[O_SREF +: 8]  <= q_word[15:8];
                            cx[O_SMASK +: 8] <= q_word[7:0];
                        end
                        8'h0B: begin cx[O_BSRC +: 3] <= q_word[5:3]; cx[O_BDST +: 3] <= q_word[2:0]; end
                        8'h0C: begin cx[O_SFAIL +: 3] <= q_word[5:3]; cx[O_SPASS +: 3] <= q_word[2:0]; end
                        8'h0F: cx[O_CA +: 8]   <= q_word[7:0];
                        8'h10: cx[O_A +: 8]    <= q_word[7:0];
                        8'h11: cx[O_CST +: 8]  <= q_word[7:0];
                        8'h13: cx[O_SWM +: 8]  <= q_word[7:0];
                        8'h15: cx[O_TA +: 17]  <= q_word[16:0];
                        8'h16: cx[O_TB +: 17]  <= q_word[16:0];
                        8'h17: cx[O_TC +: 24]  <= q_word[23:0];
                        8'h18: cx[O_TD +: 17]  <= q_word[16:0];
                        8'h19: cx[O_TE +: 17]  <= q_word[16:0];
                        8'h1A: cx[O_TF +: 24]  <= q_word[23:0];
                        8'h1B: begin cx[O_SCX +: 11] <= q_word[21:11]; cx[O_SCY +: 11] <= q_word[10:0]; end
                        8'h1C: begin cx[O_SCW +: 12] <= q_word[23:12]; cx[O_SCH +: 12] <= q_word[11:0]; end
                        8'h20: cx[O_CM +: 4]   <= q_word[3:0];
                        8'h2A: cx[O_PAL +: 22] <= q_word[21:0];
                        8'h22: begin sv_k <= '0; xs_q <= X_SAVE; end
                        8'h23: begin
                            if (sv_cnt == 3'd0) cx <= cx_default();
                            else begin
                                sv_k     <= '0;
                                sv_raddr <= {sv_sp - 2'd1, 4'd0};
                                xs_q     <= X_REST;
                            end
                        end
                        8'h26: begin clr_mask <= q_word[2:0]; n_kind <= J_CLR; xs_q <= X_SET0; end
                        default: ;
                    endcase
                    K_BMP:   begin n_kind <= J_BMP;   xs_q <= X_SET0; end
                    K_RECT:  begin n_kind <= J_RECT;  xs_q <= X_SET0; end
                    K_POINT: begin n_kind <= J_POINT; xs_q <= X_SET0; end
                    K_EOL:   xs_q <= X_EOL;
                    default: ;
                endcase
            end

            // SAVE: 9 слов по 36 бит в место sv_sp (запись — sv_we)
            X_SAVE: begin
                if (sv_k == 4'(CX_WORDS - 1)) begin
                    sv_sp  <= sv_sp + 2'd1;
                    sv_cnt <= sv_cnt == 3'd4 ? 3'd4 : sv_cnt + 3'd1;
                    xs_q   <= X_FETCH;
                end else sv_k <= sv_k + 4'd1;
            end

            // RESTORE: чтение с задержкой такт — слово k приходит на шаге k + 1
            X_REST: begin
                sv_raddr <= sv_raddr + 6'd1;
                sv_k     <= sv_k + 4'd1;
                // слово k (пришло на шаге k + 1) — в свои 36 бит контекста
                case (sv_k)
                    4'd1: cx[0 +: 36]   <= sv_q;
                    4'd2: cx[36 +: 36]  <= sv_q;
                    4'd3: cx[72 +: 36]  <= sv_q;
                    4'd4: cx[108 +: 36] <= sv_q;
                    4'd5: cx[144 +: 36] <= sv_q;
                    4'd6: cx[180 +: 36] <= sv_q;
                    4'd7: cx[216 +: 36] <= sv_q;
                    4'd8: cx[252 +: 36] <= sv_q;
                    4'd9: cx[288 +: CXW - 288] <= sv_q[CXW - 289:0];
                    default: ;
                endcase
                if (sv_k == 4'(CX_WORDS)) begin
                    sv_sp  <= sv_sp - 2'd1;
                    sv_cnt <= sv_cnt - 3'd1;
                    xs_q   <= X_FETCH;
                end
            end

            // подготовка: пролёт и операнды
            // подготовка: 0 — края пролёта и строки, 0B — пролёт в точках и ножницы,
            // 1 — смещение первой точки и квадраты, 2 — преобразование, 3 — его суммы
            X_SET0: begin
                setup0a();
                xs_q <= X_SET0B;
            end
            X_SET0B: begin
                setup0b();
                xs_q <= X_SET0C;
            end
            X_SET0C: begin
                setup0c();
                xs_q <= X_SET1;
            end
            X_SET1: begin : set1
                logic signed [20:0] rel;
                rel   = $signed({6'd0, n_xs, 4'd0}) - $signed({it[22], it[22:3]});
                n_rel <= rel[17:0];
                n_cy  <= axis_cov2(n_cyio, n_s);
                if (n_kind == J_RECT || n_kind == J_POINT) begin
                    // квадраты: dy, r + 8, r + 16, K + 1
                    m0 <= $signed({22'd0, n_dy}) * $signed({22'd0, n_dy});
                    m1 <= $signed({22'd0, n_r8}) * $signed({22'd0, n_r8});
                    m2 <= $signed({22'd0, n_o}) * $signed({22'd0, n_o});
                    mk <= 36'($signed(n_k1) * $signed(n_k1));
                end
                m_sl <= it[81:70] * it[92:82];
                xs_q <= X_SET2;
            end
            X_SET2: begin
                if (n_kind == J_BMP) begin
                    m0 <= n_rel * c_ta + n_rely * c_tb;
                    m2 <= n_rel * c_td + n_rely * c_te;
                end
                m_cell <= m_sl * it[126:120];
                xs_q   <= X_SET3;
            end
            X_SET3: begin
                n_sx <= 33'(m0 >>> 4) + {{9{c_tc[23]}}, c_tc};
                n_sy <= 33'(m2 >>> 4) + {{9{c_tf[23]}}, c_tf};
                if (n_kind == J_BMP && !n_skip) xs_q <= X_SET4;
                else                            xs_q <= X_XFER;
            end
            // предвыборка начала пролёта: первый тексель — строка ty0·stride, столбец tx0
            X_SET4: begin : set4
                logic signed [24:0] tx, ty;
                logic [14:0] lw, txw;
                logic [10:0] lh, tyw;
                logic [11:0] stride;
                logic [4:0]  fmt;
                stride = it[81:70];
                fmt    = it[69:65];
                lh     = it[92:82];
                case (fmt)
                    5'd1:              lw = {stride, 3'b000};
                    5'd17:             lw = {1'b0, stride, 2'b00};
                    5'd2:              lw = {2'b0, stride, 1'b0};
                    5'd0, 5'd6, 5'd7:  lw = {4'b0, stride[11:1]};
                    default:           lw = {3'b0, stride};
                endcase
                tx  = n_sx[32:8];
                ty  = n_sy[32:8];
                txw = it[94] ? tx[14:0] & (lw - 15'd1) : tx[14:0];
                tyw = it[95] ? ty[10:0] & (lh - 11'd1) : ty[10:0];
                sp_ok <= lw != 15'd0 && lh != 11'd0 &&
                         (it[94] || (tx >= 0 && tx < $signed({10'd0, lw}))) &&
                         (it[95] || (ty >= 0 && ty < $signed({14'd0, lh})));
                sp_m <= tyw * stride;
                case (fmt)
                    5'd1:             sp_col <= {3'd0, txw[14:3]};
                    5'd17:            sp_col <= {2'd0, txw[13:2]};
                    5'd2:             sp_col <= {1'd0, txw[12:1]};
                    5'd0, 5'd6, 5'd7: sp_col <= {txw[10:0], 1'b0};
                    default:          sp_col <= txw[11:0];
                endcase
                xs_q <= X_SET5;
            end
            X_SET5: begin : set5
                logic [21:0] a;
                a = it[64:43] + m_cell[21:0] + sp_m[21:0] + {10'd0, sp_col};
                // в RAM_G (как P2/P3: ПЗУ — со сдвигом слова, рампа и вне карты — без заявки)
                if (sp_ok && a < 22'h300000 && (ramg_3mb || a < 22'h200064 || a >= 22'h200268)) begin
                    sp_req      <= 1'b1;
                    sp_req_line <= !ramg_3mb && a >= 22'h200000 ? 18'((21'(a[21:2]) + 21'h048000) >> 3)
                                                                 : 18'(a[21:5]);
                end
                xs_q <= X_XFER;
            end

            // перенос в регистры задания: конвейер свободен от прошлого задания, место — от
            // его фрагментов в ft_rd_pix
            X_XFER: begin
                if (n_skip) begin
                    xs_q <= X_FETCH;
                end else if (n_kind == J_CLR && untouched && it_rall && n_tfull) begin
                    // первый CLEAR на пустой плитке во всю плитку — только значение по умолчанию
                    lazy_clear();
                    xs_q <= X_FETCH;
                end else if (!j_run && front_empty && !slot_busy[slot_nx]) begin
                    // конвейер свободен от прошлого задания (его регистры j_* читают стадии)
                    transfer();
                    j_run <= 1'b1;
                    xs_q  <= X_FETCH;
                end
            end

            X_RUN: xs_q <= X_FETCH;

            // конец плитки: всё дорисовано — буфер уходит на запись, дальше — другой; следующая
            // плитка — правее, у правого края — в начале следующей полосы строк
            X_EOL: if (!j_run && front_empty && be_empty) begin
                tile_go  <= 1'b1;
                tile_buf <= cur_buf;
                tile_x   <= 4'(tx0 >> TW_LOG);
                tile_y   <= ty0;
                cur_buf  <= !cur_buf;
                if (!t_last_x) begin
                    tx0  <= tx0 + 10'(TW);
                    xs_q <= X_TILE;
                end else if (!t_last_y) begin
                    tx0  <= '0;
                    ty0  <= ty0 + 10'(TH);
                    xs_q <= X_TILE;
                end else begin
                    busy <= 1'b0;
                    xs_q <= X_IDLE;
                end
            end
            default: xs_q <= X_IDLE;
            endcase
        end
    end

    // ---- подготовка, шаг 0: края пролёта и строки ----
    task automatic setup0a();
        logic signed [20:0] vx, vy, x1, ry1, dyp;
        logic signed [20:0] bl, br;
        logic [15:0] dl, dh;
        logic [11:0] bw, r;
        logic [12:0] ps;
        logic [13:0] o;
        logic [4:0]  s;
        logic        above, below;
        vx  = $signed({it[22], it[22:3]});
        vy  = $signed({it[42], it[42:23]});
        x1  = $signed({it[62], it[62:43]});
        ry1 = $signed({it[82], it[82:63]});
        r   = it[94:83];
        ps  = it[55:43];
        bw  = it[107:96];
        s   = r >= 12'd16 ? 5'd16 : r[4:0];
        above = $signed(y16) < vy;              // RECT: строка выше ядра
        below = $signed(y16) > ry1;             //       ниже ядра
        dyp   = $signed(y16) - vy;
        // пролёт строки: lo = левый край − dl, hi = правый край + dh
        bl = vx;
        br = n_kind == J_RECT ? x1 : vx;
        case (n_kind)
            J_BMP:   begin dl = 16'd0; dh = {bw, 4'd0}; end
            J_RECT:  if ((above || below) && r >= 12'd16) begin dl = 16'(r) + 16'd17; dh = 16'(r) + 16'd16; end  // угол: r + 16
                     else begin dl = 16'(r) + 16'd8; dh = 16'(r) + 16'd7; end   // ядро, и r < 16: < r + 8
            J_POINT: begin dl = 16'(ps) + 16'd17; dh = 16'(ps) + 16'd16; end
            default: begin bl = 21'sd0; br = 21'sd0; dl = 16'd1; dh = 16'd32752; end   // CLEAR
        endcase
        n_lo   <= bl - $signed({5'd0, dl});
        n_hi   <= br + $signed({5'd0, dh});
        n_cylo <= $signed({vy[20], vy}) - $signed({10'd0, r}) - $signed({1'b0, y16});
        n_cyhi <= $signed({ry1[20], ry1}) + $signed({10'd0, r}) - $signed({1'b0, y16});
        n_rely <= 18'(dyp);
        n_corner <= 1'b0;
        n_dy0    <= 1'b0;
        case (n_kind)
        J_RECT: begin
            o = {2'b0, r} + 14'd16;
            n_dy     <= above ? 14'(vy - $signed(y16)) : (below ? 14'($signed(y16) - ry1) : 14'd0);
            n_corner <= (above || below) && r >= 12'd16;   // r < 16: углы не скругляются
            n_s      <= s;
            n_r8     <= {2'b0, r} + 14'd8;
            n_o      <= o;
            n_k1     <= $signed({({2'b0, r} + 14'd8), 4'd0}) - $signed({5'd0, vfull(s)}) + 18'sd1;
        end
        J_POINT: begin
            o  = {1'b0, ps} + 14'd16;
            n_dy  <= dyp < 0 ? 14'(-dyp) : dyp[13:0];
            n_dy0 <= dyp == 0;
            n_s   <= 5'd16;
            n_r8  <= {1'b0, ps} + 14'd8;
            n_o   <= o;
            n_k1  <= $signed({1'b0, ps, 4'd0}) - 18'sd127;     // 16·r − 128 + 1
        end
        default: ;
        endcase
    endtask

    // ---- подготовка, шаг 0B: пролёт в точках ----
    task automatic setup0b();
        // BITMAPS: первая точка ⌈vx/16⌉, после последней ⌈(vx + w·16)/16⌉; фигуры:
        // точки p с lo < p·16 ≤ hi
        if (n_kind == J_BMP) begin
            n_first <= (n_lo + 21'sd15) >>> 4;
            n_last  <= (n_hi + 21'sd15) >>> 4;
        end else begin
            n_first <= (n_lo + 21'sd16) >>> 4;
            n_last  <= (n_hi >>> 4) + 21'sd1;
        end
        n_cyio <= axis_ov(n_cylo, n_cyhi);
    endtask

    // ---- подготовка, шаг 0C: ножницы и строка ----
    task automatic setup0c();
        logic [21:0] cl;
        cl = clip(n_first, n_last, scx_r, scend_r, 11'h7FF);   // ширина кадра — уже в scend_r
        n_xs   <= cl[21:11];
        n_xe   <= cl[10:0];
        n_tfull <= cl[21:11] == {1'b0, tx0} && cl[10:0] == t_x1;
        n_skip <= it_r0 >= it_r1 || cl[21:11] >= cl[10:0] ||
                  (n_kind == J_CLR && !(clr_mask[2] || clr_mask[1]));
    endtask

    // ---- CLEAR без записи: значение по умолчанию строки ----
    task automatic lazy_clear();
        logic [31:0] v;
        logic [7:0]  s;
        v = impl_c;
        s = impl_s;
        if (clr_mask[2]) begin
            if (c_cm[3]) v[23:16] = cx[O_CRGB + 16 +: 8];
            if (c_cm[2]) v[15:8]  = cx[O_CRGB + 8 +: 8];
            if (c_cm[1]) v[7:0]   = cx[O_CRGB +: 8];
            if (c_cm[0]) v[31:24] = cx[O_CA +: 8];
        end
        if (clr_mask[1])
            s = (s & ~cx[O_SWM +: 8]) | (cx[O_CST +: 8] & cx[O_SWM +: 8]);
        impl_c    <= v;
        impl_s    <= s;
        impl_we   <= 1'b1;
        impl_rgba <= v;
        impl_st   <= s;
    endtask

    // ---- перенос задания ----
    task automatic transfer();
        logic [4:0] fmt;
        logic [11:0] stride;
        fmt    = it[69:65];
        stride = it[81:70];
        untouched <= 1'b0;
        j_kind  <= n_kind;
        j_row   <= it_r0[3:0];
        j_rend  <= it_r1;
        j_xs    <= n_xs;
        j_x     <= n_xs;
        j_xe    <= n_xe;
        j_slot  <= slot_nx;
        slot_nx <= !slot_nx;
        j_rgb   <= n_kind == J_CLR ? cx[O_CRGB +: 24] : c_rgb;
        j_ca    <= n_kind == J_CLR ? cx[O_CA +: 8] : c_a;
        // место ft_rd_pix: проверки, трафарет, смешение, маски, очистка
        slot_we   <= 1'b1;
        slot_sel  <= slot_nx;
        slot_data <= {n_kind == J_CLR && clr_mask[2], n_kind == J_CLR && clr_mask[1],
                      cx[O_CST +: 8], cx[O_CM +: 4], cx[O_BDST +: 3], cx[O_BSRC +: 3],
                      cx[O_SPASS +: 3], cx[O_SFAIL +: 3], cx[O_SWM +: 8], cx[O_SMASK +: 8],
                      cx[O_SREF +: 8], cx[O_SF +: 3], cx[O_AREF +: 8], cx[O_AF +: 3]};
        // BITMAPS
        j_sx     <= n_sx;
        j_sy     <= n_sy;
        j_sx0    <= n_sx;
        j_sy0    <= n_sy;
        j_a      <= c_ta;
        j_a2     <= {c_ta, 1'b0};
        j_bil    <= n_kind == J_BMP && it[93];
        j_bilp   <= n_kind == J_BMP && it[93] && !it[95] && c_td == 17'sd0 && !(it[64] && !ramg_3mb) &&
                    (fmt == 5'd0 || (fmt >= 5'd3 && fmt <= 5'd7) || (fmt >= 5'd14 && fmt <= 5'd16));
        j_k      <= 2'd0;
        bq_t     <= 1'b0;
        bq_b     <= 1'b0;
        p2_first <= 1'b1;
        j_dual   <= n_kind == J_BMP && !it[93] && c_td == 17'sd0 && c_ta >= 17'sd0 && c_ta <= 17'sd256 &&
                    (fmt == 5'd0 || (fmt >= 5'd3 && fmt <= 5'd7) || (fmt >= 5'd14 && fmt <= 5'd16)) &&
                    c_rgb == 24'hFFFFFF && c_a == 8'd255 && cx[O_BSRC +: 3] == 3'd2 && cx[O_BDST +: 3] == 3'd4 &&
                    cx[O_AF +: 3] == 3'd7 && cx[O_SF +: 3] == 3'd7 &&
                    ((cx[O_SFAIL +: 3] == 3'd1 && cx[O_SPASS +: 3] == 3'd1) || cx[O_SWM +: 8] == 8'd0) &&
                    cx[O_CM +: 4] == 4'hF;
        j_d      <= c_td;
        j_b      <= c_tb;
        j_e      <= c_te;
        j_base   <= it[64:43] + m_cell[21:0];
        j_stride <= stride;
        j_fmt    <= fmt;
        j_dfmt   <= fmt == 5'd14 ? 5'd7 : (fmt == 5'd15 ? 5'd6 : fmt);
        j_p8     <= fmt == 5'd16;
        j_lh     <= it[92:82];
        case (fmt)
            5'd1:              j_lw <= {stride, 3'b000};       // L1
            5'd17:             j_lw <= {1'b0, stride, 2'b00};  // L2
            5'd2:              j_lw <= {2'b0, stride, 1'b0};   // L4
            5'd0, 5'd6, 5'd7:  j_lw <= {4'b0, stride[11:1]};   // 16 бит
            default:           j_lw <= {3'b0, stride};
        endcase
        j_wx     <= it[94];
        j_wy     <= it[95];
        j_lhm    <= it[92:82] - 11'd1;
        j_pal    <= cx[O_PAL +: 22];
        case (fmt)
            5'd1:              j_lwm <= {stride, 3'b000} - 15'd1;
            5'd17:             j_lwm <= {1'b0, stride, 2'b00} - 15'd1;
            5'd2:              j_lwm <= {2'b0, stride, 1'b0} - 15'd1;
            5'd0, 5'd6, 5'd7:  j_lwm <= {4'b0, stride[11:1]} - 15'd1;
            default:           j_lwm <= {3'b0, stride} - 15'd1;
        endcase
        if (n_kind == J_BMP) begin
            if (!(fmt <= 5'd7 || fmt == 5'd17 || (fmt >= 5'd14 && fmt <= 5'd16))) unsup[2] <= 1'b1;
        end
        // RECTS/POINTS
        j_x0     <= $signed({it[22], it[22:3]});
        j_x1     <= $signed({it[62], it[62:43]});
        j_r      <= it[94:83];
        j_rz     <= n_kind == J_RECT ? it[94:83] == 12'd0 : it[55:43] == 13'd0;
        j_r8     <= n_r8;
        j_o      <= n_o;
        j_dy2    <= m0[27:0];
        j_r8sq   <= m1[27:0];
        j_osq    <= m2[27:0];
        j_kq     <= 28'((mk - 36'd1) >> 8);
        j_kok    <= n_k1 > 0;
        j_corner <= n_corner;
        j_dy0    <= n_dy0;
        j_cy     <= n_cy;
        j_cyio   <= n_cyio;
        j_thin   <= it[94:83] < 12'd16;
        j_s      <= n_s;
    endtask

    // =========================================================================================
    // Конвейер: стадии
    // =========================================================================================
    // P0: тексель / расстояние
    // REPEAT — маска (ширина − 1), как у bt8xxemu (раскладка 6×5: тексели 0, 1, 0, 1, 4, 5…)
    wire signed [24:0] i_txr = j_sx[32:8];
    wire signed [24:0] i_tyr = j_sy[32:8];
    wire signed [24:0] i_tx = j_wx ? {10'd0, i_txr[14:0] & j_lwm} : i_txr;
    wire signed [24:0] i_ty = j_wy ? {14'd0, i_tyr[10:0] & j_lhm} : i_tyr;
    wire        i_in = (j_wx || (i_tx >= 0 && i_tx < $signed({10'd0, j_lw}))) &&
                       (j_wy || (i_ty >= 0 && i_ty < $signed({14'd0, j_lh}))) &&
                       j_lw != 15'd0 && j_lh != 11'd0;
    // тексель той же точки следующей строки задания (sx + B, sy + E) — для предвыборки: P0N
    // координаты, P1N строка·stride, P2N адрес строки кэша (только RAM_G; ПЗУ и рампу не трогает)
    wire signed [32:0] n_sxn = j_sx + {{16{j_b[16]}}, j_b};
    wire signed [32:0] n_syn = j_sy + {{16{j_e[16]}}, j_e};
    wire signed [24:0] n_txr = n_sxn[32:8];
    wire signed [24:0] n_tyr = n_syn[32:8];
    wire signed [24:0] n_tx = j_wx ? {10'd0, n_txr[14:0] & j_lwm} : n_txr;
    wire signed [24:0] n_ty = j_wy ? {14'd0, n_tyr[10:0] & j_lhm} : n_tyr;
    wire        n_in = (j_wx || (n_tx >= 0 && n_tx < $signed({10'd0, j_lw}))) &&
                       (j_wy || (n_ty >= 0 && n_ty < $signed({14'd0, j_lh}))) &&
                       j_lw != 15'd0 && j_lh != 11'd0;
    reg  [11:0] n_col;
    always_comb begin
        case (j_fmt)
            5'd1:             n_col = n_tx[14:3];
            5'd17:            n_col = n_tx[13:2];
            5'd2:             n_col = n_tx[12:1];
            5'd0, 5'd6, 5'd7: n_col = {n_tx[10:0], 1'b0};
            default:          n_col = n_tx[11:0];
        endcase
    end
    reg         n0_v, n1_v, n2_v;
    reg  [10:0] n0_ty;
    reg  [11:0] n0_col;
    reg  [21:0] n1_bc;
    reg  [22:0] n1_ro;
    reg  [17:0] n2_line;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            n0_v <= 1'b0; n1_v <= 1'b0; n2_v <= 1'b0;
            n0_ty <= '0; n0_col <= '0; n1_bc <= '0; n1_ro <= '0; n2_line <= '0;
        end else begin
            // только у BITMAPS, если у задания в плитке есть строка ниже
            n0_v   <= issue && k_bmp && n_in && {1'b0, j_row} + 5'd1 < j_rend;
            n0_ty  <= n_ty[10:0];
            n0_col <= n_col;
            n1_v   <= n0_v;
            n1_bc  <= j_base + {10'd0, n0_col};
            n1_ro  <= n0_ty * j_stride;
            begin : n2
                logic [21:0] a;
                a = n1_bc + n1_ro[21:0];
                n2_v    <= n1_v && (ramg_3mb ? a < 22'h300000 : a < 22'h200000);
                n2_line <= {1'b0, a[21:5]};
            end
        end
    end

    // BILINEAR: тексель прохода k, доли fx, fy, вес
    wire signed [24:0] b_txr = j_sx[32:8] + {24'd0, j_k[0]};
    wire signed [24:0] b_tyr = j_sy[32:8] + {24'd0, j_k[1]};
    wire signed [24:0] b_tx = j_wx ? {10'd0, b_txr[14:0] & j_lwm} : b_txr;
    wire signed [24:0] b_ty = j_wy ? {14'd0, b_tyr[10:0] & j_lhm} : b_tyr;
    wire        b_in = (j_wx || (b_tx >= 0 && b_tx < $signed({10'd0, j_lw}))) &&
                       (j_wy || (b_ty >= 0 && b_ty < $signed({14'd0, j_lh}))) &&
                       j_lw != 15'd0 && j_lh != 11'd0;
    reg  [11:0] b_col;
    reg  [2:0]  b_sub;
    always_comb begin
        case (j_fmt)
            5'd1:             begin b_col = b_tx[14:3]; b_sub = b_tx[2:0]; end
            5'd17:            begin b_col = b_tx[13:2]; b_sub = {1'b0, b_tx[1:0]}; end
            5'd2:             begin b_col = b_tx[12:1]; b_sub = {2'b0, b_tx[0]}; end
            5'd0, 5'd6, 5'd7: begin b_col = {b_tx[10:0], 1'b0}; b_sub = 3'd0; end
            default:          begin b_col = b_tx[11:0]; b_sub = 3'd0; end
        endcase
    end
    wire [7:0]  b_fx = j_sx[7:0];
    wire [7:0]  b_fy = j_sy[7:0];
    wire [17:0] b_m;
    ft_mul9 u_mul_w11 (.clk(clk), .a({1'b0, b_fx}), .b({1'b0, b_fy}), .p(b_m));
    wire [7:0]  b_w11 = b_m[15:8];
    reg  [8:0]  b_w;
    always_comb begin
        case (j_k)
            2'd0:    b_w = 9'd256 - {1'b0, b_fx} - {1'b0, b_fy} + {1'b0, b_w11};
            2'd1:    b_w = {1'b0, b_fx} - {1'b0, b_w11};
            2'd2:    b_w = {1'b0, b_fy} - {1'b0, b_w11};
            default: b_w = {1'b0, b_w11};
        endcase
    end
    // пара текселей: вес второго (tx + 1) — вес прохода k + 1
    wire [8:0]  b_w2 = j_k[1] ? {1'b0, b_w11} : {1'b0, b_fx} - {1'b0, b_w11};


    reg  [11:0] i_col;
    reg  [2:0]  i_sub;
    always_comb begin
        case (j_fmt)
            5'd1:             begin i_col = i_tx[14:3]; i_sub = i_tx[2:0]; end
            5'd17:            begin i_col = i_tx[13:2]; i_sub = {1'b0, i_tx[1:0]}; end
            5'd2:             begin i_col = i_tx[12:1]; i_sub = {2'b0, i_tx[0]}; end
            5'd0, 5'd6, 5'd7: begin i_col = {i_tx[10:0], 1'b0}; i_sub = 3'd0; end
            default:          begin i_col = i_tx[11:0]; i_sub = 3'd0; end
        endcase
    end
    // вторая точка пары (0 ≤ A ≤ 256, D = 0): её тексель — тот же или следующий в строке
    // (перенос дробной части sx + A); байт в слове — по младшим разрядам начала строки
    // base + ty·stride и текселя. Короткий путь: решение идёт в шаг j_x и j_sx того же такта.
    wire        i_b16 = j_fmt == 5'd0 || j_fmt == 5'd6 || j_fmt == 5'd7;      // 2 байта на тексель
    wire [8:0]  i_fr2 = {1'b0, j_sx[7:0]} + j_a[8:0];
    wire        i_inc = i_fr2[8];                                         // второй — следующий
    assign      i_rb  = j_base[1:0] + 2'(i_ty[1:0] * j_stride[1:0]);
    wire [1:0]  i_b0  = i_rb + (i_b16 ? {i_tx[0], 1'b0} : i_tx[1:0]);
    wire signed [12:0] i_dc = i_inc ? (i_b16 ? 13'sd2 : 13'sd1) : 13'sd0;
    // пара по регистрам: байт текселя в слове — из выравнивания строки p2_rb и младших разрядов tx
    wire [1:0]  p2_tx  = j_wx ? (j_sx[9:8] & j_lwm[1:0]) : j_sx[9:8];
    wire [1:0]  p2_b0  = p2_rb + (i_b16 ? {p2_tx[0], 1'b0} : p2_tx);
    wire        p2_same = i_b16 ? (p2_b0 <= 2'd2 && (!i_inc || p2_b0 == 2'd0)) : (!i_inc || p2_b0 != 2'd3);
    // следующая точка: шаг +A или +2A — суммы заранее, x-проверка для обеих
    assign s_a1 = j_sx + {{16{j_a[16]}}, j_a};
    assign s_a2 = j_sx + {{15{j_a2[17]}}, j_a2};
    // tx следующей точки (повтор — по маске) и проверки: {в раскладке, tx < ширина − 1}
    wire signed [24:0] k1_r = s_a1[32:8];
    wire signed [24:0] k2_r = s_a2[32:8];
    wire signed [24:0] k1_t = j_wx ? $signed({10'd0, k1_r[14:0] & j_lwm}) : k1_r;
    wire signed [24:0] k2_t = j_wx ? $signed({10'd0, k2_r[14:0] & j_lwm}) : k2_r;
    assign xk1 = {j_wx || (k1_t >= 0 && k1_t < $signed({10'd0, j_lw})), k1_t < $signed({10'd0, j_lwm})};
    assign xk2 = {j_wx || (k2_t >= 0 && k2_t < $signed({10'd0, j_lw})), k2_t < $signed({10'd0, j_lwm})};
    // BILINEAR: оба текселя следующей точки (tx, tx + 1) — в окне кэша: слово текселя и следующее
    // той же строки кэша (у слова 7 следующего нет — пара целиком в нём); 16 бит — с чётного байта
    wire [4:0]  bq_tx = i_b16 ? {k1_t[3:0], 1'b0} : k1_t[4:0];
    wire [4:0]  bq_at = bq_rt + bq_tx;
    wire [4:0]  bq_ab = bq_rb + bq_tx;
    assign bq_okt = (bq_at[4:2] != 3'd7 || (i_b16 ? bq_at[1:0] == 2'd0 : bq_at[1:0] != 2'd3)) && !(i_b16 && bq_at[0]);
    assign bq_okb = (bq_ab[4:2] != 3'd7 || (i_b16 ? bq_ab[1:0] == 2'd0 : bq_ab[1:0] != 2'd3)) && !(i_b16 && bq_ab[0]);
    assign i_yin = (j_wy || (i_ty >= 0 && i_ty < $signed({14'd0, j_lh}))) && j_lw != 15'd0 && j_lh != 11'd0;
    assign j_xn1 = j_x + 11'd1;
    assign j_xn2 = j_x + 11'd2;
    assign j_e1 = j_xn1 >= j_xe;                                // точка — последняя в строке
    assign j_e2 = j_xn2 >= j_xe;
`ifdef NO_DUAL
    always_comb p2_go = 1'b0;                                      // проверка: пары выключены
`else
    always_comb p2_go = j_dual && !p2_first && !j_x[0] && !j_e1 && p2_xin && p2_yin && p2_same &&
                        (!i_inc || p2_lt);
`endif
    wire signed [20:0] i_px = $signed({7'd0, j_x[9:0], 4'd0});
    wire        i_left  = i_px < j_x0;
    wire        i_right = i_px > j_x1;
    wire        i_cnr   = j_corner && (i_left || i_right);
    wire signed [20:0] i_dxr = i_left ? j_x0 - i_px : i_px - j_x1;     // RECT, угол
    wire signed [20:0] i_dxp = i_px - j_x0;                             // POINT
    wire [13:0] i_axp = i_dxp < 0 ? 14'(-i_dxp) : i_dxp[13:0];
    // RECT: края фигуры по x относительно центра точки — на P1 из них покрытие
    wire signed [21:0] i_alo = $signed({j_x0[20], j_x0}) - $signed({10'd0, j_r}) - $signed({i_px[20], i_px});
    wire signed [21:0] i_ahi = $signed({j_x1[20], j_x1}) + $signed({10'd0, j_r}) - $signed({i_px[20], i_px});
    reg  signed [21:0] p0_alo, p0_ahi;

    // P4: биты текселя из слова кэша (или рампы ПЗУ)
    function automatic logic [7:0] ramp_byte(input logic [21:0] a);
        logic [22:0] d;
        d = {1'b0, a} - 23'h200065;
        if (a < 22'h200065 || a >= 22'h200265) ramp_byte = 8'd0;
        else ramp_byte = d < 23'd128 ? 8'd0 : (d >= 23'd383 ? 8'd255 : 8'(d - 23'd128));
    endfunction
    wire [31:0] ramp_w = {ramp_byte({p4_a[21:2], 2'd3}), ramp_byte({p4_a[21:2], 2'd2}),
                          ramp_byte({p4_a[21:2], 2'd1}), ramp_byte({p4_a[21:2], 2'd0})};
    // слово P4 — из своей половины кэша; у следующего нужна лишь младшая половина: второй
    // тексель пары BILINEAR за границей слова начинается с его байта 0
    wire [31:0] cq  = p4_wa[0] ? cqo : cqe;
    wire [15:0] cqn = p4_wa[0] ? cqe[15:0] : cqo[15:0];
    wire [31:0] tw = p4_ramp ? ramp_w : cq;
    wire [7:0]  tbyte = tw[8 * p4_a[1:0] +: 8];
    // второй тексель (пары точек или пары BILINEAR): байт (или пара байтов) слова со сдвигом
    // p4_d2 (у пары точек сдвиг не отрицателен и в слове); за границей слова — начало следующего
    wire [2:0]  t_bo2 = {1'b0, p4_a[1:0]} + 3'(p4_d2);
    wire [15:0] t_raw2 = t_bo2[2] ? (i_b16 ? cqn : {8'd0, cqn[7:0]}) :
                         i_b16 ? tw[8 * t_bo2[1:0] +: 16] : {8'd0, tw[8 * t_bo2[1:0] +: 8]};
    reg  [15:0] t_raw;
    always_comb begin
        case (j_fmt)
            5'd1:  t_raw = {15'd0, tbyte[3'd7 - p4_sub]};
            5'd17: t_raw = {14'd0, tbyte[3'd6 - {p4_sub[1:0], 1'b0} +: 2]};
            5'd2:  t_raw = {12'd0, p4_sub[0] ? tbyte[3:0] : tbyte[7:4]};
            5'd0, 5'd6, 5'd7:
                   t_raw = p4_a[1:0] == 2'd3 ? {8'd0, tw[31:24]} : tw[8 * p4_a[1:0] +: 16];
            default: t_raw = {8'd0, tbyte};
        endcase
    end

    // P4: покрытие угла / точки из корня: ((r + 8)·16 − F)·s / 16
    wire [17:0] sq_v  = {j_r8, 4'd0} - sq_root[17:0];
    wire [22:0] sq_vs = sq_v * j_s;
    wire [8:0]  band_cov = sq_vs[22:4] > 19'd256 ? 9'd256 : sq_vs[12:4];

    // P5: тексель → RGBA (таблицы раскрытия каналов — замер bt8xxemu, как в модели)
    function automatic logic [7:0] ex3(input logic [2:0] v);
        case (v)
            3'd0: ex3 = 8'd0; 3'd1: ex3 = 8'd36; 3'd2: ex3 = 8'd71; 3'd3: ex3 = 8'd107;
            3'd4: ex3 = 8'd143; 3'd5: ex3 = 8'd179; 3'd6: ex3 = 8'd215; default: ex3 = 8'd251;
        endcase
    endfunction
    function automatic logic [7:0] ex4(input logic [3:0] v);
        case (v)
            4'd0: ex4 = 8'd0; 4'd1: ex4 = 8'd17; 4'd2: ex4 = 8'd33; 4'd3: ex4 = 8'd50;
            4'd4: ex4 = 8'd67; 4'd5: ex4 = 8'd84; 4'd6: ex4 = 8'd101; 4'd7: ex4 = 8'd118;
            4'd8: ex4 = 8'd135; 4'd9: ex4 = 8'd152; 4'd10: ex4 = 8'd169; 4'd11: ex4 = 8'd186;
            4'd12: ex4 = 8'd203; 4'd13: ex4 = 8'd220; 4'd14: ex4 = 8'd237; default: ex4 = 8'd254;
        endcase
    endfunction
    // 4-битная альфа (L4, ARGB4, PAL4444): 15 — непрозрачно (255). У bt8xxemu 254 — как и у
    // цветов (ex4), но это «глюк FT812» (решение 2026-10-19): фон Zuma на деле непрозрачный, и
    // только с 255 точка не требует смешения
    function automatic logic [7:0] exa4(input logic [3:0] v);
        exa4 = v == 4'd15 ? 8'd255 : ex4(v);
    endfunction
    function automatic logic [7:0] ex5(input logic [4:0] v);
        case (v)
            5'd0: ex5 = 8'd0; 5'd1: ex5 = 8'd8; 5'd2: ex5 = 8'd16; 5'd3: ex5 = 8'd24;
            5'd4: ex5 = 8'd32; 5'd5: ex5 = 8'd41; 5'd6: ex5 = 8'd49; 5'd7: ex5 = 8'd57;
            5'd8: ex5 = 8'd65; 5'd9: ex5 = 8'd73; 5'd10: ex5 = 8'd82; 5'd11: ex5 = 8'd90;
            5'd12: ex5 = 8'd98; 5'd13: ex5 = 8'd106; 5'd14: ex5 = 8'd115; 5'd15: ex5 = 8'd123;
            5'd16: ex5 = 8'd131; 5'd17: ex5 = 8'd139; 5'd18: ex5 = 8'd147; 5'd19: ex5 = 8'd156;
            5'd20: ex5 = 8'd164; 5'd21: ex5 = 8'd172; 5'd22: ex5 = 8'd180; 5'd23: ex5 = 8'd189;
            5'd24: ex5 = 8'd197; 5'd25: ex5 = 8'd205; 5'd26: ex5 = 8'd213; 5'd27: ex5 = 8'd221;
            5'd28: ex5 = 8'd230; 5'd29: ex5 = 8'd238; 5'd30: ex5 = 8'd246; default: ex5 = 8'd254;
        endcase
    endfunction
    function automatic logic [7:0] ex6(input logic [5:0] v);
        case (v)
            6'd0: ex6 = 8'd0; 6'd1: ex6 = 8'd4; 6'd2: ex6 = 8'd8; 6'd3: ex6 = 8'd12;
            6'd4: ex6 = 8'd16; 6'd5: ex6 = 8'd20; 6'd6: ex6 = 8'd24; 6'd7: ex6 = 8'd28;
            6'd8: ex6 = 8'd32; 6'd9: ex6 = 8'd36; 6'd10: ex6 = 8'd40; 6'd11: ex6 = 8'd44;
            6'd12: ex6 = 8'd48; 6'd13: ex6 = 8'd52; 6'd14: ex6 = 8'd56; 6'd15: ex6 = 8'd60;
            6'd16: ex6 = 8'd64; 6'd17: ex6 = 8'd68; 6'd18: ex6 = 8'd72; 6'd19: ex6 = 8'd76;
            6'd20: ex6 = 8'd80; 6'd21: ex6 = 8'd84; 6'd22: ex6 = 8'd89; 6'd23: ex6 = 8'd93;
            6'd24: ex6 = 8'd97; 6'd25: ex6 = 8'd101; 6'd26: ex6 = 8'd105; 6'd27: ex6 = 8'd109;
            6'd28: ex6 = 8'd113; 6'd29: ex6 = 8'd117; 6'd30: ex6 = 8'd121; 6'd31: ex6 = 8'd125;
            6'd32: ex6 = 8'd129; 6'd33: ex6 = 8'd133; 6'd34: ex6 = 8'd137; 6'd35: ex6 = 8'd141;
            6'd36: ex6 = 8'd145; 6'd37: ex6 = 8'd149; 6'd38: ex6 = 8'd153; 6'd39: ex6 = 8'd157;
            6'd40: ex6 = 8'd161; 6'd41: ex6 = 8'd165; 6'd42: ex6 = 8'd169; 6'd43: ex6 = 8'd174;
            6'd44: ex6 = 8'd178; 6'd45: ex6 = 8'd182; 6'd46: ex6 = 8'd186; 6'd47: ex6 = 8'd190;
            6'd48: ex6 = 8'd194; 6'd49: ex6 = 8'd198; 6'd50: ex6 = 8'd202; 6'd51: ex6 = 8'd206;
            6'd52: ex6 = 8'd210; 6'd53: ex6 = 8'd214; 6'd54: ex6 = 8'd218; 6'd55: ex6 = 8'd222;
            6'd56: ex6 = 8'd226; 6'd57: ex6 = 8'd230; 6'd58: ex6 = 8'd234; 6'd59: ex6 = 8'd238;
            6'd60: ex6 = 8'd242; 6'd61: ex6 = 8'd246; 6'd62: ex6 = 8'd250; default: ex6 = 8'd254;
        endcase
    endfunction
    // ⌊p/255⌋ = (p + 1 + p/256)/256 для p ≤ 255·255
    function automatic logic [7:0] d255(input logic [17:0] p);
        d255 = 8'((p[15:0] + 17'd1 + {9'd0, p[15:8]}) >> 8);
    endfunction

    // раскрытие каналов — в Q6 (разряды текселя или запись палитры), в P5 — готовые каналы
    reg [7:0] t_r, t_g, t_b, t_a;
    wire [15:0] pl_h  = q6_ph ? plq[31:16] : plq[15:0];
    wire [15:0] p5_dr = k_pal ? pl_h : q6_raw;
    wire [4:0]  d_fmt = j_dfmt;
    always_comb begin
        t_r = 8'd0; t_g = 8'd0; t_b = 8'd0; t_a = 8'd0;
        if (q6_inok && j_p8) begin
            // PALETTED8: запись ARGB8888 (B, G, R, A в памяти)
            t_a = plq[31:24]; t_r = plq[23:16]; t_g = plq[15:8]; t_b = plq[7:0];
        end else if (q6_inok) case (d_fmt)
            5'd1:  begin t_r = 8'd255; t_g = 8'd255; t_b = 8'd255; t_a = p5_dr[0] ? 8'd255 : 8'd0; end
            5'd17: begin t_r = 8'd255; t_g = 8'd255; t_b = 8'd255; t_a = 8'(85 * p5_dr[1:0]); end
            5'd2:  begin t_r = 8'd255; t_g = 8'd255; t_b = 8'd255; t_a = exa4(p5_dr[3:0]); end
            5'd3:  begin t_r = 8'd255; t_g = 8'd255; t_b = 8'd255; t_a = p5_dr[7:0]; end
            5'd0:  begin t_r = ex5(p5_dr[14:10]); t_g = ex5(p5_dr[9:5]); t_b = ex5(p5_dr[4:0]);
                         t_a = p5_dr[15] ? 8'd255 : 8'd0; end
            5'd4:  begin t_r = ex3(p5_dr[7:5]); t_g = ex3(p5_dr[4:2]); t_b = 8'(85 * p5_dr[1:0]);
                         t_a = 8'd255; end
            5'd5:  begin t_r = 8'(85 * p5_dr[5:4]); t_g = 8'(85 * p5_dr[3:2]);
                         t_b = 8'(85 * p5_dr[1:0]); t_a = 8'(85 * p5_dr[7:6]); end
            5'd6:  begin t_r = ex4(p5_dr[11:8]); t_g = ex4(p5_dr[7:4]); t_b = ex4(p5_dr[3:0]);
                         t_a = exa4(p5_dr[15:12]); end
            5'd7:  begin t_r = ex5(p5_dr[15:11]); t_g = ex6(p5_dr[10:5]); t_b = ex5(p5_dr[4:0]);
                         t_a = 8'd255; end
            default: ;
        endcase
    end

    // вторая точка пары: те же форматы (кроме L1/L2/L4 — у пары тексель не меньше байта)
    reg [7:0] t_r2, t_g2, t_b2, t_a2;
    wire [15:0] pl_h2  = q6_ph2 ? plq2[31:16] : plq2[15:0];
    wire [15:0] p5_dr2 = k_pal ? pl_h2 : q6_raw2;
    always_comb begin
        t_r2 = 8'd0; t_g2 = 8'd0; t_b2 = 8'd0; t_a2 = 8'd0;
        if (q6_inok && j_p8) begin
            t_a2 = plq2[31:24]; t_r2 = plq2[23:16]; t_g2 = plq2[15:8]; t_b2 = plq2[7:0];
        end else if (q6_inok) case (d_fmt)
            5'd3:  begin t_r2 = 8'd255; t_g2 = 8'd255; t_b2 = 8'd255; t_a2 = p5_dr2[7:0]; end
            5'd0:  begin t_r2 = ex5(p5_dr2[14:10]); t_g2 = ex5(p5_dr2[9:5]); t_b2 = ex5(p5_dr2[4:0]);
                         t_a2 = p5_dr2[15] ? 8'd255 : 8'd0; end
            5'd4:  begin t_r2 = ex3(p5_dr2[7:5]); t_g2 = ex3(p5_dr2[4:2]); t_b2 = 8'(85 * p5_dr2[1:0]);
                         t_a2 = 8'd255; end
            5'd5:  begin t_r2 = 8'(85 * p5_dr2[5:4]); t_g2 = 8'(85 * p5_dr2[3:2]);
                         t_b2 = 8'(85 * p5_dr2[1:0]); t_a2 = 8'(85 * p5_dr2[7:6]); end
            5'd6:  begin t_r2 = ex4(p5_dr2[11:8]); t_g2 = ex4(p5_dr2[7:4]); t_b2 = ex4(p5_dr2[3:0]);
                         t_a2 = exa4(p5_dr2[15:12]); end
            5'd7:  begin t_r2 = ex5(p5_dr2[15:11]); t_g2 = ex6(p5_dr2[10:5]); t_b2 = ex5(p5_dr2[4:0]);
                         t_a2 = 8'd255; end
            default: ;
        endcase
    end

    // BILINEAR: слагаемое прохода ⌊тексель·w / 256⌋ — в P5, от регистров p5_t и p5_w (за
    // раскрытием каналов в Q6 умножение было бы длинным путём); сумма проходов — b_acc.
    // Без BILINEAR w = 256, второй вес 0, b_acc 0: b_sum — сам тексель (1.28: выбора между
    // b_sum и p5_t перед умножением на цвет нет — он стоял на худшем пути clk_mem)
    reg  [8:0]  p5_w;
    wire [17:0] bp_a, bp_r, bp_g, bp_b;
    ft_mul9 u_bil_a (.clk(clk), .a({1'b0, p5_t[31:24]}), .b(p5_w), .p(bp_a));
    ft_mul9 u_bil_r (.clk(clk), .a({1'b0, p5_t[23:16]}), .b(p5_w), .p(bp_r));
    ft_mul9 u_bil_g (.clk(clk), .a({1'b0, p5_t[15:8]}),  .b(p5_w), .p(bp_g));
    ft_mul9 u_bil_b (.clk(clk), .a({1'b0, p5_t[7:0]}),   .b(p5_w), .p(bp_b));
    wire [7:0]  bw_a = bp_a[15:8];
    wire [7:0]  bw_r = bp_r[15:8];
    wire [7:0]  bw_g = bp_g[15:8];
    wire [7:0]  bw_b = bp_b[15:8];
    // второй тексель пары (вес 0, если пары нет)
    reg  [8:0]  p5_w2;
    wire [17:0] bq_pa, bq_pr, bq_pg, bq_pb;
    ft_mul9 u_bil2_a (.clk(clk), .a({1'b0, p5_t2[31:24]}), .b(p5_w2), .p(bq_pa));
    ft_mul9 u_bil2_r (.clk(clk), .a({1'b0, p5_t2[23:16]}), .b(p5_w2), .p(bq_pr));
    ft_mul9 u_bil2_g (.clk(clk), .a({1'b0, p5_t2[15:8]}),  .b(p5_w2), .p(bq_pg));
    ft_mul9 u_bil2_b (.clk(clk), .a({1'b0, p5_t2[7:0]}),   .b(p5_w2), .p(bq_pb));
    reg  [31:0] b_acc;                  // сумма прошлых проходов точки (≤ 255 на канал)
    wire [31:0] b_sum = {8'(b_acc[31:24] + bw_a + bq_pa[15:8]), 8'(b_acc[23:16] + bw_r + bq_pr[15:8]),
                         8'(b_acc[15:8] + bw_g + bq_pg[15:8]),  8'(b_acc[7:0] + bw_b + bq_pb[15:8])};
    wire [31:0] p5_tx = b_sum;
    // P5 уходит из стадии: вперёд (fe_adv) или стоим, а фрагмент принят / проход без фрагмента
    wire        p5_take = p5_v && (fe_adv || ((f_ready || !p5_emit) && !sp_need_w));
    always_ff @(posedge clk or negedge rst_n)
        if (!rst_n) b_acc <= '0;
        else if (abort) b_acc <= '0;
        else if (p5_take && j_bil) b_acc <= p5_emit ? 32'd0 : b_sum;

    // выход: цвет фрагмента (у битмапа — тексель и COLOR: умножает ft_rd_pix)
    // альфа фигуры — ⌊COLOR_A · покрытие / 4096⌋ (RECTS r < 16 — без округления до 1/256)
    wire [20:0] cov_m = {13'd0, j_ca} * {8'd0, p5_num};
    // Пара на P5: вторая точка с частичной альфой идёт следующим тактом отдельно (sp2), тогда
    // конвейер стоит такт; её цвет — сам тексель (× 255)
    wire        sp2     = sp2_q;
    wire        p5_s2   = p5_t2[31:24] == 8'd0 || p5_t2[31:24] == 8'd255;
    wire        sp_need = sp_need_w;                                  // первая уходит, вторая — потом
    always_ff @(posedge clk or negedge rst_n)
        if (!rst_n) sp2_q <= 1'b0;
        else if (abort) sp2_q <= 1'b0;
        else if (sp_need && f_ready) sp2_q <= 1'b1;                    // первая принята
        else if (sp2_q && f_ready) sp2_q <= 1'b0;                      // вторая принята
    assign f_v    = p5_emit;
    assign f_x    = {p5_r[TH_LOG-1:0], p5_x[TW_LOG-1:1], p5_x[0] | sp2};
    assign f_p2   = p5_v && p5_p2 && !j_bil && p5_s2 && !sp2;
    assign f_w2   = p5_t2[31:24] == 8'd255;
    assign f_rgba2 = p5_t2;
    assign f_slot = j_slot;
    assign f_clr  = p5_kind_clr;
    assign f_xn   = sp_need ? {p5_r[TH_LOG-1:0], p5_x[TW_LOG-1:1], 1'b1} : {q6_r[TH_LOG-1:0], q6_x[TW_LOG-1:0]};
    assign f_p2n  = sp_need ? 1'b0 : q6_p2 && !j_bil;
    assign f_adv  = fe_adv || sp_need;
    assign f_raw  = !sp2 && !p5_kind_clr && p5_kind_bmp;
    assign f_rgba = f_raw ? {p5_tx, j_ca, j_rgb} :
                    {32'd0, sp2 ? p5_t2 : p5_kind_clr ? {j_ca, j_rgb} : {cov_m[19:12], j_rgb}};

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            p0_v <= 1'b0; p1_v <= 1'b0; p2_v <= 1'b0; p3_v <= 1'b0; p4_v <= 1'b0; p5_v <= 1'b0;
            p0_p2 <= 1'b0; p1_p2 <= 1'b0; p2_p2 <= 1'b0; p3_p2 <= 1'b0; p4_p2 <= 1'b0;
            q5_p2 <= 1'b0; q6_p2 <= 1'b0; p5_p2 <= 1'b0; p5_t2 <= '0; q6_hit2 <= 1'b0;
            p0_x <= '0; p1_x <= '0; p2_x <= '0; p3_x <= '0; p4_x <= '0; p5_x <= '0;
            p0_r <= '0; p1_r <= '0; p2_r <= '0; p3_r <= '0; p4_r <= '0; p5_r <= '0; q5_r <= '0; q6_r <= '0;
            p0_in <= 1'b0; p1_in <= 1'b0; p2_in <= 1'b0; p3_in <= 1'b0; p4_in <= 1'b0;
            p0_ty <= '0; p0_col <= '0;
            p0_sub <= '0; p1_sub <= '0; p2_sub <= '0; p3_sub <= '0; p4_sub <= '0;
            p1_bc <= '0; p1_ro <= '0; p2_a <= '0; p3_a <= '0; p4_a <= '0;
            p2_oob <= 1'b0; p3_oob <= 1'b0; p4_oob <= 1'b0;
            p2_ramp <= 1'b0; p3_ramp <= 1'b0; p4_ramp <= 1'b0; p2_rom <= 1'b0;
            p3_wa <= '0; p4_wa <= '0; p5_t <= '0; p5_w <= '0; p5_w2 <= '0; p3_ew <= '0; p4_ew <= '0;
            p0_bt <= 1'b0; p1_bt <= 1'b0; bq_rt <= '0; bq_rb <= '0;
            p0_cn <= 1'b0; p1_cn <= 1'b0; p2_cn <= 1'b0; p3_cn <= 1'b0; p4_cn <= 1'b0;
            p0_ix <= '0; p0_ax <= 1'b0; p1_ax <= 1'b0;
            p1_aio <= '0; p2_cnc <= '0; p0_alo <= '0; p0_ahi <= '0;
            p1_ix2 <= '0; p2_q <= '0; p3_q <= '0; p4_q <= '0;
            p2_frag <= 1'b0; p3_frag <= 1'b0; p4_frag <= 1'b0;
            p2_cls <= '0; p3_cls <= '0; p4_cls <= '0; p5_num <= '0;
            p2_m <= '0; p2_k16 <= 1'b0; p3_num <= '0; p4_num <= '0;
            q5_v <= 1'b0; q6_v <= 1'b0; q5_x <= '0; q6_x <= '0; q5_raw <= '0; q6_raw <= '0;
            q5_inok <= 1'b0; q6_inok <= 1'b0; q5_pw <= '0; q6_pw <= '0; q5_ph <= 1'b0; q6_ph <= 1'b0;
            q5_num <= '0; q6_num <= '0; q6_hit <= 1'b0;
            p5_kind_bmp <= 1'b0; p5_kind_clr <= 1'b0;
        end else if (abort) begin
            p0_v <= 1'b0; p1_v <= 1'b0; p2_v <= 1'b0; p3_v <= 1'b0; p4_v <= 1'b0; p5_v <= 1'b0;
            q5_v <= 1'b0; q6_v <= 1'b0;
        end else if (!fe_adv) begin
            // P0…Q6 стоят (кэш текселей или палитры, корень); фрагмент P5, принятый
            // ft_rd_pix, уходит; строка палитры записана — запись Q6 в кэше
            if (p5_v && (f_ready || !p5_emit) && !sp_need_w) p5_v <= 1'b0;
            // пришла строка палитры: попадание — у той точки, чья строка (у пары их может быть две)
            if (pl_tag_we && dn_line[16:0] == q6_pw[19:3])  q6_hit  <= 1'b1;
            if (pl_tag_we && dn_line[16:0] == q6_pw2[19:3]) q6_hit2 <= 1'b1;
        end else begin
            // P0
            p0_v   <= issue;
            p0_p2  <= issue && (p2_go || bp_now);
            p0_d2  <= bp_now ? (i_b16 ? 3'sd2 : 3'sd1) : 3'(i_dc);
            p0_w2  <= bp_now ? b_w2 : 9'd0;
            p0_bt  <= issue && j_bil && p2_first && j_k == 2'd0;
            p0_x   <= j_x[9:0];
            p0_r   <= j_row;
            p0_in  <= j_bil ? b_in : i_in;
            p0_ty  <= j_bil ? b_ty[10:0] : i_ty[10:0];
            p0_col <= j_bil ? b_col : i_col;
            p0_sub <= j_bil ? b_sub : i_sub;
            p0_w   <= j_bil ? b_w : 9'd256;   // без BILINEAR: P5 даёт сам тексель (b_sum)
            p0_k   <= j_k;
            if (j_kind == J_RECT) begin
                p0_cn  <= i_cnr;
                p0_ix  <= i_dxr[13:0];
                p0_ax  <= 1'b0;
                p0_alo <= i_alo;
                p0_ahi <= i_ahi;
            end else begin
                p0_cn  <= 1'b1;
                p0_ix  <= i_axp;
                p0_ax  <= j_dy0 && i_axp == j_o;
            end
            // P1
            p1_v   <= p0_v;
            p1_w   <= p0_w;   p1_k <= p0_k;   p1_w2 <= p0_w2;   p1_bt <= p0_bt;
            // первая точка строки, проход k = 0 (на P1): байт начала строк текстуры в строке
            // кэша — верх (base + ty·stride) и низ (+ stride: REPEAT по y у пар не бывает)
            if (p1_v && p1_bt) begin
                bq_rt <= j_base[4:0] + p1_ro[4:0];
                bq_rb <= j_base[4:0] + p1_ro[4:0] + j_stride[4:0];
            end
            p1_p2  <= p0_p2;
            p1_d2  <= p0_d2;
            p1_x   <= p0_x;
            p1_r   <= p0_r;
            p1_in  <= p0_in;
            p1_sub <= p0_sub;
            p1_bc  <= j_base + {10'd0, p0_col};
            p1_ro  <= p0_ty * j_stride;
            p1_cn  <= p0_cn;
            p1_ax  <= p0_ax;
            p1_aio <= axis_ov(p0_alo, p0_ahi);
            p1_ix2 <= p0_ix * p0_ix;
            // P2
            p2_v   <= p1_v;
            p2_w   <= p1_w;   p2_k <= p1_k;   p2_w2 <= p1_w2;
            p2_p2  <= p1_p2;
            p2_d2  <= p1_d2;
            p2_x   <= p1_x;
            p2_r   <= p1_r;
            p2_in  <= p1_in;
            p2_sub <= p1_sub;
            begin : p2_addr
                logic [21:0] a;
                a = p1_bc + p1_ro[21:0];
                p2_a    <= a;
                p2_oob  <= a >= 22'h300000;
                p2_rom  <= !ramg_3mb && a >= 22'h200000;
                p2_ramp <= !ramg_3mb && a >= 22'h200064 && a < 22'h200268;
            end
            p2_cn  <= p1_cn;
            begin : p2_axis
                logic [8:0] ax;
                ax = axis_cov2(p1_aio, j_s);
                p2_cnc <= (j_kind == J_RECT && !p1_cn) ? (ax < j_cy ? ax : j_cy) : 9'd0;
            end
            // RECT r < 16 (замер bt8xxemu): покрытие ox·oy·k/4096, k = 16, если по какой-то оси
            // фигура целиком в полосе пикселя, иначе r
            p2_m   <= 9'(p1_aio[4:0] * j_cyio[4:0]);
            p2_k16 <= p1_aio[5] || j_cyio[5];
            begin : p2_cov
                logic [28:0] q;
                q = {1'b0, p1_ix2} + {1'b0, j_dy2};
                p2_q    <= q;
                // RECT вне угла — фрагмент всегда; угол и точка — в круге r + 16
                p2_frag <= (!p1_cn && j_kind == J_RECT) || (q <= {1'b0, j_osq} && !p1_ax);
                if (j_rz || q >= {1'b0, j_r8sq})               p2_cls <= 2'd0;
                else if (j_kok && q <= {1'b0, j_kq})            p2_cls <= 2'd1;
                else                                            p2_cls <= 2'd2;
            end
            // P3
            p3_v    <= p2_v;
            p3_w    <= p2_w;  p3_k <= p2_k;   p3_w2 <= p2_w2;
            p3_p2   <= p2_p2;
            p3_d2   <= p2_d2;
            p3_x    <= p2_x;
            p3_r    <= p2_r;
            p3_in   <= p2_in;
            p3_sub  <= p2_sub;
            p3_a    <= p2_a;
            p3_oob  <= p2_oob;
            p3_ramp <= p2_ramp;
            p3_wa   <= p2_rom ? 21'(p2_a[21:2]) + 21'h048000 : {1'b0, p2_a[21:2]};
            p3_ew   <= p2_a[4:3] + {1'b0, p2_a[2]};
            p3_cn   <= p2_cn;
            p3_num  <= !j_thin ? {p2_cnc, 4'd0} :
                       (p2_k16 ? {p2_m, 4'd0} : 13'(p2_m * j_r[3:0]));
            p3_q    <= p2_q;
            p3_frag <= p2_frag;
            p3_cls  <= p2_cls;
            // P4
            p4_v    <= p3_v;
            p4_w    <= p3_w;  p4_k <= p3_k;   p4_w2 <= p3_w2;
            p4_p2   <= p3_p2;
            p4_d2   <= p3_d2;
            p4_x    <= p3_x;
            p4_r    <= p3_r;
            p4_in   <= p3_in;
            p4_sub  <= p3_sub;
            p4_a    <= p3_a;
            p4_oob  <= p3_oob;
            p4_ramp <= p3_ramp;
            p4_wa   <= p3_wa;
            p4_ew   <= p3_ew;
            p4_cn   <= p3_cn;
            p4_num  <= p3_num;
            p4_q    <= p3_q;
            p4_frag <= p3_frag;
            p4_cls  <= p3_cls;
            // Q5 (фигуры без фрагмента выпадают здесь): адрес записи палитры — индекс·2
            // (565/4444) или ·4 (PALETTED8) от PALETTE_SOURCE задания
            q5_v    <= p4_v && (k_bmp || j_kind == J_CLR || p4_frag);
            q5_x    <= p4_x;
            q5_r    <= p4_r;
            q5_raw  <= t_raw;
            q5_w    <= p4_w;  q5_k <= p4_k;   q5_w2 <= p4_w2;
            q5_p2   <= p4_p2;
            q5_raw2 <= t_raw2;
            begin : q5_pal2
                logic [21:0] pa;
                pa = j_pal + (j_fmt == 5'd16 ? {12'd0, t_raw2[7:0], 2'b00} : {13'd0, t_raw2[7:0], 1'b0});
                q5_pw2 <= pa[21:2];
                q5_ph2 <= pa[1];
            end
            q5_inok <= p4_in && !p4_oob;
            begin : q5_pal
                logic [21:0] pa;
                pa = j_pal + (j_fmt == 5'd16 ? {12'd0, t_raw[7:0], 2'b00} : {13'd0, t_raw[7:0], 1'b0});
                q5_pw <= pa[21:2];
                q5_ph <= pa[1];
            end
            q5_num  <= !p4_cn ? p4_num : (p4_cls == 2'd0 ? 13'd0 :
                       p4_cls == 2'd1 ? 13'd4096 : {band_cov, 4'd0});
            // Q6: чтение кэша палитры (адрес — Q5), попадание
            q6_v    <= q5_v;
            q6_x    <= q5_x;
            q6_r    <= q5_r;
            q6_raw  <= q5_raw;
            q6_w    <= q5_w;  q6_k <= q5_k;   q6_w2 <= q5_w2;
            q6_p2   <= q5_p2;
            q6_raw2 <= q5_raw2;
            q6_pw2  <= q5_pw2;
            q6_ph2  <= q5_ph2;
            q6_hit2 <= q5_hit2;
            q6_inok <= q5_inok;
            q6_pw   <= q5_pw;
            q6_ph   <= q5_ph;
            q6_num  <= q5_num;
            q6_hit  <= q5_hit;
            // P5: каналы (раскрыты в Q6)
            p5_v        <= q6_v;
            p5_x        <= q6_x;
            p5_r        <= q6_r;
            p5_t        <= {t_a, t_r, t_g, t_b};
            p5_w        <= q6_w;
            p5_w2       <= q6_w2;
            p5_k        <= q6_k;
            p5_t2       <= {t_a2, t_r2, t_g2, t_b2};
            p5_p2       <= q6_p2;
            p5_kind_bmp <= k_bmp;
            p5_kind_clr <= j_kind == J_CLR;
            p5_num      <= q6_num;
        end
    end

    // --- заполнение кэша ---------------------------------------------------------------------
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            tx_req <= 1'b0; tx_addr <= '0; clr_n <= 10'd512;
            pf_v <= 1'b0; pf_line <= '0; pf_src <= '0; sp_v <= 1'b0; sp_line <= '0;
            nr_v <= 1'b0; nr_line <= '0; pf_q <= 1'b0; pf_lq <= '0;
            for (int k = 0; k < 2; k++) begin
                fs_v[k] <= 1'b0; fs_pal[k] <= 1'b0; fs_line[k] <= '0;
            end
            fs_d <= 1'b0; fs_s <= 1'b0; fs_a <= 1'b0; fs_ns <= '0; rc_n <= '0; rc_line <= '0;
            dn_v <= 1'b0; dn_pal <= 1'b0; dn_line <= '0;
        end else begin
            if (clr_on) clr_n <= clr_n - 10'd1;
            if (start && xs_q == X_IDLE) begin
                clr_n  <= 10'd512;                 // кадр: RAM_G могла измениться
                pf_v   <= 1'b0;
                sp_v   <= 1'b0;
                nr_v   <= 1'b0;
            end
            pf_q  <= f_pf_dec;
            pf_lq <= t2_line;
            // кандидат предвыборки снят: заказан или уже в кэше (начало пролёта — первым)
            if (f_go_pfq) begin
                if (sp_v)      sp_v <= 1'b0;
                else if (nr_v) nr_v <= 1'b0;
                else           pf_v <= 1'b0;
            end
            // заказ: место fs_a, метка строки-цели сброшена в этот такт (t_we), заявка — в очередь
            if (f_go_pal || f_go_dem || f_go_pf) begin
                fs_v[fs_a]    <= 1'b1;
                fs_pal[fs_a]  <= f_go_pal;
                fs_line[fs_a] <= f_go_pal ? {1'b0, q6_pl} : f_go_dem ? p4_line : pf_lq;
                fs_a          <= !fs_a;
            end
            // заявки — по порядку мест; tx_req держится, пока есть неподанные
            begin : fs_send
                logic [1:0] ns;
                ns = fs_ns + {1'b0, f_go_pal || f_go_dem || f_go_pf} - {1'b0, tx_req && tx_ack};
                fs_ns <= ns;
                if (tx_req && tx_ack) fs_s <= !fs_s;
                begin : nxt
                    logic s;
                    s = (tx_req && tx_ack) ? !fs_s : fs_s;
                    tx_req <= ns != 2'd0;
                    // адрес следующей заявки: место s (если оно заказано в этот же такт — его строка)
                    if (s == fs_a && (f_go_pal || f_go_dem || f_go_pf))
                        tx_addr <= {f_go_pal ? {1'b0, q6_pl} : f_go_dem ? p4_line : pf_lq, 3'd0};
                    else
                        tx_addr <= {fs_line[s], 3'd0};
                end
            end
            // последнее слово старшего места: строка пришла; метка — следующим тактом (dn_v)
            if (rc_n != 2'd0) rc_n <= rc_n - 2'd1;
            dn_v       <= 1'b0;
            if (fs_done && fs_v[fs_d]) begin
                fs_v[fs_d] <= 1'b0;
                fs_d       <= !fs_d;
                rc_line    <= f_dline;
                rc_n       <= 2'd3;
                dn_v       <= 1'b1;
                dn_pal     <= f_dpal;
                dn_line    <= f_dline;
            end
            if (sp_req) begin
                sp_v    <= 1'b1;
                sp_line <= sp_req_line;
            end
            // строка той же точки строкой плитки ниже (P2N) — когда сменилась
            if (n2_v && n2_line != nr_line) begin
                nr_v    <= 1'b1;
                nr_line <= n2_line;
            end
            // кандидат предвыборки: строка после той, в которую попала точка P4
            if (p4_need && c_hit && p4_wa[20:3] != pf_src) begin
                pf_src  <= p4_wa[20:3];
                pf_line <= p4_wa[20:3] + 18'd1;
                pf_v    <= 1'b1;
            end
        end
    end

    // --- корень: ⌊√(q·256)⌋ по разряду за такт ---------------------------------------------------
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            sq_run <= 1'b0; sq_done <= 1'b0; sq_i <= '0; sq_n <= '0; sq_rem <= '0; sq_root <= '0;
        end else if (fe_adv || abort) begin
            sq_run <= 1'b0;
            sq_done <= 1'b0;
        end else if (p4_band && !sq_run && !sq_done) begin
            sq_run  <= 1'b1;
            sq_i    <= 5'd18;
            sq_n    <= {1'b0, p4_q, 8'd0};
            sq_rem  <= '0;
            sq_root <= '0;
        end else if (sq_run) begin : step
            logic [21:0] r, t;
            r = {sq_rem, sq_n[37:36]};
            t = {1'b0, sq_root, 2'b01};
            sq_n <= {sq_n[35:0], 2'b00};
            if (r >= t) begin
                sq_rem  <= 20'(r - t);
                sq_root <= {sq_root[17:0], 1'b1};
            end else begin
                sq_rem  <= 20'(r);
                sq_root <= {sq_root[17:0], 1'b0};
            end
            if (sq_i == 5'd0) begin
                sq_run  <= 1'b0;
                sq_done <= 1'b1;
            end else sq_i <= sq_i - 5'd1;
        end
    end
endmodule
