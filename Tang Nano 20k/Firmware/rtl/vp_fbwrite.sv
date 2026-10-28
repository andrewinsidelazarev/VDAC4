// SPDX-License-Identifier: Apache-2.0
//
// Ядро видео (MJPEG): запись кадров декодера JPEG (third_party/core_jpeg, jpeg_core) в кадры FT
// кадрового буфера (fb_core, режим FT) — на месте растеризатора FT812 (ft_render): тот же разъём
// SDRAM (клиент sdram_burst, только запись) и то же объявление готового кадра. Вывод (fb_scanout)
// растягивает кадр, как кадр FT812: 4:3 или 16:9 и размер 100/95/90 % — настройки слота 0.
//
// ХОЛСТ. Развёртка знает три размера кадра FT: 512×384 (FT_W512), 640×480 и 1024×768. Картинка
// ролика кладётся на холст наименьшего из них, в который входит, по центру: по x — с шагом 16
// точек (пакеты SDRAM остаются выровнены), по y — точно. Картинка больше 1024×768 (с 1.38) —
// вдвое меньше: берутся точки с чётными x и y (HD 1280×720 → 640×360, 1920×1080 → 960×540);
// прежде такой ролик FTVIEW4 отдавал пути FT812 — экран 4:3, кадр обрезан, декодирует BL616
// медленно (пользователь, 27.10.2026: «широкоформатные видео обрезаются в 4/3»). Поля
// холста чёрные: когда размер картинки в этом буфере другой, чем у прошлого кадра в нём (или
// буфер ещё не писался), холст сначала стирается целиком пакетами нулей (512×384 — ~2,5 мс,
// один раз на буфер в начале ролика), а точки правее ширины картинки (дополнение MCU) пишутся
// чёрными. Объявляется размер холста. Ролик 512×384 занимает холст целиком.
// С 1.36 у широкой картинки (ширина ≥ 14/9 высоты) — холсты 16:9 той же ширины: 512×288,
// 640×360, 1024×576 (наименьший, куда входит; выше 576 строк — снова 4:3). Развёртка выводит их
// окном 16:9 на всю ширину (пользователь, 27.10.2026: ролик 16:9 на холсте 4:3 шёл «не на весь
// экран» — с полями со всех сторон).
//
// Декодер выдаёт точки по одной (x, y, R, G, B) блоками 8×8 в порядке MCU: 4:2:0 — MCU 16×16
// (Y0 Y1 Y2 Y3), 4:2:2 — 16×8, 4:4:4 и серый — 8×8. Здесь они собираются в плитки 16 точек ×
// TH строк (TH = 16 у 4:2:0, иначе 8; плитку 16×8 у MCU 8×8 дают два MCU подряд; у
// уменьшенной картинки TH вдвое меньше — 8 или 4, плитку дают 2 или 4 MCU подряд): строка
// плитки — 8 слов, ровно пакет SDRAM. Плиток две: пока одна пишется в SDRAM, вторая собирается.
// Плитка закрывается, когда в ней все точки, когда пришла точка другой плитки (правый край
// картинки, ширина не кратна 16) и в конце кадра. Строки ниже картинки и правее 1024 точек
// не пишутся.
//
// Кадры FT — три, как у ft_render и fb_core: 0 и 1 — с FB_BASE + b·FT_BUF_WORDS, 2 — строки
// 0…511 с T_LO, 512…767 с T_HI; строка — 512 слов, точка x — слово x/2, младшая половина —
// чётная точка (fb_line_ram). Кадр пишется в буфер, который не выводится (fb_front) и не
// объявлен (pub_buf). Записанный кадр ждёт разрешения pub_go (темп кадров — снаружи) и
// объявляется: pub_v = 1 навсегда, pub_buf — его буфер; fb_core покажет его с начала
// следующего выходного кадра.
//
// Пары точек: декодер выдаёт строку блока слева направо, поэтому чётная точка запоминается, а
// с нечётной уходит слово {нечётная, чётная}. RGB565 — отбрасыванием младших разрядов.
//
// Отмена кадра (abort: битый кадр — сторож vp_core, сброс потока): точки больше не берутся,
// плитки отбрасываются, кадр не объявляется; начатый пакет SDRAM дописывается до конца (иначе
// нарушился бы протокол контроллера) — потом запись свободна (busy = 0).
//
`default_nettype none   // неявный провод — ошибка (1.38: d_accept без объявления выбросил декодер)
module vp_fbwrite #(
    parameter logic [20:0] FB_BASE      = 21'h128000,
    parameter logic [20:0] FT_BUF_WORDS = 21'h060000,   // кадр: 768 строк × 512 слов
    parameter logic [20:0] T_LO         = 21'h080000,   // кадр 2: строки 0…511
    parameter logic [20:0] T_HI         = 21'h0C9000    //         строки 512…767
) (
    input  wire         clk,
    input  wire         rst_n,

    // кадр: начало (до первой точки; выбирается буфер) и конец (декодер выдал всё)
    input  wire         frame_start,
    input  wire         frame_end,
    input  wire         abort,           // импульс: отменить кадр (см. выше)
    input  wire [1:0]   mode,            // jpeg_core outport_mode_o: 2 — 4:2:0 (MCU 16 строк);
                                         // mode, img_w, img_h — к первой точке кадра (SOF)
    input  wire [15:0]  img_w,
    input  wire [15:0]  img_h,

    // точки декодера
    input  wire         px_valid,
    output wire         px_ready,
    input  wire [15:0]  px_x,
    input  wire [15:0]  px_y,
    input  wire [7:0]   px_r,
    input  wire [7:0]   px_g,
    input  wire [7:0]   px_b,

    // SDRAM: клиент sdram_burst (только запись, пакет 8 слов; метка — снаружи)
    output reg          s_req,
    output wire         s_we,
    output reg  [20:0]  s_addr,
    output wire [2:0]   s_len,
    input  wire         s_ack,
    input  wire         s_wr_take,
    output wire [31:0]  s_wdata,

    // объявление кадра (fb_core): размер — холста
    input  wire         fb_showing,
    input  wire [1:0]   fb_front,
    output reg          pub_v,
    output reg  [1:0]   pub_buf,
    output reg  [9:0]   pub_words,
    output reg  [10:0]  pub_w,
    output reg  [9:0]   pub_h,

    // темп: кадр записан и ждёт (frame_ready); объявить (pub_go)
    output wire         frame_ready,
    input  wire         pub_go,
    output wire         busy             // кадр в работе: от frame_start до объявления
);
    assign s_we  = 1'b1;
    assign s_len = 3'd0;

    // --- кадр ---------------------------------------------------------------------------------
    localparam logic [2:0] F_IDLE = 3'd0, F_RUN = 3'd1, F_END = 3'd2, F_DONE = 3'd3, F_CLEAR = 3'd4;
    reg [2:0]  fst;
    reg [1:0]  wbuf;                     // буфер записываемого кадра
    reg [1:0]  th;                       // строк в плитке: 2 — 16 (4:2:0), 1 — 8, 0 — 4 (уменьшение)
    reg        t16;                      // ряд плиток — 16 строк источника (4:2:0), иначе 8
    reg        half;                     // картинка больше 1024×768: каждая вторая точка и строка
    reg [15:0] fw, fh;                   // картинка на холсте (не больше 1024×768)
    reg [10:0] cw;                       // холст: 512, 640 или 1024 точки
    reg [9:0]  ch;                       //        384, 480 или 768 строк
    reg [5:0]  ox16;                     // картинка на холсте: x / 16
    reg [9:0]  oy;                       //                     y
    reg        geo_ok;                   // размер кадра принят, холст готов — точки берутся
    reg        got_px;                   // в кадре уже была точка: конец кадра — только после неё
    reg        aborting;                 // отмена: ждём конца начатого пакета
    reg [20:0] bgeo [0:2];               // буфер: {ширина, высота} картинки, под которую стёрт
                                         // холст; 0 — не стирался
    reg [9:0]  cl_line;                  // стирание: строка холста
    reg [5:0]  cl_col;                   //           пакет в строке

    // буфер вне выводимого и объявленного
    function automatic [1:0] pick(input logic sh, input logic [1:0] fr, input logic pv, input logic [1:0] pb);
        logic [2:0] used;
        used = 3'b000;
        if (sh) used[fr] = 1'b1;
        if (pv) used[pb] = 1'b1;
        pick = !used[0] ? 2'd0 : !used[1] ? 2'd1 : 2'd2;
    endfunction

    // начало строки line кадра b (адрес слова)
    function automatic [20:0] line_base(input logic [1:0] b, input logic [9:0] line);
        case (b)
            2'd0:    line_base = FB_BASE + {2'b00, line, 9'd0};
            2'd1:    line_base = FB_BASE + FT_BUF_WORDS + {2'b00, line, 9'd0};
            default: line_base = line[9] ? T_HI + {3'b000, line[8:0], 9'd0} : T_LO + {3'b000, line[8:0], 9'd0};
        endcase
    endfunction

    // размер картинки (SOF) → холст и место на нём; больше 1024×768 — вдвое меньше (с округлением
    // вверх: у нечётного размера последняя точка — чётная), больше 2048×1536 — ещё и обрезается.
    // Регистрами в четыре шага: SOF разобран за сотни тактов до первой точки кадра (заголовок,
    // таблицы, IDCT блока), а путь «размер → уменьшение → холст → место» в такт не укладывался
    // (1.38, clk_mem 57 МГц)
    reg        g1_half, g2_half, g3_half, g_half;
    reg [15:0] g1_w, g1_h;                       // после уменьшения
    reg [15:0] g2_w, g2_h, g3_w, g3_h, g_w, g_h; // и обрезки до 1024×768
    reg        g3_wd, g3_512, g3_640, w3_512, w3_640;
    reg [10:0] g_cw, g_dx;
    reg [9:0]  g_ch, g_dy;
    wire [19:0] g2_w9  = {1'b0, g2_w, 3'b000} + {4'd0, g2_w};
    wire [19:0] g2_h14 = {g2_h, 4'b0000} - {3'd0, g2_h, 1'b0};
    wire [10:0] g3_cw = g3_wd ? (w3_512 ? 11'd512 : w3_640 ? 11'd640 : 11'd1024)
                              : (g3_512 ? 11'd512 : g3_640 ? 11'd640 : 11'd1024);
    wire [9:0]  g3_ch = g3_wd ? (w3_512 ? 10'd288 : w3_640 ? 10'd360 : 10'd576)
                              : (g3_512 ? 10'd384 : g3_640 ? 10'd480 : 10'd768);
    always_ff @(posedge clk) begin
        g1_half <= img_w > 16'd1024 || img_h > 16'd768;
        g1_w    <= (img_w > 16'd1024 || img_h > 16'd768) ? {1'b0, img_w[15:1]} + {15'd0, img_w[0]} : img_w;
        g1_h    <= (img_w > 16'd1024 || img_h > 16'd768) ? {1'b0, img_h[15:1]} + {15'd0, img_h[0]} : img_h;
        g2_half <= g1_half;
        g2_w    <= g1_w > 16'd1024 ? 16'd1024 : g1_w;
        g2_h    <= g1_h > 16'd768 ? 16'd768 : g1_h;
        // широкая: 9·ширина ≥ 14·высота; холст 16:9, если картинка в него входит по высоте
        g3_half <= g2_half; g3_w <= g2_w; g3_h <= g2_h;
        g3_wd   <= g2_w9 >= g2_h14 && g2_h <= 16'd576;
        g3_512  <= g2_w <= 16'd512 && g2_h <= 16'd384;
        g3_640  <= g2_w <= 16'd640 && g2_h <= 16'd480;
        w3_512  <= g2_w <= 16'd512 && g2_h <= 16'd288;
        w3_640  <= g2_w <= 16'd640 && g2_h <= 16'd360;
        g_half  <= g3_half; g_w <= g3_w; g_h <= g3_h;
        g_cw    <= g3_cw;
        g_ch    <= g3_ch;
        g_dx    <= g3_cw - g3_w[10:0];
        g_dy    <= g3_ch - g3_h[9:0];
    end
    wire [20:0] g_geo = {g_w[10:0], g_h[9:0]};

    // --- плитки -------------------------------------------------------------------------------
    localparam logic [1:0] T_FREE = 2'd0, T_FILL = 2'd1, T_FULL = 2'd2, T_FLUSH = 2'd3;
    reg [1:0]  tst [0:1];
    reg [5:0]  ttx [0:1];                // плитка: x/16 (до 64 — 1024 точки)
    reg [9:0]  tty [0:1];                // y / TH
    reg        fill_ts;                  // какая собирается
    reg [8:0]  fill_cnt;                 // точек в ней
    reg [15:0] pair_lo;                  // чётная точка пары
    reg        fl_ts;                    // какая пишется следующей (плитки закрываются по очереди)

    (* ram_style = "block" *) reg [31:0] tmem [0:255];   // {плитка, строка[3:0], слово[2:0]}

    // точка на картинке холста (у уменьшенной — x/2, y/2; нечётные x и y — мимо)
    wire [15:0] o_x   = half ? {1'b0, px_x[15:1]} : px_x;
    wire        drop  = half && (px_x[0] || px_y[0]);
    // точка правее картинки (дополнение MCU) — чёрная: она ложится на поле холста
    wire [15:0] px565 = o_x >= fw ? 16'h0000 : {px_r[7:3], px_g[7:2], px_b[7:3]};
    // плитка: столбец — 16 точек холста (у уменьшенной — 32 точки источника), ряд — ряд MCU
    // источника (16 строк у 4:2:0, иначе 8: у уменьшенной та же высота источника, строк на холсте
    // вдвое меньше). Этот путь — в готовность для декодера (px_ready): мультиплексоров в нём мало
    wire [5:0]  p_tx  = half ? px_x[10:5] : px_x[9:4];
    wire [9:0]  p_ty  = t16 ? px_y[13:4] : px_y[12:3];
    wire [3:0]  p_row = half ? (t16 ? {1'b0, px_y[3:1]} : {2'b00, px_y[2:1]})
                             : (t16 ? px_y[3:0] : {1'b0, px_y[2:0]});
    wire        cur_open = tst[fill_ts] == T_FILL;
    wire        other_tile = cur_open && (p_tx != ttx[fill_ts] || p_ty != tty[fill_ts]);
    wire        slot_ok = tst[fill_ts] == T_FREE || tst[fill_ts] == T_FILL;
    wire [8:0]  tile_px = th == 2'd2 ? 9'd256 : th == 2'd1 ? 9'd128 : 9'd64;

    // точка принимается в собираемую (или свободную) плитку той же плитки; точка другой
    // плитки сначала закрывает нынешнюю (такт без приёма); выброшенная уменьшением — сразу
    wire   run      = fst == F_RUN && geo_ok && !aborting;
    assign px_ready = run && (drop || (slot_ok && !other_tile));
    wire   take_px  = px_valid && px_ready && !drop;
    wire   close_now = run && px_valid && !drop && slot_ok && other_tile;
    wire   last_px  = take_px && fill_cnt == tile_px - 9'd1;

    // --- запись в SDRAM: плитки (строка — пакет 8 слов) и стирание холста ------------------------
    localparam logic [2:0] W_IDLE = 3'd0, W_REQ = 3'd1, W_DATA = 3'd2, W_CREQ = 3'd3, W_CDATA = 3'd4;
    reg [2:0]  wst;
    reg [3:0]  wrow;
    reg [2:0]  wcnt;
    reg [7:0]  rd_ptr;
    reg [31:0] rd_q;
    wire [7:0] rd_next = s_wr_take ? rd_ptr + 8'd1 : rd_ptr;
    // слово плитки правее картинки — нули: таких точек декодер может не выдать вовсе (MCU 8×8,
    // ширина кратна 8, но не 16), и в памяти плиток там старое
    wire       w_out   = {5'd0, ttx[fl_ts], wcnt, 2'b00} >= {fw[14:0], 1'b0};
    assign s_wdata = (wst == W_CDATA || w_out) ? 32'd0 : rd_q;

    wire [9:0] w_line = th == 2'd2 ? {tty[fl_ts][5:0], wrow} :
                        th == 2'd1 ? {tty[fl_ts][6:0], wrow[2:0]} : {tty[fl_ts][7:0], wrow[1:0]};
    wire [4:0] th_m1  = th == 2'd2 ? 5'd15 : th == 2'd1 ? 5'd7 : 5'd3;
    // строку пишем, если она в картинке и в пределах кадра FT
    wire       row_in = ({6'd0, w_line} < fh) && ({6'd0, ttx[fl_ts], 4'd0} < fw);
    wire [6:0] w_col  = {1'b0, ox16} + {1'b0, ttx[fl_ts]};      // пакет в строке холста

    always_ff @(posedge clk) begin
        if (take_px && o_x[0])
            tmem[{fill_ts, p_row, o_x[3:1]}] <= {px565, pair_lo};
        rd_q <= tmem[rd_next];
    end

    integer k;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            fst <= F_IDLE; wbuf <= 2'd0; th <= 2'd1; t16 <= 1'b0; half <= 1'b0; fw <= '0; fh <= '0; got_px <= 1'b0;
            cw <= 11'd512; ch <= 10'd384; ox16 <= '0; oy <= '0; geo_ok <= 1'b0;
            aborting <= 1'b0; cl_line <= '0; cl_col <= '0;
            for (k = 0; k < 3; k = k + 1) bgeo[k] <= '0;
            for (k = 0; k < 2; k = k + 1) begin tst[k] <= T_FREE; ttx[k] <= '0; tty[k] <= '0; end
            fill_ts <= 1'b0; fill_cnt <= '0; pair_lo <= '0; fl_ts <= 1'b0;
            wst <= W_IDLE; wrow <= '0; wcnt <= '0; rd_ptr <= '0; s_req <= 1'b0; s_addr <= '0;
            pub_v <= 1'b0; pub_buf <= 2'd0; pub_words <= '0; pub_w <= '0; pub_h <= '0;
        end else begin
            rd_ptr <= rd_next;

            // ---- кадр
            case (fst)
                F_IDLE: if (frame_start) begin
                    fst      <= F_RUN;
                    wbuf     <= pick(fb_showing, fb_front, pub_v, pub_buf);
                    fill_cnt <= '0;
                    got_px   <= 1'b0;
                    geo_ok   <= 1'b0;
                end
                F_RUN: if (!geo_ok) begin
                    // первая точка: SOF уже разобран — размер, холст; другой размер — стереть
                    if (px_valid && !aborting) begin
                        half <= g_half;
                        t16  <= mode == 2'd2;
                        th   <= mode == 2'd2 ? (g_half ? 2'd1 : 2'd2) : (g_half ? 2'd0 : 2'd1);
                        fw   <= g_w;
                        fh   <= g_h;
                        cw   <= g_cw;
                        ch   <= g_ch;
                        ox16 <= g_dx[10:5];
                        oy   <= {1'b0, g_dy[9:1]};
                        if (bgeo[wbuf] == g_geo) begin
                            geo_ok <= 1'b1;
                        end else begin
                            fst     <= F_CLEAR;
                            cl_line <= '0;
                            cl_col  <= '0;
                        end
                    end
                end else if (frame_end && got_px && !px_valid && !aborting) begin
                    fst <= F_END;
                    if (tst[fill_ts] == T_FILL) begin   // недособранная плитка — тоже в память
                        tst[fill_ts] <= T_FULL;
                        fill_ts      <= !fill_ts;
                        fill_cnt     <= '0;
                    end
                end
                F_END: if (tst[0] == T_FREE && tst[1] == T_FREE) fst <= F_DONE;
                F_DONE: if (pub_go && !aborting) begin
                    fst       <= F_IDLE;
                    pub_v     <= 1'b1;
                    pub_buf   <= wbuf;
                    pub_w     <= cw;
                    pub_h     <= ch;
                    pub_words <= cw[10:1];
                end
                default: ;                               // F_CLEAR — ведёт запись ниже
            endcase

            // ---- сборка плиток
            if (close_now) begin
                tst[fill_ts] <= T_FULL;
                fill_ts      <= !fill_ts;
                fill_cnt     <= '0;
            end else if (take_px) begin
                got_px <= 1'b1;
                if (!o_x[0]) pair_lo <= px565;
                if (tst[fill_ts] == T_FREE) begin
                    tst[fill_ts] <= T_FILL;
                    ttx[fill_ts] <= p_tx;
                    tty[fill_ts] <= p_ty;
                end
                if (last_px) begin
                    tst[fill_ts] <= T_FULL;
                    fill_ts      <= !fill_ts;
                    fill_cnt     <= '0;
                end else begin
                    fill_cnt <= fill_cnt + 9'd1;
                end
            end

            // ---- запись в SDRAM
            case (wst)
                W_IDLE: if (fst == F_CLEAR && !aborting) begin
                    wst <= W_CREQ;                       // стирание холста
                end else if (tst[fl_ts] == T_FULL && !aborting) begin
                    tst[fl_ts] <= T_FLUSH;
                    wrow       <= '0;
                    wst        <= W_REQ;
                end
                W_REQ: begin
                    if (!s_req) begin
                        if (aborting)
                            wst <= W_IDLE;                 // отмена: следующий пакет не начинаем
                        else if (row_in) begin
                            s_req  <= 1'b1;
                            s_addr <= line_base(wbuf, oy + w_line) + {11'd0, w_col, 3'd0};
                            rd_ptr <= {fl_ts, wrow, 3'd0};
                        end else if ({1'b0, wrow} == th_m1) begin   // строк больше нет
                            tst[fl_ts] <= T_FREE;
                            fl_ts      <= !fl_ts;
                            wst        <= W_IDLE;
                        end else begin
                            wrow <= wrow + 4'd1;
                        end
                    end else if (s_ack) begin
                        s_req <= 1'b0;
                        wcnt  <= '0;
                        wst   <= W_DATA;
                    end
                end
                W_DATA: if (s_wr_take) begin           // 8 слов подряд
                    wcnt <= wcnt + 3'd1;
                    if (wcnt == 3'd7) begin
                        if ({1'b0, wrow} == th_m1) begin
                            tst[fl_ts] <= T_FREE;
                            fl_ts      <= !fl_ts;
                            wst        <= W_IDLE;
                        end else begin
                            wrow <= wrow + 4'd1;
                            wst  <= W_REQ;
                        end
                    end
                end
                W_CREQ: begin                            // стирание: пакет нулей
                    if (!s_req) begin
                        if (aborting)
                            wst <= W_IDLE;
                        else begin
                            s_req  <= 1'b1;
                            s_addr <= line_base(wbuf, cl_line) + {11'd0, 1'b0, cl_col, 3'd0};
                        end
                    end else if (s_ack) begin
                        s_req <= 1'b0;
                        wcnt  <= '0;
                        wst   <= W_CDATA;
                    end
                end
                W_CDATA: if (s_wr_take) begin
                    wcnt <= wcnt + 3'd1;
                    if (wcnt == 3'd7) begin
                        if ({1'b0, cl_col} == cw[10:4] - 7'd1) begin
                            cl_col <= '0;
                            if (cl_line == ch - 10'd1) begin
                                // холст стёрт — кадр дальше обычным порядком
                                wst        <= W_IDLE;
                                fst        <= F_RUN;
                                geo_ok     <= 1'b1;
                                bgeo[wbuf] <= {fw[10:0], fh[9:0]};
                            end else begin
                                cl_line <= cl_line + 10'd1;
                                wst     <= W_CREQ;
                            end
                        end else begin
                            cl_col <= cl_col + 6'd1;
                            wst    <= W_CREQ;
                        end
                    end
                end
                default: wst <= W_IDLE;
            endcase

            // ---- отмена: после начатого пакета — плитки свободны, кадра нет
            if (abort && fst != F_IDLE) aborting <= 1'b1;
            if (aborting && wst == W_IDLE && !s_req) begin
                aborting <= 1'b0;
                fst      <= F_IDLE;
                for (k = 0; k < 2; k = k + 1) tst[k] <= T_FREE;
                fill_ts  <= 1'b0; fl_ts <= 1'b0; fill_cnt <= '0;
            end
        end
    end

    assign frame_ready = fst == F_DONE && !aborting;
    assign busy        = fst != F_IDLE;
endmodule
`default_nettype wire
