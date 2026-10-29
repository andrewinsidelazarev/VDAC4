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
// плитки — 8 слов, ровно пакет SDRAM. Плитка закрывается, когда в ней все точки, когда пришла
// точка другой плитки (правый край картинки, ширина не кратна 16) и в конце кадра. Строки ниже
// картинки и правее 1024 точек не пишутся.
//
// ДЕБЛОКЕР (1.43; DEBLOCK = 2 — все края, 1 — только вертикальные, 0 — нет). На малом потоке
// (0,3–0,6 бита на точку у роликов с карты Evo) кадр распадается на плоские квадраты: блоки
// яркости 8×8 и блоки цвета 16×16 (у 4:2:0 декодер растягивает цвет повтором точек 2×2). Их
// сглаживает фильтр окна края (vp_dbk_lp): по 4 точки с каждой стороны (половинки соседних блоков;
// у уменьшенной картинки — по 2, блок источника там — 4 точки холста; DB_HALF = 0 — у неё деблокера
// нет), в плоском окне — фильтр низких частот MPEG-4 с нечётным отражением за концы окна:
// ступенька становится плавным переходом, градиент не меняется, концы окна — тоже (стыки окон без
// швов). Составляющие R, G, B сглаживаются в 8 битах, до сведения к RGB565; «плоско» — одно на
// три: размах зелёной окна (≈ яркость) < 2·QP — так уходят и квадраты цвета, ступенька которых по
// R или B больше порога. QP = qy/2, qy — наибольший из шагов DC и первого AC таблицы яркости кадра
// (jpeg_dqt; у MJPEG ffmpeg шаг DC всегда 8 и о качестве не говорит). Пробы 29.10.2026: фильтр
// H.263 (по 2 точки у края) квадратов не убирал (пользователь: «как были квадраты так и
// остались»), «пологий скат» (прибавка ската от разницы средних половинок) гнул градиенты —
// решётка линий в тёмных заливках. Сначала все вертикальные края, потом горизонтальные по итогу
// первых. Окна соседних краёв не пересекаются (край — через 8 точек, окно — 8), поэтому итог от
// порядка обхода не зависит.
//   Плиток в памяти четыре (кольцо): собранная плитка T ждёт следующую — правую соседку R (её
// слова 0 и 1 — правая половина окна правого края T), потом проход деблокера по памяти плиток
// (порт A — общий со сборкой, сборка первая): вертикальные края внутри T и её правый край (меняет
// и слова R), горизонтальный край внутри T (у 4:2:0) и верхний край T. Четыре нижние строки
// каждой плитки (у уменьшенной — две) при записи в SDRAM копируются в буфер строк (lsm: 64 столбца
// плиток × 4 строки); верхний край плитки T фильтруется с ними, и они пишутся в SDRAM ещё раз —
// перед строками T (кадр показывается только целиком, двойная запись не видна).
//
// ЗЕРНО (DITHER = 1). RGB565 — не усечением: к составляющей до усечения прибавляется порог места
// точки — синий шум 64×64 (vp_noise_rom, пороги 0…15: R, B — /2, G — /4), один на R, G и B (без
// цветной ряби). Таблица каждый кадр сдвигается на случайное место (LFSR) — ровное мелкое зерно,
// как плёночное, вместо ступеней тёмных заливок (у R и B лишь 32 уровня). Пользователь, 29.10.2026:
// «дизеринг лучше … должен быть нерегулярным — как плёночное зерно»; синий шум выбран им же.
//
// Кадры FT — три, как у ft_render и fb_core: 0 и 1 — с FB_BASE + b·FT_BUF_WORDS, 2 — строки
// 0…511 с T_LO, 512…767 с T_HI; строка — 512 слов, точка x — слово x/2, младшая половина —
// чётная точка (fb_line_ram). Кадр пишется в буфер, который не выводится (fb_front) и не
// объявлен (pub_buf). Записанный кадр ждёт разрешения pub_go (темп кадров — снаружи) и
// объявляется: pub_v = 1 навсегда, pub_buf — его буфер; fb_core покажет его с начала
// следующего выходного кадра.
//
// Пары точек: декодер выдаёт строку блока слева направо, поэтому чётная точка запоминается, а
// с нечётной уходит слово {нечётная, чётная} (в памяти плиток — RGB888, 48 бит). RGB565 — при
// записи в SDRAM.
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
    parameter logic [20:0] T_HI         = 21'h0C9000,   //         строки 512…767
    parameter int          DEBLOCK      = 2,            // 0 — нет, 1 — вертикальные края, 2 — все
    parameter int          DITHER       = 1,            // 1 — зерно при сведении к RGB565
    parameter int          DB_HALF      = 1             // 1 — деблокер и у уменьшенной картинки
) (
    input  wire         clk,
    input  wire         rst_n,

    // кадр: начало (до первой точки; выбирается буфер) и конец (декодер выдал всё)
    input  wire         frame_start,
    input  wire         frame_end,
    input  wire         abort,           // импульс: отменить кадр (см. выше)
    input  wire [1:0]   mode,            // jpeg_core outport_mode_o: 2 — 4:2:0 (MCU 16 строк);
                                         // mode, img_w, img_h, qy — к первой точке кадра (SOF)
    input  wire [15:0]  img_w,
    input  wire [15:0]  img_h,
    input  wire [7:0]   qy,              // шаг квантования таблицы яркости (jpeg_dqt) — сила деблокера

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
    localparam bit DB  = DEBLOCK > 0;
    localparam bit DB2 = DEBLOCK > 1;
    localparam bit DTH = DITHER > 0;

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
    reg [5:0]  qp2;                      // деблокер: 2·QP кадра (0 — не сглаживать)
    reg [15:0] nz_lfsr;                  // зерно: сдвиг таблицы шума, новый каждый кадр
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

    // первая строка плитки ряда ty
    function automatic [9:0] tile_line0(input logic [1:0] t, input logic [9:0] ty);
        tile_line0 = t == 2'd2 ? {ty[5:0], 4'd0} : t == 2'd1 ? {ty[6:0], 3'd0} : {ty[7:0], 2'd0};
    endfunction

    // пара точек RGB888 {нечётная, чётная} → слово RGB565: усечением или с порогами зерна
    // {нечётной, чётной} (0…15: к R и B — /2, к G — /4, с насыщением)
    function automatic [15:0] px565(input logic [23:0] p, input logic [3:0] t);
        logic [8:0] r, g, b;
        r = {1'b0, p[23:16]} + {6'd0, t[3:1]};
        g = {1'b0, p[15:8]}  + {7'd0, t[3:2]};
        b = {1'b0, p[7:0]}   + {6'd0, t[3:1]};
        px565 = {r[8] ? 5'h1F : r[7:3], g[8] ? 6'h3F : g[7:2], b[8] ? 5'h1F : b[7:3]};
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
    reg [5:0]  g_qp2;                            // 2·QP: QP = (qy + 1) / 2, не больше 31
    wire [19:0] g2_w9  = {1'b0, g2_w, 3'b000} + {4'd0, g2_w};
    wire [19:0] g2_h14 = {g2_h, 4'b0000} - {3'd0, g2_h, 1'b0};
    wire [10:0] g3_cw = g3_wd ? (w3_512 ? 11'd512 : w3_640 ? 11'd640 : 11'd1024)
                              : (g3_512 ? 11'd512 : g3_640 ? 11'd640 : 11'd1024);
    wire [9:0]  g3_ch = g3_wd ? (w3_512 ? 10'd288 : w3_640 ? 10'd360 : 10'd576)
                              : (g3_512 ? 10'd384 : g3_640 ? 10'd480 : 10'd768);
    wire [5:0]  g_qyh = qy[5:0] + 6'd1;          // (qy + 1) при qy < 61
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
        g_qp2   <= qy >= 8'd61 ? 6'd62 : {g_qyh[5:1], 1'b0};
    end
    wire [20:0] g_geo = {g_w[10:0], g_h[9:0]};

    // --- плитки -------------------------------------------------------------------------------
    // Кольцо из четырёх: собирается (FILL) → собрана (FULL) → деблокер прошёл (DONE) → пишется в
    // SDRAM (FLUSH) → свободна. Указатели сборки, деблокера и записи идут по кольцу по очереди.
    localparam logic [2:0] T_FREE = 3'd0, T_FILL = 3'd1, T_FULL = 3'd2, T_DONE = 3'd3, T_FLUSH = 3'd4;
    reg [2:0]  tst [0:3];
    reg [5:0]  ttx [0:3];                // плитка: x/16 (до 64 — 1024 точки)
    reg [9:0]  tty [0:3];                // y / TH
    reg [1:0]  fill_p, eng_p, fl_p;      // собирается; ждёт деблокера; пишется следующей
    reg [8:0]  fill_cnt;                 // точек в собираемой
    reg        f_open;                   // собираемая открыта (её ttx/tty — копия в f_tx/f_ty)
    reg [5:0]  f_tx;
    reg [9:0]  f_ty;
    reg [23:0] pair_lo;                  // чётная точка пары

    // Память плиток: {плитка[1:0], строка[3:0], слово[2:0]}, слово — пара точек RGB888: две
    // памяти — чётная точка (tml) и нечётная (tmh), запись — каждой своя (деблокер пишет колонку).
    // Порт A — сборка и деблокер, порт B — чтение для SDRAM
    (* ram_style = "block" *) reg [23:0] tml [0:511];
    (* ram_style = "block" *) reg [23:0] tmh [0:511];

    // точка на картинке холста (у уменьшенной — x/2, y/2; нечётные x и y — мимо)
    wire [15:0] o_x   = half ? {1'b0, px_x[15:1]} : px_x;
    wire        drop  = half && (px_x[0] || px_y[0]);
    // точка правее картинки (дополнение MCU) — чёрная: она ложится на поле холста
    wire [23:0] px888 = o_x >= fw ? 24'h000000 : {px_r, px_g, px_b};
    // плитка: столбец — 16 точек холста (у уменьшенной — 32 точки источника), ряд — ряд MCU
    // источника (16 строк у 4:2:0, иначе 8: у уменьшенной та же высота источника, строк на холсте
    // вдвое меньше). Этот путь — в готовность для декодера (px_ready): мультиплексоров в нём мало
    wire [5:0]  p_tx  = half ? px_x[10:5] : px_x[9:4];
    wire [9:0]  p_ty  = t16 ? px_y[13:4] : px_y[12:3];
    wire [3:0]  p_row = half ? (t16 ? {1'b0, px_y[3:1]} : {2'b00, px_y[2:1]})
                             : (t16 ? px_y[3:0] : {1'b0, px_y[2:0]});
    wire        other_tile = f_open && (p_tx != f_tx || p_ty != f_ty);
    wire        slot_ok = f_open || tst[fill_p] == T_FREE;
    wire [8:0]  tile_px = th == 2'd2 ? 9'd256 : th == 2'd1 ? 9'd128 : 9'd64;

    // точка принимается в собираемую (или свободную) плитку той же плитки; точка другой
    // плитки сначала закрывает нынешнюю (такт без приёма); выброшенная уменьшением — сразу
    wire   run      = fst == F_RUN && geo_ok && !aborting;
    assign px_ready = run && (drop || (slot_ok && !other_tile));
    wire   take_px  = px_valid && px_ready && !drop;
    wire   close_now = run && px_valid && !drop && slot_ok && other_tile;
    wire   last_px  = take_px && fill_cnt == tile_px - 9'd1;

    // запись пары — тактом позже, регистром: порт A делят сборка и деблокер, и выбор между ними
    // идёт от регистра, а не от готовности точки
    reg        fwr_v;
    reg [8:0]  fwr_a;
    reg [47:0] fwr_d;
    always_ff @(posedge clk) begin
        fwr_v <= take_px && o_x[0];
        fwr_a <= {fill_p, p_row, o_x[3:1]};
        fwr_d <= {px888, pair_lo};
    end

    // --- деблокер: проход по плитке ----------------------------------------------------------
    // Операция — один край: вертикальный (фаза 0) — слова строки поперёк края (4, у уменьшенной 2);
    // горизонтальный (фазы 1 и 2) — одна колонка точек (чётная или нечётная половина слов) в строках
    // поперёк края (8, у уменьшенной 4). Шаги: чтение слов (точки окна — в p[]), подача в фильтр
    // (G, R, B; каждая дважды — окно и окно задом наперёд: вторая половина окна теми же формулами),
    // ожидание итога, запись слов из rr[] (у горизонтального — только своя половина слова: память
    // плиток и буфер строк — половинами, с отдельной записью). Регистров на одну колонку и вдвое
    // меньше сумматоров фильтра: пробная сборка с двумя колонками и шестью суммами не развелась
    // (ПЛИС заполнена на 99 %).
    // E_SET — такт после плитки: готовность следующей (регистр) считается уже по новому eng_p
    // уменьшенная картинка — для деблокера (DB_HALF = 0: в таких кадрах деблокера нет, ветка окна 4
    // точки выбрасывается синтезом); деблокер в этом кадре
    wire       hh  = DB_HALF ? half : 1'b0;
    wire       dbf = DB && (DB_HALF || !half);
    localparam logic [1:0] E_IDLE = 2'd0, E_RUN = 2'd1, E_WAIT = 2'd2, E_SET = 2'd3;
    // SP_CHK — такт перед операцией: её годность (e_okr) — регистром от новых счётчиков
    localparam logic [2:0] SP_RD = 3'd0, SP_CAP = 3'd1, SP_FEED = 3'd2, SP_PIPE = 3'd3, SP_WR = 3'd4,
                           SP_CHK = 3'd5, SP_CAP2 = 3'd6;
    reg [1:0]  est;
    reg [1:0]  e_ph;                     // 0 — вертикальные края; 1 — горизонтальный внутри плитки
                                         // (4:2:0); 2 — верхний край плитки (с буфером строк)
    reg [1:0]  e_s;                      // слот плитки
    reg [5:0]  e_tx;
    reg [9:0]  e_ty;
    reg [9:0]  e_l0;                     // первая строка плитки на картинке
    reg        e_hr;                     // правая соседка в кольце (правая половина окна края)
    reg [3:0]  e_r;                      // фаза 0: строка
    reg [1:0]  e_m;                      //         край в строке
    reg [2:0]  e_k;                      // фазы 1, 2: слово-столбец
    reg        e_col;                    //           колонка: 0 — чётная точка слова, 1 — нечётная
    reg [2:0]  e_sp;                     // шаг операции
    reg [2:0]  e_j;                      // слово операции; шаг подачи
    reg [1:0]  e_pw;                     // ожидание итога фильтра
    reg [23:0] p [0:7];                  // точки окна
    reg [23:0] rr [0:7];                 // итог
    reg        cap_v, cap_ls, cap_col;   // чтение прошлого такта: данные — в p[]
    reg [2:0]  cap_j;
    reg [5:0]  lf_tx;                    // последняя записанная в SDRAM плитка кадра
    reg [9:0]  lf_ty;
    reg        lf_v;

    wire [9:0] e_x0  = {e_tx, 4'd0};
    wire [2:0] e_m1  = {1'b0, e_m} + 3'd1;
    wire [3:0] e_qw  = hh ? {e_m1, 1'b0} : {e_m1[1:0], 2'b00};     // слово края: 2 4 6 8 | 4 8
    wire [3:0] e_th1 = th == 2'd2 ? 4'd15 : th == 2'd1 ? 4'd7 : 4'd3;
    wire       e_v   = !DB2 || e_ph == 2'd0;
    wire [3:0] e_rw  = hh ? 4'd2 : 4'd4;                            // полуокно: точек (строк)
    wire [2:0] e_nw1 = e_v ? (hh ? 3'd1 : 3'd3) : (hh ? 3'd3 : 3'd7);   // слов в операции − 1
    // подача j = 0…5: составляющая G, G, R, R, B, B; нечётная — окно задом наперёд
    wire [1:0] e_ch  = e_j[2] ? 2'd2 : e_j[1] ? 2'd0 : 2'd1;
    wire       e_rev = e_j[0];

    // годна ли операция: всё окно — на картинке
    wire [10:0] e_xv   = {1'b0, e_x0} + {6'd0, e_qw, 1'b0} + {7'd0, e_rw};       // x края + полуокно
    wire        e_ok_v = ({6'd0, e_l0} + {12'd0, e_r} < fh) && ({5'd0, e_xv} <= fw) && (e_qw != 4'd8 || e_hr);
    wire [4:0]  e_yv   = e_ph == 2'd1 ? (hh ? 5'd6 : 5'd12) : {1'b0, e_rw};   // y края + полуокно
    wire        e_ok_h = ({6'd0, e_l0} + {11'd0, e_yv} <= fh) && ({6'd0, e_x0} + {12'd0, e_k, e_col} < fw);
    wire        e_ok   = e_v ? e_ok_v : e_ok_h;

    // адрес слова e_j операции: в памяти плиток или в буфере строк (верхний край: слова 0…e_rw−1 —
    // строки над плиткой из буфера, дальше — её строки 0…)
    wire [3:0] e_wi  = e_qw - (hh ? 4'd1 : 4'd2) + {1'b0, e_j};    // вертикальный: слово в строке
    wire       e_lsw = DB2 && e_ph == 2'd2 && {1'b0, e_j} < e_rw;     // слово — из буфера строк
    wire [3:0] e_row = e_v ? e_r : e_ph == 2'd1 ? (hh ? 4'd2 : 4'd4) + {1'b0, e_j}
                                                : (hh ? {3'd0, e_j[0]} : {2'd0, e_j[1:0]});
    wire [8:0] e_ta  = e_v ? {e_wi[3] ? e_s + 2'd1 : e_s, e_r, e_wi[2:0]} : {e_s, e_row, e_k};
    wire [10:0] e_la = {e_tx, e_j[1:0], e_k};
    reg        e_okr;                                                 // e_ok прошлого такта
    wire       e_rd  = est == E_RUN && e_sp == SP_RD && (e_j != 3'd0 || e_okr);
    wire       e_wr  = est == E_RUN && e_sp == SP_WR;
    wire       e_tm  = (e_rd || e_wr) && !e_lsw;                      // нужен порт A памяти плиток
    wire       e_go  = !(e_tm && fwr_v);                              // сборка пишет — шаг ждёт
    wire [47:0] e_wd = e_v ? {rr[{e_j[1:0], 1'b1}], rr[{e_j[1:0], 1'b0}]} : {rr[e_j], rr[e_j]};
    wire       e_wlo = e_v || !e_col;                                 // пишутся половины слова
    wire       e_whi = e_v || e_col;

    // порт A памяти плиток (половины слова — отдельные памяти)
    wire        a_wl = fwr_v || (e_tm && e_wr && e_wlo);
    wire        a_wh = fwr_v || (e_tm && e_wr && e_whi);
    wire [8:0]  a_ad = fwr_v ? fwr_a : e_ta;
    wire [47:0] a_wd = fwr_v ? fwr_d : e_wd;
    // режим записи — обычный: в такт записи выход не меняется (Gowin DPB не знает «чтение до
    // записи», PA2122); деблокер в такт записи и не читает
    reg  [23:0] a_ql, a_qh;
    always_ff @(posedge clk) begin
        if (a_wl) tml[a_ad] <= a_wd[23:0];
        else a_ql <= tml[a_ad];
    end
    always_ff @(posedge clk) begin
        if (a_wh) tmh[a_ad] <= a_wd[47:24];
        else a_qh <= tmh[a_ad];
    end
    wire [47:0] a_q = {a_qh, a_ql};

    // буфер строк (DEBLOCK = 2): {столбец плиток, строка (4 нижние), слово}, половинами слова;
    // порт A — деблокер, порт B — запись в SDRAM (чтение строк над плиткой и копия нижних строк)
    wire [47:0] ls_aq, ls_bq;
    wire        ls_a_we = DB2 && e_wr && e_lsw;
    wire        ls_b_we;
    wire [10:0] ls_b_ad;
    reg  [47:0] rd_q;
    generate if (DB2) begin : g_ls
        (* ram_style = "block" *) reg [23:0] lsl [0:2047];
        (* ram_style = "block" *) reg [23:0] lsh [0:2047];
        reg [23:0] aql, aqh, bql, bqh;
        always_ff @(posedge clk) begin
            if (ls_a_we && e_wlo) lsl[e_la] <= e_wd[23:0];
            else aql <= lsl[e_la];
        end
        always_ff @(posedge clk) begin
            if (ls_a_we && e_whi) lsh[e_la] <= e_wd[47:24];
            else aqh <= lsh[e_la];
        end
        always_ff @(posedge clk) begin
            if (ls_b_we) lsl[ls_b_ad] <= rd_q[23:0];
            else bql <= lsl[ls_b_ad];
        end
        always_ff @(posedge clk) begin
            if (ls_b_we) lsh[ls_b_ad] <= rd_q[47:24];
            else bqh <= lsh[ls_b_ad];
        end
        assign ls_aq = {aqh, aql};
        assign ls_bq = {bqh, bql};
    end else begin : g_nols
        assign ls_aq = '0;
        assign ls_bq = '0;
    end endgenerate

    // фильтр: вход — составляющая e_ch точек окна (при e_rev — задом наперёд: окно 8 — i ↔ 7−i,
    // окно 4 — i ↔ 3−i), регистром. Решение «плоско» — по зелёной (≈ яркость), одно на R, G, B:
    // цветовая ступенька (квадраты цвета 16×16 у 4:2:0) в плоском по яркости месте сглаживается,
    // хотя по R или B она больше 2·QP (пользователь, 29.10.2026: полосы на красном фоне Muse)
    function automatic [7:0] chn(input logic [23:0] px, input logic [1:0] c);
        chn = c == 2'd0 ? px[23:16] : c == 2'd1 ? px[15:8] : px[7:0];
    endfunction
    wire [7:0]  fx [0:7];
    genvar gi;
    generate for (gi = 0; gi < 8; gi = gi + 1) begin : g_fx
        assign fx[gi] = chn(p[gi], e_ch);
    end endgenerate
    reg  [63:0] fi;
    wire [31:0] fo;
    integer fl, rl;                      // счётчики циклов (у каждого блока свой)
    always_ff @(posedge clk) begin
        for (fl = 0; fl < 4; fl = fl + 1)
            fi[8 * fl +: 8] <= !e_rev ? fx[fl] : hh ? fx[3 - fl] : fx[7 - fl];
        for (fl = 4; fl < 8; fl = fl + 1)
            fi[8 * fl +: 8] <= !e_rev ? fx[fl] : fx[7 - fl];
    end
    // «плоско»: размах зелёной точек окна < 2·QP — из p[] (окно собрано к SP_CAP2) в три ступени:
    // пары, наибольшая и наименьшая, сравнение — к второй ступени фильтра первой подачи; до
    // следующего окна p[] не меняется. Отдельно от фильтра: дерево сравнений после выбора
    // составляющей не укладывалось в такт (проба 21p2: fi → mx1 −0,8 нс, gx → g_flat −0,1 нс)
    function automatic [7:0] mx2(input logic [7:0] a, input logic [7:0] b);
        mx2 = a > b ? a : b;
    endfunction
    function automatic [7:0] mn2(input logic [7:0] a, input logic [7:0] b);
        mn2 = a < b ? a : b;
    endfunction
    reg [7:0] gx [0:3];                  // ступень 1: пары
    reg [7:0] gn [0:3];
    reg [7:0] gmx_q, gmn_q;              // ступень 2
    reg       g_flat;
    integer gl;
    wire [7:0] gx01 = mx2(gx[0], gx[1]), gn01 = mn2(gn[0], gn[1]);
    wire [7:0] gmx  = hh ? gx01 : mx2(gx01, mx2(gx[2], gx[3]));
    wire [7:0] gmn  = hh ? gn01 : mn2(gn01, mn2(gn[2], gn[3]));
    always_ff @(posedge clk) begin
        for (gl = 0; gl < 4; gl = gl + 1) begin
            gx[gl] <= mx2(p[2 * gl][15:8], p[2 * gl + 1][15:8]);
            gn[gl] <= mn2(p[2 * gl][15:8], p[2 * gl + 1][15:8]);
        end
        gmx_q  <= gmx;
        gmn_q  <= gmn;
        g_flat <= {1'b0, gmx_q} - {1'b0, gmn_q} < {3'd0, qp2};
    end
    vp_dbk_lp filt (.clk(clk), .half(hh), .flat(g_flat), .vin(fi), .vout(fo));
    // метка подачи: итог фильтра — через 3 такта (вход, две ступени), в rr — к 4-му; прямой ход
    // пишет o0…o3 в rr[0…3], обратный — в rr[7…4] (у окна 4 — o0, o1 в rr[3], rr[2])
    reg        ft0_v, ft1_v, ft2_v, ft0_rev, ft1_rev, ft2_rev;
    reg [1:0]  ft0_ch, ft1_ch, ft2_ch;
    wire       feed = est == E_RUN && e_sp == SP_FEED;
    function automatic [23:0] put(input logic [23:0] px, input logic [1:0] c, input logic [7:0] x);
        put = c == 2'd0 ? {x, px[15:0]} : c == 2'd1 ? {px[23:16], x, px[7:0]} : {px[23:8], x};
    endfunction
    always_ff @(posedge clk) begin
        ft0_v <= feed;  ft0_rev <= e_rev;   ft0_ch <= e_ch;
        ft1_v <= ft0_v; ft1_rev <= ft0_rev; ft1_ch <= ft0_ch;
        ft2_v <= ft1_v; ft2_rev <= ft1_rev; ft2_ch <= ft1_ch;
        if (ft2_v) begin
            if (!ft2_rev) begin
                for (rl = 0; rl < 4; rl = rl + 1) rr[rl] <= put(rr[rl], ft2_ch, fo[8 * rl +: 8]);
            end else if (!hh) begin
                for (rl = 0; rl < 4; rl = rl + 1) rr[7 - rl] <= put(rr[7 - rl], ft2_ch, fo[8 * rl +: 8]);
            end else begin
                rr[3] <= put(rr[3], ft2_ch, fo[7:0]);
                rr[2] <= put(rr[2], ft2_ch, fo[15:8]);
            end
        end
        // чтение: данные — тактом позже; вертикальный край — точки слова j в p[2j], p[2j+1],
        // горизонтальный — точка своей колонки в p[j]
        cap_v   <= e_rd && e_go;
        cap_ls  <= e_lsw;
        cap_j   <= e_j;
        cap_col <= e_col;
        if (cap_v) begin
            if (e_v) begin
                p[{cap_j[1:0], 1'b0}] <= cap_ls ? ls_aq[23:0] : a_q[23:0];
                p[{cap_j[1:0], 1'b1}] <= cap_ls ? ls_aq[47:24] : a_q[47:24];
            end else begin
                p[cap_j] <= cap_col ? (cap_ls ? ls_aq[47:24] : a_q[47:24]) : (cap_ls ? ls_aq[23:0] : a_q[23:0]);
            end
        end
    end

    // деблокер: следующая операция плитки (или фаза, или конец плитки)
    wire e_m_last = e_m == (hh ? 2'd3 : 2'd1);
    wire e_k_last = e_k == 3'd7 && e_col;
    wire e_r_last = e_r == e_th1;
    wire e_next   = est == E_RUN && (e_sp == SP_RD && e_j == 3'd0 && !e_okr || e_sp == SP_WR && e_j == e_nw1 && e_go);
    // плитка, которую ждёт деблокер: собрана, и собрана правая соседка (или её нет)
    wire [1:0]  eng_n   = eng_p + 2'd1;
    wire [10:0] eng_xr  = {1'b0, ttx[eng_p], 4'd0} + 11'd16 + {7'd0, e_rw};   // правый край + полуокно
    wire        eng_nr  = {5'd0, eng_xr} <= fw;                       // правая соседка нужна
    wire        eng_rdy = tst[eng_p] == T_FULL &&
                          (!dbf || !eng_nr || tst[eng_n] == T_FULL || fst == F_END);
    // верхний край ждёт, пока плитка над ней (её нижние строки — в буфере строк) уйдёт в SDRAM
    wire        e_above = lf_v && (lf_ty == e_ty || lf_ty + 10'd1 == e_ty && lf_tx >= e_tx);
    // регистрами: от «размер картинки, плитка» до состояний плиток путь не укладывался в такт
    // (проба 21: half → e_ok → tst −2,2 нс); готовность после смены eng_p — через E_SET, ожидание
    // верхней плитки монотонно, годность операции — через SP_CHK
    reg         eng_rdy_r, eng_hr_r, e_above_r;
    always_ff @(posedge clk) begin
        eng_rdy_r <= eng_rdy;
        eng_hr_r  <= eng_nr && tst[eng_n] == T_FULL;
        e_above_r <= e_above;
        e_okr     <= e_ok;
    end

    // --- запись в SDRAM: плитки (строка — пакет 8 слов) и стирание холста ------------------------
    localparam logic [2:0] W_IDLE = 3'd0, W_REQ = 3'd1, W_DATA = 3'd2, W_CREQ = 3'd3, W_CDATA = 3'd4;
    reg [2:0]  wst;
    reg [3:0]  wrow;
    reg [2:0]  wcnt;
    reg        wls;                      // пишутся строки буфера строк (над плиткой)
    reg [5:0]  w_tx;                     // плитка в записи
    reg [9:0]  w_ty;
    reg [10:0] rd_ptr;
    wire [10:0] rd_next = s_wr_take ? rd_ptr + 11'd1 : rd_ptr;
    // слово плитки правее картинки — нули: таких точек декодер может не выдать вовсе (MCU 8×8,
    // ширина кратна 8, но не 16), и в памяти плиток там старое
    wire       w_out   = {5'd0, w_tx, wcnt, 2'b00} >= {fw[14:0], 1'b0};

    wire [9:0] w_l0   = tile_line0(th, w_ty);
    wire [9:0] w_line = wls ? w_l0 - {6'd0, e_rw} + {6'd0, wrow} : w_l0 + {6'd0, wrow};
    wire [4:0] th_m1  = th == 2'd2 ? 5'd15 : th == 2'd1 ? 5'd7 : 5'd3;
    // строку пишем, если она в картинке и в пределах кадра FT
    wire       row_in = ({6'd0, w_line} < fh) && ({6'd0, w_tx, 4'd0} < fw);
    wire [6:0] w_col  = {1'b0, ox16} + {1'b0, w_tx};           // пакет в строке холста
    // нижние строки плитки (4, у уменьшенной 2) — в буфер строк, пока уходят в SDRAM
    wire [1:0] w_lsr  = hh ? {1'b0, wrow[0]} : wrow[1:0];      // строка буфера
    assign ls_b_we = DB2 && dbf && wst == W_DATA && !wls && s_wr_take && {1'b0, wrow} > th_m1 - {1'b0, e_rw};
    assign ls_b_ad = wls ? rd_next : {w_tx, w_lsr, wcnt};

    always_ff @(posedge clk) rd_q <= {tmh[rd_next[8:0]], tml[rd_next[8:0]]};

    // зерно: порог места — таблица 64×64 со сдвигом кадра; читается вместе со словом (rd_next)
    wire [7:0]  nz_q;
    wire [5:0]  nz_y = w_line[5:0] + nz_lfsr[10:5];
    wire [4:0]  nz_x = {w_tx[1:0], rd_next[2:0]} + nz_lfsr[4:0];
    generate if (DTH) begin : g_nz
        vp_noise_rom nz_rom (.clk(clk), .addr({nz_y, nz_x}), .q(nz_q));
    end else begin : g_nonz
        assign nz_q = 8'h00;
    end endgenerate
    wire [47:0] w_src = wls ? ls_bq : rd_q;
    assign s_wdata = (wst == W_CDATA || w_out) ? 32'd0
                   : {px565(w_src[47:24], nz_q[7:4]), px565(w_src[23:0], nz_q[3:0])};

    integer k;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            fst <= F_IDLE; wbuf <= 2'd0; th <= 2'd1; t16 <= 1'b0; half <= 1'b0; fw <= '0; fh <= '0; got_px <= 1'b0;
            cw <= 11'd512; ch <= 10'd384; ox16 <= '0; oy <= '0; geo_ok <= 1'b0; qp2 <= '0; nz_lfsr <= 16'hACE1;
            aborting <= 1'b0; cl_line <= '0; cl_col <= '0;
            for (k = 0; k < 3; k = k + 1) bgeo[k] <= '0;
            for (k = 0; k < 4; k = k + 1) begin tst[k] <= T_FREE; ttx[k] <= '0; tty[k] <= '0; end
            fill_p <= '0; eng_p <= '0; fl_p <= '0; fill_cnt <= '0; pair_lo <= '0;
            f_open <= 1'b0; f_tx <= '0; f_ty <= '0;
            est <= E_IDLE; e_ph <= '0; e_s <= '0; e_tx <= '0; e_ty <= '0; e_l0 <= '0; e_hr <= 1'b0;
            e_r <= '0; e_m <= '0; e_k <= '0; e_col <= 1'b0; e_sp <= SP_RD; e_j <= '0; e_pw <= '0;
            lf_v <= 1'b0; lf_tx <= '0; lf_ty <= '0;
            wst <= W_IDLE; wrow <= '0; wcnt <= '0; wls <= 1'b0; w_tx <= '0; w_ty <= '0;
            rd_ptr <= '0; s_req <= 1'b0; s_addr <= '0;
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
                    lf_v     <= 1'b0;
                    nz_lfsr  <= {1'b0, nz_lfsr[15:1]} ^ (nz_lfsr[0] ? 16'hB400 : 16'h0000);
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
                        qp2  <= g_qp2;
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
                    if (f_open) begin                    // недособранная плитка — тоже в память
                        tst[fill_p] <= T_FULL;
                        fill_p      <= fill_p + 2'd1;
                        fill_cnt    <= '0;
                        f_open      <= 1'b0;
                    end
                end
                F_END: if (tst[0] == T_FREE && tst[1] == T_FREE && tst[2] == T_FREE && tst[3] == T_FREE)
                    fst <= F_DONE;
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
                tst[fill_p] <= T_FULL;
                fill_p      <= fill_p + 2'd1;
                fill_cnt    <= '0;
                f_open      <= 1'b0;
            end else if (take_px) begin
                got_px <= 1'b1;
                if (!o_x[0]) pair_lo <= px888;
                if (!f_open) begin
                    tst[fill_p] <= T_FILL;
                    ttx[fill_p] <= p_tx;
                    tty[fill_p] <= p_ty;
                    f_open      <= 1'b1;
                    f_tx        <= p_tx;
                    f_ty        <= p_ty;
                end
                if (last_px) begin
                    tst[fill_p] <= T_FULL;
                    fill_p      <= fill_p + 2'd1;
                    fill_cnt    <= '0;
                    f_open      <= 1'b0;
                end else begin
                    fill_cnt <= fill_cnt + 9'd1;
                end
            end

            // ---- деблокер
            case (est)
                E_IDLE: if (eng_rdy_r && !aborting) begin
                    if (!dbf) begin
                        tst[eng_p] <= T_DONE;            // без деблокера — сразу в запись
                        eng_p      <= eng_n;
                        est        <= E_SET;
                    end else begin
                        est  <= E_RUN;
                        e_s  <= eng_p;
                        e_tx <= ttx[eng_p];
                        e_ty <= tty[eng_p];
                        e_l0 <= tile_line0(th, tty[eng_p]);
                        e_hr <= eng_hr_r;
                        e_ph <= 2'd0; e_r <= '0; e_m <= '0; e_k <= '0; e_col <= 1'b0;
                        e_sp <= SP_CHK; e_j <= '0;
                    end
                end
                E_WAIT: if (e_above_r) est <= E_RUN;
                E_SET:  est <= E_IDLE;
                E_RUN: begin
                    // шаги операции
                    case (e_sp)
                        SP_RD: if (e_okr || e_j != 3'd0) begin
                            if (e_go) begin
                                if (e_j == e_nw1) begin e_sp <= SP_CAP; e_j <= '0; end
                                else e_j <= e_j + 3'd1;
                            end
                        end
                        SP_CAP: e_sp <= SP_CAP2;            // окно собирается в p[]
                        SP_CAP2: begin e_sp <= SP_FEED; e_j <= '0; end  // «плоско»: ступень 1
                        SP_FEED: begin                   // G G R R B B: окно и задом наперёд
                            if (e_j == 3'd5) begin e_sp <= SP_PIPE; e_pw <= '0; e_j <= '0; end
                            else e_j <= e_j + 3'd1;
                        end
                        SP_PIPE: begin
                            e_pw <= e_pw + 2'd1;
                            if (e_pw == 2'd2) e_sp <= SP_WR;
                        end
                        SP_WR: if (e_go && e_j != e_nw1) e_j <= e_j + 3'd1;
                        SP_CHK: e_sp <= SP_RD;
                        default: e_sp <= SP_RD;
                    endcase
                    // следующая операция
                    if (e_next) begin
                        e_sp <= SP_CHK;
                        e_j  <= '0;
                        if (e_v) begin
                            if (!e_m_last) e_m <= e_m + 2'd1;
                            else begin
                                e_m <= '0;
                                if (!e_r_last) e_r <= e_r + 4'd1;
                                else if (DB2 && t16) begin e_ph <= 2'd1; e_k <= '0; end
                                else if (DB2 && e_ty != 10'd0) begin e_ph <= 2'd2; e_k <= '0; est <= E_WAIT; end
                                else begin tst[e_s] <= T_DONE; eng_p <= eng_n; est <= E_SET; end
                            end
                        end else begin
                            e_col <= !e_col;
                            if (!e_k_last) begin if (e_col) e_k <= e_k + 3'd1; end
                            else if (e_ph == 2'd1 && e_ty != 10'd0) begin e_ph <= 2'd2; e_k <= '0; est <= E_WAIT; end
                            else begin tst[e_s] <= T_DONE; eng_p <= eng_n; est <= E_SET; end
                        end
                    end
                end
                default: est <= E_IDLE;
            endcase

            // ---- запись в SDRAM
            case (wst)
                W_IDLE: if (fst == F_CLEAR && !aborting) begin
                    wst <= W_CREQ;                       // стирание холста
                end else if (tst[fl_p] == T_DONE && !aborting) begin
                    tst[fl_p] <= T_FLUSH;
                    w_tx      <= ttx[fl_p];
                    w_ty      <= tty[fl_p];
                    wls       <= DB2 && dbf && tty[fl_p] != 10'd0;
                    wrow      <= '0;
                    wst       <= W_REQ;
                end
                W_REQ: begin
                    if (!s_req) begin
                        if (aborting)
                            wst <= W_IDLE;                 // отмена: следующий пакет не начинаем
                        else if (row_in) begin
                            s_req  <= 1'b1;
                            s_addr <= line_base(wbuf, oy + w_line) + {11'd0, w_col, 3'd0};
                            rd_ptr <= wls ? {w_tx, wrow[1:0], 3'd0} : {2'b00, fl_p, wrow, 3'd0};
                        end else if (wls) begin                // (строки над плиткой — всегда в картинке)
                            if (wrow == e_rw - 4'd1) begin wls <= 1'b0; wrow <= '0; end
                            else wrow <= wrow + 4'd1;
                        end else if ({1'b0, wrow} == th_m1) begin   // строк больше нет
                            tst[fl_p] <= T_FREE;
                            fl_p      <= fl_p + 2'd1;
                            lf_v      <= 1'b1; lf_tx <= w_tx; lf_ty <= w_ty;
                            wst       <= W_IDLE;
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
                        if (wls) begin
                            if (wrow == e_rw - 4'd1) begin wls <= 1'b0; wrow <= '0; end
                            else wrow <= wrow + 4'd1;
                            wst <= W_REQ;
                        end else if ({1'b0, wrow} == th_m1) begin
                            tst[fl_p] <= T_FREE;
                            fl_p      <= fl_p + 2'd1;
                            lf_v      <= 1'b1; lf_tx <= w_tx; lf_ty <= w_ty;
                            wst       <= W_IDLE;
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
                for (k = 0; k < 4; k = k + 1) tst[k] <= T_FREE;
                fill_p <= '0; eng_p <= '0; fl_p <= '0; fill_cnt <= '0; f_open <= 1'b0;
                est    <= E_IDLE; lf_v <= 1'b0;
            end
        end
    end

    assign frame_ready = fst == F_DONE && !aborting;
    assign busy        = fst != F_IDLE;
endmodule
`default_nettype wire
