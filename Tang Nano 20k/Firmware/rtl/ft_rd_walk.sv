// SPDX-License-Identifier: Apache-2.0
//
// Растеризатор FT812, обходчик дисплей-листа (ft_render). Кадр рисуется плитками 128×8 (TW_LOG,
// TH_LOG; 1024 точки)
// (слева направо, сверху вниз; у правого и нижнего края — сколько осталось): на каждой
// плитке лист выполняется с начала, как у FT812 на каждой строке. Для каждой точки
// экрана примитивы идут в порядке листа, как у чипа, — картинка та же; а тексели, нужные
// плитке, лежат кучно при любом повороте и масштабе битмапа, и кэш текселей исполнителя
// держит их (строками экрана повёрнутые битмапы задевали сотни строк текстуры на строку).
//
// Обходчик читает RAM_DL по слову за такт, ведёт свою часть контекста (VERTEX_FORMAT,
// VERTEX_TRANSLATE, BITMAP_HANDLE, CELL, POINT_SIZE, LINE_WIDTH), таблицу битмапов
// (BITMAP_SOURCE/LAYOUT/SIZE — состояние ручек живёт между плитками и кадрами), стек
// CALL/RETURN и пары вершин RECTS. Вершина, чья фигура не задевает плитку, отбрасывается
// здесь; задевающая уходит исполнителю (ft_rd_exec) заданием с готовыми параметрами и
// строками плитки, которые она задевает: BITMAPS — одним заданием на все свои строки
// (исполнитель идёт их подряд, без слива конвейера: пролёт по x у битмапа от строки не
// зависит), RECTS и POINTS — заданием на каждую строку (покрытие зависит от y). Команды
// цвета, смешения, трафарета, ножниц, преобразования идут исполнителю как есть, в порядке
// листа, по разу; CLEAR — одним заданием на все строки плитки.
//
// Правила строк — как у модели scripts/ft_render_model.py (замеры bt8xxemu):
//   BITMAPS — 0 ≤ y·16 − vy < высота·16;  POINTS — |y·16 − vy| < размер + 16;
//   RECTS — расстояние строки до ядра по y меньше LINE_WIDTH + 8.
// По x фигура проверяется с запасом (пролёт точно режет исполнитель).
// Плитка кончается на DISPLAY, на RETURN без CALL, на CALL глубже 4, после слова 2047 и
// после 16384 слов (как строка у модели). LINES и LINE_STRIP — отрезками вдоль осей (как
// RECTS); наклонные отрезки, EDGE_STRIP и MACRO пока не рисуются (флаги unsup).
//
// Задание (it_data, 136 бит), [2:0] — вид, [130:127] — первая строка плитки (0…15),
// [135:131] — после последней (1…16):
//   0 STATE  [34:3] слово листа
//   1 BMP    [22:3] vx, [42:23] vy (1/16 точки, со знаком), [64:43] SOURCE, [69:65] формат,
//            [81:70] stride, [92:82] высота раскладки, [93] фильтр, [94] wrap x, [95] wrap y,
//            [107:96] ширина, [119:108] высота (1…2048), [126:120] CELL
//   2 RECT   [22:3] x0, [42:23] y0, [62:43] x1, [82:63] y1 (x0 ≤ x1, y0 ≤ y1), [94:83] LINE_WIDTH
//   3 POINT  [22:3] vx, [42:23] vy, [55:43] POINT_SIZE
//   4 EOL    конец плитки
//
// Конвейер: выборка (адрес RAM_DL из регистра, слова — в очередь на 2 места) → D (разбор,
// контекст, вершина, таблица ручек) → E1 (задание, границы фигуры) → E1b (строки плитки,
// которые задевает фигура, проверка по x) → E2 (запись в очередь заданий; RECTS и POINTS —
// по строке за такт). Переход
// CALL/JUMP/RETURN сбрасывает выборку — 2 пустых такта; слово листа за такт в остальном.
module ft_rd_walk #(
    parameter int TW_LOG = 7,               // плитка: 2^TW_LOG × 2^TH_LOG точек (1024)
    parameter int TH_LOG = 3
) (
    input  wire         clk,
    input  wire         rst_n,
    input  wire         abort,          // растеризатор выключен: всё в исходное
    input  wire         start,          // такт: новый кадр
    input  wire [9:0]   lines,          // строк в кадре (1…768)
    input  wire [10:0]  hsize,          // точек в строке (1…1024)
    output wire         ready,          // таблица ручек очищена после сброса

    output wire [10:0]  dl_addr,        // RAM_DL показываемого банка, ответ через такт
    input  wire [31:0]  dl_q,

    output wire         it_v,           // такт: задание в очередь (пишется прямо из E2)
    output wire [135:0] it_data,
    input  wire         it_full,        // очередь не примет

    output reg          busy,           // кадр идёт (лист читается)
    output reg  [3:0]   unsup           // липкие: 0 линии, 1 MACRO, 2 стек CALL, 3 предел шагов
);
    localparam logic [2:0] K_STATE = 3'd0, K_BMP = 3'd1, K_RECT = 3'd2, K_POINT = 3'd3,
                           K_EOL = 3'd4;

    // --- контекст обходчика ------------------------------------------------------------------
    reg  [2:0]  vfmt;
    reg  [4:0]  handle;
    reg  [6:0]  vcell;
    reg  [16:0] tr_x, tr_y;
    reg  [12:0] psize;
    reg  [11:0] lwidth;
    // смешение и трафарет — только чтобы отбросить BITMAPS с пустой раскладкой (stride или
    // высота раскладки 0: все тексели — прозрачный чёрный), если она ничего не меняет: множитель
    // приёмника ONE или ONE_MINUS_SRC_ALPHA, трафарет не пишется (маска 0 или обе операции KEEP).
    // Так ручка, которую игра не задала (размер 0 = 2048×2048), не стоит пролёта через весь экран.
    reg  [2:0]  bdst;
    reg  [2:0]  sfail, spass;
    reg  [7:0]  swm;
    reg  [3:0]  prim;
    reg         odd;                    // RECTS: ждём вторую вершину пары
    reg  [19:0] px, py;                 // предыдущая вершина

    localparam int CTXW = 3 + 5 + 7 + 17 + 17 + 13 + 12 + 3 + 3 + 3 + 8;    // 88
    wire [CTXW-1:0] ctx_now = {vfmt, handle, vcell, tr_x, tr_y, psize, lwidth, bdst, sfail, spass, swm};
    localparam logic [CTXW-1:0] CTX_DEF = {3'd4, 5'd0, 7'd0, 17'd0, 17'd0, 13'd16, 12'd16,
                                           3'd4, 3'd1, 3'd1, 8'hFF};

    // SAVE_CONTEXT/RESTORE_CONTEXT: 4 места по кругу, переполнение вытесняет старое
    reg  [CTXW-1:0] sv_mem [0:3];
    reg  [1:0]  sv_sp;
    reg  [2:0]  sv_cnt;

    // CALL/RETURN
    reg  [10:0] cs_mem [0:3];
    reg  [2:0]  cs_cnt;

    // --- таблица ручек: поля по командам (каждая команда пишет своё поле целиком) ------------
    reg  [21:0] h_src  [0:31];
    reg  [23:0] h_lay  [0:31];          // формат, stride[9:0], высота[8:0]
    reg  [3:0]  h_layh [0:31];          // stride[11:10], высота[10:9]
    reg  [20:0] h_siz  [0:31];          // фильтр, wrap x, wrap y, ширина[8:0], высота[8:0]
    reg  [3:0]  h_sizh [0:31];          // ширина[10:9], высота[10:9]
    reg  [5:0]  h_clr;                  // очистка после сброса: [5] — идёт
    assign ready = !h_clr[5];

    // --- кадр и плитка ------------------------------------------------------------------------
    reg  [9:0]  ty0, tx0, n_lines;      // верхняя строка и левая точка плитки
    reg  [10:0] n_h;
    reg  [14:0] steps;
    reg         d_fend;                 // слово 2047 разобрано: дальше — конец плитки
    reg         eol_wait;               // конец плитки отдан в E, ждём его ухода
    wire [10:0] t_left = {1'b0, n_lines} - {1'b0, ty0};
    localparam int TW = 1 << TW_LOG, TH = 1 << TH_LOG;
    wire [4:0]  t_rows = t_left >= 11'(TH) ? 5'(TH) : t_left[4:0];   // строк в плитке
    wire        t_last_x = {1'b0, tx0} + 11'(TW) >= n_h;            // плитка у правого края
    wire        t_last_y = {1'b0, ty0} + 11'(TH) >= {1'b0, n_lines};

    // --- выборка -----------------------------------------------------------------------------
    reg  [10:0] fpc;                    // адрес следующего чтения (RAM_DL — прямо с регистра)
    reg         f_run;                  // строка читается
    reg         f_fly, f_fep;           // чтение прошлого такта: слово сейчас в dl_q; его эпоха
    reg  [10:0] f_fpc;                  // его адрес
    reg         ep;                     // эпоха: переход сбрасывает очередь, слово в пути — мимо
    reg  [31:0] fq_w0, fq_w1;
    reg  [10:0] fq_pc0, fq_pc1;
    reg  [1:0]  fq_n;                   // слов в очереди
    assign dl_addr = fpc;

    // --- ступень E ---------------------------------------------------------------------------
    // В D фигура только запоминается (сырые поля); границы строк, порядок вершин RECTS и
    // задание считаются в E — так короче путь от RAM_DL и таблицы ручек.
    reg         e_v;
    reg  [2:0]  e_kind;
    reg  [31:0] e_w;                    // STATE: слово листа
    reg  [19:0] e_vx, e_vy, e_px, e_py; // вершина и предыдущая (RECTS)
    reg  [21:0] e_hs;
    reg  [23:0] e_hl;
    reg  [3:0]  e_hlh, e_hzh;
    reg  [20:0] e_hz;
    reg  [6:0]  e_vc;
    reg  [12:0] e_ps;
    reg  [11:0] e_lw;

    function automatic logic [20:0] s21(input logic [19:0] v);
        s21 = {v[19], v};
    endfunction
    wire [10:0] eh_w11 = {e_hzh[3:2], e_hz[17:9]};
    wire [10:0] eh_h11 = {e_hzh[1:0], e_hz[8:0]};
    wire [11:0] e_bw = eh_w11 == 11'd0 ? 12'd2048 : {1'b0, eh_w11};
    wire [11:0] e_bh = eh_h11 == 11'd0 ? 12'd2048 : {1'b0, eh_h11};
    wire        e_sx = $signed(e_px) <= $signed(e_vx);
    wire        e_sy = $signed(e_py) <= $signed(e_vy);
    wire [19:0] e_x0 = e_sx ? e_px : e_vx, e_x1 = e_sx ? e_vx : e_px;
    wire [19:0] e_y0 = e_sy ? e_py : e_vy, e_y1 = e_sy ? e_vy : e_py;
    reg  [126:0] e_data;
    reg         e_cull;                 // BITMAPS ничего не меняет (пустая раскладка) — строк нет
    reg         e_test;                 // 1 — фигура: строки и x проверяются
    reg  [20:0] e_lo, e_hi;             // строки фигуры: e_lo ≤ y·16 < e_hi (1/16, со знаком)
    reg  [20:0] e_xl, e_xh;             // края по x с запасом (1/16, со знаком)
    always_comb begin
        e_test = 1'b1;
        e_lo   = '0;
        e_hi   = '0;
        e_xl   = '0;
        e_xh   = '0;
        case (e_kind)
            K_BMP: begin
                e_lo   = s21(e_vy);
                e_hi   = e_cull ? s21(e_vy) : s21(e_vy) + {5'd0, e_bh, 4'd0};
                e_xl   = s21(e_vx);
                e_xh   = s21(e_vx) + {5'd0, e_bw, 4'd0};
                e_data = {e_vc, e_bh, e_bw, e_hz[18], e_hz[19], e_hz[20], e_hlh[1:0], e_hl[8:0],
                          e_hlh[3:2], e_hl[18:9], e_hl[23:19], e_hs, e_vy, e_vx, K_BMP};
            end
            K_POINT: begin
                e_lo   = s21(e_vy) - {8'd0, e_ps} - 21'd15;
                e_hi   = s21(e_vy) + {8'd0, e_ps} + 21'd16;
                e_xl   = s21(e_vx) - {8'd0, e_ps} - 21'd17;
                e_xh   = s21(e_vx) + {8'd0, e_ps} + 21'd16;
                e_data = {71'd0, e_ps, e_vy, e_vx, K_POINT};
            end
            K_RECT: begin
                e_lo   = s21(e_y0) - {9'd0, e_lw} - 21'd7;
                e_hi   = s21(e_y1) + {9'd0, e_lw} + 21'd8;
                e_xl   = s21(e_x0) - {9'd0, e_lw} - 21'd17;
                e_xh   = s21(e_x1) + {9'd0, e_lw} + 21'd16;
                e_data = {32'd0, e_lw, e_y1, e_x1, e_y0, e_x0, K_RECT};
            end
            K_EOL: begin
                e_test = 1'b0;
                e_data = {124'd0, K_EOL};
            end
            default: begin
                e_test = 1'b0;
                e_data = {92'd0, e_w, K_STATE};
            end
        endcase
    end
    wire        e_clr = e_kind == K_STATE && e_w[31:24] == 8'h26;

    // --- ступень E1b: строки плитки и проверка по x -----------------------------------------------
    reg         w_v;
    reg  [2:0]  w_kind;
    reg  [126:0] w_data;
    reg         w_test, w_clr;
    reg  [20:0] w_lo, w_hi, w_xl, w_xh;
    // Строки плитки [w_rlo, w_rhi): у фигуры — те, где lo ≤ y·16 < hi, и только если она
    // задевает плитку по x (первая точка ≤ ⌊xl/16⌋, после последней ≥ ⌊xh/16⌋ + 1 — шире
    // пролёта исполнителя); CLEAR — все строки плитки; прочее — один раз (строка 0)
    wire signed [20:0] ty16 = $signed({7'd0, ty0, 4'd0});
    wire signed [20:0] w_l  = $signed(w_lo) - ty16 + 21'sd15;
    wire signed [20:0] w_h  = $signed(w_hi) - ty16 + 21'sd15;
    wire signed [16:0] w_rl = 17'(w_l >>> 4), w_rh = 17'(w_h >>> 4);
    wire signed [16:0] w_fx = 17'($signed(w_xl) >>> 4), w_lx = 17'($signed(w_xh) >>> 4) + 17'sd1;
    wire        w_inx = w_lx > $signed({7'd0, tx0}) && w_fx < $signed({7'd0, tx0}) + 17'(TW);
    reg  [4:0]  w_rlo, w_rhi;
    always_comb begin
        if (!w_test) begin
            w_rlo = 5'd0;
            w_rhi = w_clr ? t_rows : 5'd1;
        end else begin
            w_rlo = w_rl < 0 ? 5'd0 : (w_rl > 17'(TH) ? 5'(TH) : w_rl[4:0]);
            w_rhi = !w_inx || w_rh < 0 ? 5'd0 : (w_rh > $signed({12'd0, t_rows}) ? t_rows : w_rh[4:0]);
        end
    end
    wire        w_multi = w_kind == K_BMP || w_clr;

    // --- ступень E2: запись в очередь заданий — по строке плитки за такт --------------------------
    reg         x_v;
    reg  [2:0]  x_kind;
    reg  [126:0] x_data;
    reg  [4:0]  x_r, x_rhi;             // строка, которая пишется, и конец
    reg         x_multi;                // BITMAPS, CLEAR: все строки — одним заданием
    reg         x_none;                 // строк нет: фигура мимо плитки
    reg         x_last;                 // x_r — последняя строка (или задание — одно на все)
    // свобода ступеней — из регистров (и места в очереди): путь до выборки D короткий
    wire x_go    = x_v && !x_none && !it_full;
    wire x_end   = x_v && (x_none || (x_go && x_last));   // E2 свободна в этом такте
    wire x_free  = !x_v || x_end;
    assign it_v    = x_go && !abort;
    assign it_data = {x_multi ? x_rhi : x_r + 5'd1, x_r[3:0], x_data};
    wire w_free  = !w_v || x_free;
    wire e1_free = !e_v || w_free;
    wire d_v     = fq_n != 2'd0;
    wire d_go    = d_v && e1_free && !eol_wait;

    // --- разбор слова в D ----------------------------------------------------------------------
    wire [31:0] w   = fq_w0;
    wire [10:0] pc  = fq_pc0;
    wire [7:0]  op  = w[31:24];
    wire        is_v2f  = w[31:30] == 2'b01;
    wire        is_v2ii = w[31:30] == 2'b10;
    wire        is_vtx  = is_v2f || is_v2ii;

    wire [2:0]  frac  = vfmt > 3'd4 ? 3'd4 : vfmt;
    wire [19:0] v2f_x = {{5{w[29]}}, w[29:15]} << (3'd4 - frac);
    wire [19:0] v2f_y = {{5{w[14]}}, w[14:0]} << (3'd4 - frac);
    wire [19:0] raw_x = is_v2f ? v2f_x : {7'd0, w[29:21], 4'd0};
    wire [19:0] raw_y = is_v2f ? v2f_y : {7'd0, w[20:12], 4'd0};
    wire [19:0] vx = raw_x + {{3{tr_x[16]}}, tr_x};
    wire [19:0] vy = raw_y + {{3{tr_y[16]}}, tr_y};
    wire [4:0]  vh = is_v2ii ? w[11:7] : handle;
    wire [6:0]  vc = is_v2ii ? w[6:0] : vcell;

    wire [21:0] hs  = h_src[vh];
    wire [23:0] hl  = h_lay[vh];
    wire [3:0]  hlh = h_layh[vh];
    wire [20:0] hz  = h_siz[vh];
    wire [3:0]  hzh = h_sizh[vh];
    // BITMAPS ничего не меняет: раскладка пустая (ширина в текселях — stride·8/бит на тексель —
    // или высота 0: у модели и исполнителя такие тексели — прозрачный чёрный при любом повторе),
    // множитель приёмника ONE или ONE_MINUS_SRC_ALPHA, трафарет не пишется
    wire [11:0] hl_stride = {hlh[3:2], hl[18:9]};
    wire [10:0] hl_h      = {hlh[1:0], hl[8:0]};
    wire [4:0]  hl_fmt    = hl[23:19];
    wire        hl_empty  = hl_stride == 12'd0 || hl_h == 11'd0 ||
                            ((hl_fmt == 5'd0 || hl_fmt == 5'd6 || hl_fmt == 5'd7) && hl_stride == 12'd1);
    wire        bmp_noop  = hl_empty && (bdst == 3'd1 || bdst == 3'd4) &&
                            (swm == 8'd0 || (sfail == 3'd1 && spass == 3'd1));


    // конец строки этим словом (само слово не выполняется)
    wire end_now = d_fend || steps == 15'h4000 ||
                   (!is_vtx && (op == 8'h00 ||
                                (op == 8'h1D && cs_cnt == 3'd4) ||
                                (op == 8'h24 && cs_cnt == 3'd0)));
    // следующий адрес листа
    reg  [10:0] next_pc;
    always_comb begin
        next_pc = pc + 11'd1;
        if (!is_vtx) case (op)
            8'h1D, 8'h1E: next_pc = w[10:0];                            // CALL, JUMP
            8'h24:        next_pc = cs_mem[cs_cnt[1:0] - 2'd1];         // RETURN
            default: ;
        endcase
    end
    wire seq_wrap = pc == 11'h7FF && !(!is_vtx && (op == 8'h1D || op == 8'h1E || op == 8'h24));

    // запись таблицы — один порт на поле (очистка или команда листа), отдельным процессом:
    // так синтез кладёт таблицу в распределённую память, а не в триггеры
    wire        h_cmd  = d_go && !end_now && !is_vtx;
    wire [4:0]  h_wa   = h_clr[5] ? h_clr[4:0] : handle;
    wire        h_we_s = h_clr[5] || (h_cmd && op == 8'h01);
    wire        h_we_l = h_clr[5] || (h_cmd && op == 8'h07);
    wire        h_we_lh = h_clr[5] || (h_cmd && op == 8'h28);
    wire        h_we_z = h_clr[5] || (h_cmd && op == 8'h08);
    wire        h_we_zh = h_clr[5] || (h_cmd && op == 8'h29);
    // Ручки 16…31 после сброса — шрифты ПЗУ FT812, как у чипа: BITMAP_SOURCE — адрес глифа 0,
    // LAYOUT — L1 / L4, шаг строки, высота; SIZE — NEAREST, BORDER, ширина × высота. Числа — из
    // таблицы шрифтов ПЗУ (ROM_FONTROOT = 0x201EE0, по 148 байтов на шрифт; снято с bt8xxemu,
    // fpga/vdac2/scripts — образ шрифтов в Tang заливает ESP). Ручки 0…15 — нули.
    function automatic logic [66:0] rom_font(input logic [4:0] hnd);   // {source, layout, size}
        logic [4:0] fmt; logic [9:0] st; logic [8:0] fw, fh; logic [21:0] src;
        case (hnd)
            5'd16: begin fmt = 5'd1; st = 10'd1;  fw = 9'd8;  fh = 9'd8;  src = 22'h2FF7FC; end
            5'd17: begin fmt = 5'd1; st = 10'd1;  fw = 9'd8;  fh = 9'd8;  src = 22'h2FFBFC; end
            5'd18: begin fmt = 5'd1; st = 10'd1;  fw = 9'd8;  fh = 9'd16; src = 22'h2FE7FC; end
            5'd19: begin fmt = 5'd1; st = 10'd1;  fw = 9'd8;  fh = 9'd16; src = 22'h2FEFFC; end
            5'd20: begin fmt = 5'd1; st = 10'd2;  fw = 9'd10; fh = 9'd13; src = 22'h2FDAFC; end
            5'd21: begin fmt = 5'd1; st = 10'd2;  fw = 9'd13; fh = 9'd17; src = 22'h2FCD3C; end
            5'd22: begin fmt = 5'd1; st = 10'd2;  fw = 9'd14; fh = 9'd20; src = 22'h2FBD7C; end
            5'd23: begin fmt = 5'd1; st = 10'd3;  fw = 9'd17; fh = 9'd22; src = 22'h2FA17C; end
            5'd24: begin fmt = 5'd1; st = 10'd3;  fw = 9'd24; fh = 9'd29; src = 22'h2F7E3C; end
            5'd25: begin fmt = 5'd1; st = 10'd4;  fw = 9'd30; fh = 9'd38; src = 22'h2F3D1C; end
            5'd26: begin fmt = 5'd2; st = 10'd7;  fw = 9'd14; fh = 9'd16; src = 22'h2F181C; end
            5'd27: begin fmt = 5'd2; st = 10'd8;  fw = 9'd16; fh = 9'd20; src = 22'h2ED61C; end
            5'd28: begin fmt = 5'd2; st = 10'd9;  fw = 9'd18; fh = 9'd25; src = 22'h2E799C; end
            5'd29: begin fmt = 5'd2; st = 10'd11; fw = 9'd22; fh = 9'd28; src = 22'h2DFBBC; end
            5'd30: begin fmt = 5'd2; st = 10'd14; fw = 9'd28; fh = 9'd36; src = 22'h2D263C; end
            5'd31: begin fmt = 5'd2; st = 10'd18; fw = 9'd36; fh = 9'd49; src = 22'h2BAC3C; end
            default: begin fmt = '0; st = '0; fw = '0; fh = '0; src = '0; end
        endcase
        rom_font = {src, fmt, st, fh, 3'b000, fw, fh};
    endfunction
    wire [66:0] h_rom = rom_font(h_clr[4:0]);
    always_ff @(posedge clk) begin
        if (h_we_s)  h_src[h_wa]  <= h_clr[5] ? h_rom[66:45] : w[21:0];
        if (h_we_l)  h_lay[h_wa]  <= h_clr[5] ? h_rom[44:21] : w[23:0];
        if (h_we_lh) h_layh[h_wa] <= h_clr[5] ? 4'd0 : w[3:0];
        if (h_we_z)  h_siz[h_wa]  <= h_clr[5] ? h_rom[20:0] : w[20:0];
        if (h_we_zh) h_sizh[h_wa] <= h_clr[5] ? 4'd0 : w[3:0];
    end

    // переход: CALL, JUMP, RETURN (исполненные) — выборка с нового адреса
    wire d_jump = !is_vtx && (op == 8'h1D || op == 8'h1E || op == 8'h24);
    wire redirect = d_go && (end_now || d_jump);
    // чтение в этом такте, если в очереди останется место и для него
    wire f_issue = f_run && !redirect &&
                   ({1'b0, fq_n} + {2'b0, f_fly} - {2'b0, d_go}) <= 3'd1;
    wire f_push  = f_fly && f_fep == ep && !redirect;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            {vfmt, handle, vcell, tr_x, tr_y, psize, lwidth, bdst, sfail, spass, swm} <= CTX_DEF;
            prim <= '0; odd <= 1'b0; px <= '0; py <= '0;
            sv_sp <= '0; sv_cnt <= '0; cs_cnt <= '0;
            h_clr <= 6'h20;
            ty0 <= '0; tx0 <= '0; n_lines <= '0; n_h <= '0; steps <= '0; d_fend <= 1'b0; eol_wait <= 1'b0;
            fpc <= '0; f_run <= 1'b0; f_fly <= 1'b0; f_fep <= 1'b0; f_fpc <= '0; ep <= 1'b0;
            fq_w0 <= '0; fq_w1 <= '0; fq_pc0 <= '0; fq_pc1 <= '0; fq_n <= '0;
            x_v <= 1'b0; x_kind <= '0; x_data <= '0; x_r <= '0; x_rhi <= '0; x_multi <= 1'b0;
            x_none <= 1'b0; x_last <= 1'b0;
            w_v <= 1'b0; w_kind <= '0; w_data <= '0; w_test <= 1'b0; w_clr <= 1'b0;
            w_lo <= '0; w_hi <= '0; w_xl <= '0; w_xh <= '0;
            e_v <= 1'b0; e_kind <= '0; e_cull <= 1'b0; e_w <= '0; e_vx <= '0; e_vy <= '0; e_px <= '0; e_py <= '0;
            e_hs <= '0; e_hl <= '0; e_hlh <= '0; e_hz <= '0; e_hzh <= '0; e_vc <= '0;
            e_ps <= '0; e_lw <= '0;
            busy <= 1'b0; unsup <= '0;
        end else begin

            // очистка таблицы ручек после сброса (запись — h_we_*)
            if (h_clr[5])
                h_clr <= h_clr[4:0] == 5'd31 ? 6'd0 : h_clr + 6'd1;

            // выборка: слово прошлого чтения — в очередь, D забирает голову
            f_fly <= f_issue;
            f_fpc <= fpc;
            f_fep <= ep;
            if (f_issue) fpc <= fpc + 11'd1;
            if (redirect) begin
                fq_n <= 2'd0;
            end else case ({f_push, d_go})
                2'b10: begin
                    if (fq_n == 2'd0) begin fq_w0 <= dl_q; fq_pc0 <= f_fpc; end
                    else begin fq_w1 <= dl_q; fq_pc1 <= f_fpc; end
                    fq_n <= fq_n + 2'd1;
                end
                2'b01: begin
                    fq_w0 <= fq_w1; fq_pc0 <= fq_pc1;
                    fq_n  <= fq_n - 2'd1;
                end
                2'b11: begin
                    if (fq_n == 2'd1) begin fq_w0 <= dl_q; fq_pc0 <= f_fpc; end
                    else begin fq_w0 <= fq_w1; fq_pc0 <= fq_pc1; fq_w1 <= dl_q; fq_pc1 <= f_fpc; end
                end
                default: ;
            endcase

            // E2 → очередь заданий (it_v, it_data — из E2 в этот такт): RECTS, POINTS — строка за
            // строкой
            if (x_go) begin
                x_r    <= x_r + 5'd1;
                x_last <= x_multi || x_r + 5'd2 >= x_rhi;
            end
            // E1b → E2
            if (w_v && x_free) begin
                w_v     <= 1'b0;
                x_v     <= 1'b1;
                x_kind  <= w_kind;
                x_data  <= w_data;
                x_r     <= w_rlo;
                x_rhi   <= w_rhi;
                x_multi <= w_multi;
                x_none  <= w_rlo >= w_rhi;
                x_last  <= w_multi || w_rlo + 5'd1 >= w_rhi;
            end else if (x_end) begin
                x_v <= 1'b0;
            end
            // E1 → E1b
            if (e_v && w_free) begin
                e_v    <= 1'b0;
                w_v    <= 1'b1;
                w_kind <= e_kind;
                w_data <= e_data;
                w_test <= e_test;
                w_clr  <= e_clr;
                w_lo   <= e_lo;
                w_hi   <= e_hi;
                w_xl   <= e_xl;
                w_xh   <= e_xh;
            end

            // конец плитки ушёл: следующая плитка — правее, у правого края — в начале следующей
            // полосы строк
            if (x_go && x_kind == K_EOL) begin
                eol_wait <= 1'b0;
                if (!t_last_x) begin
                    tx0 <= tx0 + 10'(TW);
                    tile_start();
                end else if (!t_last_y) begin
                    tx0 <= '0;
                    ty0 <= ty0 + 10'(TH);
                    tile_start();
                end else begin
                    busy <= 1'b0;                         // кадр пройден
                end
            end

            if (abort) begin
                busy <= 1'b0; f_run <= 1'b0; fq_n <= 2'd0; ep <= !ep; d_fend <= 1'b0;
                e_v <= 1'b0; w_v <= 1'b0; x_v <= 1'b0; eol_wait <= 1'b0;
            end else if (start && !busy) begin
                busy    <= 1'b1;
                ty0     <= '0;
                tx0     <= '0;
                n_lines <= lines;
                n_h     <= hsize;
                tile_start();
            end else if (d_go) begin
                steps  <= steps + 15'd1;
                if (d_jump && !end_now) begin
                    fpc <= next_pc;                   // переход: выборка заново
                    ep  <= !ep;
                end
                e_kind <= K_STATE;
                e_w    <= w;
                if (end_now) begin
                    if (!d_fend && !is_vtx && op == 8'h1D) unsup[2] <= 1'b1;
                    if (steps == 15'h4000) unsup[3] <= 1'b1;
                    e_v      <= 1'b1;
                    e_kind   <= K_EOL;
                    eol_wait <= 1'b1;
                    f_run    <= 1'b0;                 // плитка кончилась: выборка стоит
                    ep       <= !ep;
                    d_fend   <= 1'b0;
                end else begin
                    if (seq_wrap)
                        d_fend <= 1'b1;               // слово 2047 последнее
                    if (is_vtx) begin
                        px <= vx;
                        py <= vy;
                        case (prim)
                            4'd1: begin                               // BITMAPS
                                e_v    <= 1'b1;
                                e_kind <= K_BMP;
                                e_cull <= bmp_noop;
                                e_vx   <= vx;
                                e_vy   <= vy;
                                e_hs   <= hs;
                                e_hl   <= hl;
                                e_hlh  <= hlh;
                                e_hz   <= hz;
                                e_hzh  <= hzh;
                                e_vc   <= vc;
                            end
                            4'd2: begin                               // POINTS
                                e_v    <= 1'b1;
                                e_kind <= K_POINT;
                                e_vx   <= vx;
                                e_vy   <= vy;
                                e_ps   <= psize;
                            end
                            4'd9: begin                               // RECTS
                                odd <= !odd;
                                if (odd) begin
                                    e_v    <= 1'b1;
                                    e_kind <= K_RECT;
                                    e_px   <= px;
                                    e_py   <= py;
                                    e_vx   <= vx;
                                    e_vy   <= vy;
                                    e_lw   <= lwidth;
                                end
                            end
                            4'd3, 4'd4: begin                         // LINES, LINE_STRIP
                                // пара: LINES — вершины по две, LINE_STRIP — с предыдущей;
                                // отрезок вдоль оси ненулевой длины у FT812 — ровно RECTS с теми
                                // же вершинами и LINE_WIDTH (замер bt8xxemu); наклонный и
                                // нулевой — не рисуются (флаг)
                                odd <= prim == 4'd4 ? 1'b1 : !odd;
                                if (odd) begin
                                    if ((px == vx) != (py == vy)) begin
                                        e_v    <= 1'b1;
                                        e_kind <= K_RECT;
                                        e_px   <= px;
                                        e_py   <= py;
                                        e_vx   <= vx;
                                        e_vy   <= vy;
                                        e_lw   <= lwidth;
                                    end else unsup[0] <= 1'b1;
                                end
                            end
                            4'd5, 4'd6, 4'd7, 4'd8: unsup[0] <= 1'b1;
                            default: ;
                        endcase
                    end else begin
                        case (op)
                            8'h05: handle <= w[4:0];
                            8'h0B: bdst <= w[2:0];
                            8'h0C: {sfail, spass} <= w[5:0];
                            8'h13: swm <= w[7:0];
                            8'h06: vcell <= w[6:0];
                            8'h0D: psize <= w[12:0];
                            8'h0E: lwidth <= w[11:0];
                            8'h1D: begin cs_mem[cs_cnt[1:0]] <= pc + 11'd1; cs_cnt <= cs_cnt + 3'd1; end
                            8'h1F: begin prim <= w[3:0]; odd <= 1'b0; end
                            8'h21: begin prim <= 4'd0; odd <= 1'b0; end
                            8'h22: begin
                                sv_mem[sv_sp] <= ctx_now;
                                sv_sp  <= sv_sp + 2'd1;
                                sv_cnt <= sv_cnt == 3'd4 ? 3'd4 : sv_cnt + 3'd1;
                            end
                            8'h23: begin
                                if (sv_cnt != 3'd0) begin
                                    {vfmt, handle, vcell, tr_x, tr_y, psize, lwidth, bdst, sfail, spass, swm} <= sv_mem[sv_sp - 2'd1];
                                    sv_sp  <= sv_sp - 2'd1;
                                    sv_cnt <= sv_cnt - 3'd1;
                                end else begin
                                    {vfmt, handle, vcell, tr_x, tr_y, psize, lwidth, bdst, sfail, spass, swm} <= CTX_DEF;
                                end
                            end
                            8'h24: cs_cnt <= cs_cnt - 3'd1;
                            8'h25: unsup[1] <= 1'b1;
                            8'h27: vfmt <= w[2:0];
                            8'h2B: tr_x <= w[16:0];
                            8'h2C: tr_y <= w[16:0];
                            default: ;
                        endcase
                        // исполнителю: цвет, смешение, трафарет, ножницы, преобразование, CLEAR,
                        // SAVE/RESTORE (у исполнителя своя часть контекста)
                        case (op)
                            8'h02, 8'h04, 8'h09, 8'h0A, 8'h0B, 8'h0C, 8'h0F, 8'h10, 8'h11, 8'h13,
                            8'h15, 8'h16, 8'h17, 8'h18, 8'h19, 8'h1A, 8'h1B, 8'h1C, 8'h20,
                            8'h22, 8'h23, 8'h26, 8'h2A: e_v <= 1'b1;
                            default: ;
                        endcase
                    end
                end
            end
        end
    end

    // новая плитка: контекст по умолчанию, лист — с начала
    task automatic tile_start();
        {vfmt, handle, vcell, tr_x, tr_y, psize, lwidth, bdst, sfail, spass, swm} <= CTX_DEF;
        prim   <= '0;
        odd    <= 1'b0;
        sv_sp  <= '0;
        sv_cnt <= '0;
        cs_cnt <= '0;
        steps  <= '0;
        fpc    <= '0;
        f_run  <= 1'b1;
        ep     <= !ep;
        fq_n   <= 2'd0;
        d_fend <= 1'b0;
    endtask
endmodule
