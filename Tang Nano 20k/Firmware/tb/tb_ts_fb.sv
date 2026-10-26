`timescale 1ns/1ps
//
// Сквозная проверка кадрового буфера: модель растра ZX Evolution → мультиплексор 2×8 →
// приём → SDRAM (модель микросхемы с проверкой протокола) → развёртка 576p50 → TMDS.
// Выход проверяется по декодированному TMDS — так, как его увидит монитор.
//
// Что проверяется:
//   • развёртка выхода: полная строка, ширина импульсов, число строк, полярность,
//     период кадра — стандарт 576p50 (или уменьшенный растр в режиме MINI);
//   • каждый выходной кадр — целиком один входной кадр, без разрывов: картинка
//     источника зависит от номера кадра во всех пикселях, номер закодирован в строке 0;
//   • номера показанных кадров не убывают; повторы (выход быстрее) и пропуски
//     (вход быстрее) считаются и сверяются с отношением частот;
//   • после остановки PCLK выход продолжает развёртку и держит последний кадр;
//   • модель SDRAM не нашла нарушений, FIFO захвата не переполнялся, строки успевали.
//
// Параметры:
//   MINI=1 — уменьшенные растры, много кадров за короткое время (логика буферов);
//   MINI=0 — настоящие размеры растров Evo и 576p50 (несколько кадров, долго);
//   RASTER — исходный растр источника (таблица set_raster ниже):
//     0 TS-Config Pentagon 896×640, 1 TS-Config PENT_312 896×624, 2 Base Conf Pentagon,
//     3 Base Conf 48k 896×624, 4 Base Conf 128k 912×622 (настоящие размеры);
//     5 уменьшенный A 100×44, 6 уменьшенный B 104×42 с положительными синхроимпульсами;
//     7 уменьшенный «незнакомый» растр SRC_H_TOTAL×SRC_V_TOTAL — окно по центру;
//   SWITCH_TO/SWITCH_AT — сменить растр на SWITCH_TO после входного кадра SWITCH_AT (−1 — нет);
//   STOP_AFTER — через сколько входных кадров остановить PCLK (0 — не останавливать);
//   SHIFT_X/SHIFT_Y — сдвинуть картинку источника от расчётного места: проверка
//     автоподстройки окна (кадры до подстройки не сверяются, после — до точки);
//   OUT_FRAMES — сколько выходных кадров проверить;
//   PATTERN=1 — Evo нет совсем (PCLK не идёт): источник AUTO переходит на испытательную
//     таблицу, она пишется в SDRAM и читается обратно; CAL=1 — перед этим калибровка точки
//     выборки чтения. Проверяется карта годных точек (CAL_MAP), выбранная точка (RD_EXPECT)
//     и каждый кадр таблицы на выходе до точки (кроме полосы движущегося квадрата);
//   OVL_TEST=1 (с PATTERN, без поля; только ядро с Evo — у ядра FT812 оверлеев нет) — оверлеи
//     ESP: холсты пишутся окном LINK, затем
//     регистры; с кадра, где они взяты, точки в прямоугольниках сверяются со смешением.
//   AVG_TEST=1 (ядро TS-Config без FT812) — FRAME BLEND, включённый ячейкой настроек во flash
//     Tang (бит 19 слова 1). Картинка: верхняя четверть меняется каждый кадр, вторая стоит,
//     нижняя половина — два кадра A/B через один (мерцание демо). Какой кадр в каком буфере
//     и какая пара взята на выходной кадр, стенд видит в fb_core; ожидаемая точка — среднее
//     пары по таблице fb_avg_q там, где кадры различаются, иначе — точка кадра.
//
// Модель SDRAM тактируется, как на плате, инверсией клока логики и выдаёт данные чтения
// с паспортными tAC/tOH (между словами — случайное слово): неверная точка выборки чтения
// ловится.
//
// Ключи запуска:
//   +img=файл.hex — вместо узора подать картинку 720×576 (scripts/scr2hex.py из экрана .scr);
//                   номер кадра по-прежнему в строке 0, а внизу по бордюру едет маркер,
//                   зависящий от номера кадра, — склейка двух кадров видна и тесту, и глазу;
//   +ppm          — выгружать каждый выходной кадр с картинкой, декодированный из TMDS,
//                   в fb_out_NN.ppm (номер — входной кадр).
//
module tb_ts_fb #(
    parameter bit MINI        = 1'b1,
    parameter int RASTER      = MINI ? 5 : 0,
    parameter int SRC_H_TOTAL = 100,          // только для RASTER=7
    parameter int SRC_V_TOTAL = 44,
    parameter int SWITCH_TO   = -1,
    parameter int SWITCH_AT   = 0,
    parameter int STOP_AFTER  = 0,
    parameter int SHIFT_X     = 0,            // картинка источника сдвинута от профиля
    parameter int SHIFT_Y     = 0,
    parameter bit AUTO_ALIGN  = 1'b1,
    parameter int OUT_FRAMES  = MINI ? 40 : 4,
    parameter bit PATTERN     = 1'b0,
    parameter bit CAL         = PATTERN,
    parameter logic [3:0] CAL_MAP = 4'b0011,  // годные точки при tAC 6 нс, tOH 2,5 нс
    // то же для SDRAM на 2 × clk_mem (SD_FAST, ядро без Evo; CL 3, 135 МГц): фронты логики
    // попадают в переход между словами, годна только точка 1 (спад, dq_neg)
    parameter logic [3:0] CAL_MAP_SD = 4'b0010,
    parameter int RD_EXPECT   = 1,
    parameter int RD_FIXED    = 1,             // точка выборки без калибровки
    parameter bit LINK_TEST   = PATTERN,       // обмен с регистрами по LINK, как его ведёт ESP
    parameter real LINK_NS    = 25.0,          // период LINK_SCK (40 МГц)
    parameter int BORDER      = 0,             // MINI: чёрное поле вокруг картинки (окно вывода)
    parameter bit HD          = 1'b0,          // MINI=0: выход 1280×720p50, картинка на всю высоту
    parameter bit ASPECT      = 1'b0,          // HD: 0 — 4:3 (960×720), 1 — 16:9 (1280×720)
    parameter int SIZE        = 0,             // HD: высота 0 — 100 %, 1 — 95 %, 2 — 90 %
    parameter bit OVL_TEST    = PATTERN && MINI && BORDER == 0,
    // Состав ядра (multiboot): только Evo — HAS_FT = 0; только FT812 — HAS_EVO = 0 (с PATTERN
    // источник — TEST: без захвата AUTO таблицу не включает)
    parameter bit HAS_EVO     = 1'b1,
    parameter bit HAS_FT      = 1'b1,
    // Автопереход: ядро без FT812 — по обмену с FT812 после «ARM1», ядро FT812 — через
    // SWITCH_MS без обмена при M_SEL = 0 (вместо записей BOOT в проверке LINK)
    parameter bit SWITCH_TEST = 1'b0,
    parameter bit SW_READ     = 1'b0,          // SWITCH_TEST ядра TS: запуск чипа — чтением REG_ID, а не ACTIVE
    parameter int SWITCH_MS   = SWITCH_TEST ? 1 : 500,
    parameter bit AVG_TEST    = 1'b0           // FRAME BLEND (см. выше)
);
    `include "fb_pattern_fn.svh"
    `include "fb_avg_tab.svh"
    initial if (AVG_TEST && (HAS_FT || !HAS_EVO || PATTERN))
        $fatal(1, "AVG_TEST — только ядро TS-Config без FT812 (HAS_FT = 0) и с картинкой Evo");
    // ------------------------------------------------------------------
    // Геометрия
    // ------------------------------------------------------------------
    localparam int H_ACTIVE    = MINI ? 64  : 720;     // картинка
    localparam int V_ACTIVE    = MINI ? 32  : 576;
    localparam bit HD_OUT      = !MINI && HD;
    // выход: кадр OUT_W×OUT_H, картинка с (IMG_X, IMG_Y)
    localparam int OUT_W  = MINI ? H_ACTIVE + 2 * BORDER : (HD_OUT ? 1280 : 720);
    localparam int OUT_H  = MINI ? V_ACTIVE + BORDER     : (HD_OUT ? 720  : 576);
    localparam int IMG_X  = (OUT_W - H_ACTIVE) / 2;
    localparam int IMG_Y  = (OUT_H - V_ACTIVE) / 2;
    // 720p: картинка на всю высоту (fb_scanout, SCALE), по ширине 4:3 (960) или весь кадр;
    // точка вывода (qx, qy) берёт точку источника ⌊qx·H_ACTIVE/PIC_W⌋, ⌊qy·V_ACTIVE/PIC_H⌋
    localparam bit SCALED = HD_OUT;
    localparam int PCT    = (SIZE == 1) ? 95 : (SIZE == 2) ? 90 : 100;
    localparam int PIC_W  = SCALED ? (ASPECT ? OUT_W : H_ACTIVE * 4 / 3) * PCT / 100 : H_ACTIVE;
    localparam int PIC_H  = SCALED ? OUT_H * PCT / 100 : V_ACTIVE;
    localparam int PIC_X  = (OUT_W - PIC_W) / 2;
    localparam int PIC_Y  = (OUT_H - PIC_H) / 2;
    localparam int H_FP   = MINI ? 4  : (HD_OUT ? 440 : 12);
    localparam int H_SYNC = MINI ? 8  : (HD_OUT ? 40  : 64);
    localparam int H_BP   = MINI ? 12 : (HD_OUT ? 220 : 68);
    localparam int V_FP   = MINI ? 2  : 5;
    localparam int V_SYNC = MINI ? 2  : 5;
    localparam int V_BP   = MINI ? 4  : (HD_OUT ? 20 : 39);
    localparam bit SYNC_NEG = !HD_OUT;                 // у 720p синхроимпульсы положительные
    localparam int H_TOTAL = OUT_W + H_FP + H_SYNC + H_BP;
    localparam int V_TOTAL = OUT_H + V_FP + V_SYNC + V_BP;

    // ------------------------------------------------------------------
    // Такты. Три независимых генератора, как на плате: PCLK Evo и кварц Tang не связаны.
    // ------------------------------------------------------------------
    localparam real T_PIX = HD_OUT ? 1000.0 / 74.25 : 1000.0 / 27.0;    // 74,25 / 27 МГц
    localparam real T_MEM = HD_OUT ? 1000.0 / 61.875 : 1000.0 / 67.5;   // 61,875 / 67,5 МГц
    reg clk_cap = 1'b0, src_clk = 1'b0;         // 56 МГц и PCLK 28 МГц = /2
    reg pixel_clk = 1'b0, clk_mem = 1'b0;
    reg pclk_run = !PATTERN;
    always #8.9285 if (pclk_run) clk_cap = ~clk_cap;
    always @(posedge clk_cap) src_clk <= ~src_clk;
    initial begin #3.1; forever #(T_PIX/2) pixel_clk = ~pixel_clk; end
    initial begin #1.7; forever #(T_MEM/2) clk_mem = ~clk_mem; end
    // SDRAM ядра FT812 — на 2 × clk_mem (SD_FAST); фронт clk_sd — на каждом фронте clk_mem
    localparam bit SDF = HAS_FT && !HAS_EVO;
    localparam bit OVL_ON = OVL_TEST && HAS_EVO;     // у ядра FT812 оверлеев нет
    // начало кадра оверлея — проводом: у ядра без Evo экземпляра fb_ovl нет (g_no_ovl)
    wire ovl_frame_start;
    generate if (HAS_EVO) begin : g_ovl_fs
        assign ovl_frame_start = dut.g_ovl.ovl.frame_start;
    end else begin : g_no_ovl_fs
        assign ovl_frame_start = 1'b0;
    end endgenerate
    localparam logic [3:0] CAL_EXP = SDF ? CAL_MAP_SD : CAL_MAP;
    reg clk_sd = 1'b1;
    initial begin #1.7; forever #(T_MEM/4) clk_sd = ~clk_sd; end

    reg rst_n = 1'b1;                           // спад в начале — асинхронный сброс всех доменов
    initial #1 rst_n = 1'b0;

    // ------------------------------------------------------------------
    // Растр источника. Картинка ставится туда, где её ищет профиль прошивки:
    // по горизонтали на отсчёт дальше смещения профиля (задержка приёма, см. tb_ts_video_chain).
    // ------------------------------------------------------------------
    integer src_ht, src_vt, src_hs, src_vs, src_hoff, src_voff, exp_mode;
    reg     src_hs_pos, src_vs_pos;
    integer raster_now = -1;

    task automatic set_raster(input integer k);
        begin
            src_hs_pos = 1'b0; src_vs_pos = 1'b0;
            case (k)
            //         полный растр                HS           VS           картинка                     режим
            0: begin src_ht = 896; src_vt = 640; src_hs = 104; src_vs = 6; src_hoff = 132; src_voff = 45; exp_mode = 1; end
            1: begin src_ht = 896; src_vt = 624; src_hs = 104; src_vs = 6; src_hoff = 132; src_voff = 29; exp_mode = 2; end
            2: begin src_ht = 896; src_vt = 640; src_hs = 106; src_vs = 6; src_hoff = 140; src_voff = 45; exp_mode = 3; end
            3: begin src_ht = 896; src_vt = 624; src_hs = 106; src_vs = 6; src_hoff = 140; src_voff = 45; exp_mode = 4; end
            4: begin src_ht = 912; src_vt = 622; src_hs = 106; src_vs = 6; src_hoff = 140; src_voff = 45; exp_mode = 5; end
            5: begin src_ht = 100; src_vt = 44;  src_hs = 8;   src_vs = 2; src_hoff = 20;  src_voff = 6;  exp_mode = 6; end
            6: begin src_ht = 104; src_vt = 42;  src_hs = 10;  src_vs = 2; src_hoff = 24;  src_voff = 5;  exp_mode = 7;
                     src_hs_pos = 1'b1; src_vs_pos = 1'b1; end
            default: begin
                // Незнакомый растр: прошивка берёт окно по центру
                src_ht = SRC_H_TOTAL; src_vt = SRC_V_TOTAL; src_hs = 8; src_vs = 2;
                src_hoff = (SRC_H_TOTAL - H_ACTIVE) / 2 + 1;
                src_voff = (SRC_V_TOTAL - V_ACTIVE) / 2;
                exp_mode = 0;
            end
            endcase
            src_hoff = src_hoff + SHIFT_X;
            src_voff = src_voff + SHIFT_Y;
            raster_now = k;
            if (src_vt < src_voff + V_ACTIVE + 1 || src_ht < src_hoff + H_ACTIVE)
                $fatal(1, "входной растр меньше активного окна: тест не имеет смысла");
        end
    endtask
    initial set_raster(RASTER);

    // ------------------------------------------------------------------
    // Источник: картинка зависит от номера кадра
    // ------------------------------------------------------------------
    integer hx = 0, hy = 0, fn = 0;

    // Картинка вместо узора (ключ +img=)
    reg [15:0] img [0:720*576-1];
    reg        use_img = 1'b0;
    reg [8*256-1:0] img_file;
    initial if ($value$plusargs("img=%s", img_file)) begin
        if (MINI) $fatal(1, "+img работает только с MINI=0");
        $readmemh(img_file, img);
        use_img = 1'b1;
    end
    // Маркер кадра на картинке: квадрат 24×24 внизу по бордюру, сдвигается на 16 точек за кадр
    function automatic bit marker(input integer x, input integer y, input integer f);
        integer mx;
        begin
            mx = 16 + (f * 16) % 656;
            marker = (y >= 540) && (y < 564) && (x >= mx) && (x < mx + 24);
        end
    endfunction
    function automatic [15:0] img_px(input integer x, input integer y, input integer f);
        if (marker(x, y, f)) img_px = (f % 2) ? 16'h7FFF : {1'b0, 5'd0, 5'd24, 5'd24};  // белый / жёлтый
        else                 img_px = img[y * 720 + x];
    endfunction

    // Номер кадра узора: AVG_TEST — верхняя четверть меняется каждый кадр, вторая стоит
    // (точки пары одинаковы — без смешения), нижняя половина — кадры A/B через один
    function automatic integer fe(input integer y, input integer f);
        if (!AVG_TEST || y < V_ACTIVE / 4) fe = f;
        else if (y < V_ACTIVE / 2)         fe = 0;
        else                               fe = f % 2;
    endfunction
    function automatic [4:0] p_r(input integer x, input integer y, input integer f);
        if (y == 0 && x < 8) p_r = f[4:0];
        else if (use_img)    p_r = img_px(x, y, f) >> 0;
        else                 p_r = (x/4 + fe(y, f)) % 32;
    endfunction
    function automatic [4:0] p_g(input integer x, input integer y, input integer f);
        if (y == 0 && x < 8) p_g = f[9:5];
        else if (use_img)    p_g = img_px(x, y, f) >> 5;
        else                 p_g = (y/4 + 3*fe(y, f)) % 32;
    endfunction
    function automatic [4:0] p_b(input integer x, input integer y, input integer f);
        if (y == 0 && x < 8) p_b = ~f[4:0];
        else if (use_img)    p_b = img_px(x, y, f) >> 10;
        else                 p_b = ((x + y)/8 + 5*fe(y, f)) % 32;
    endfunction
    function automatic p_p(input integer x, input integer y, input integer f);
        reg [15:0] w;
        begin
            if (y == 0 && x < 8) p_p = 1'b1;          // PAL_SEL=1: расширение обратимо
            else if (use_img) begin w = img_px(x, y, f); p_p = w[15]; end
            else              p_p = ((x/16 + y/16 + fe(y, f)) % 2);
        end
    endfunction
    function automatic [15:0] px16(input integer x, input integer y, input integer f);
        px16 = {p_p(x, y, f), p_b(x, y, f), p_g(x, y, f), p_r(x, y, f)};
    endfunction

    wire in_active = (hx >= src_hoff) && (hx < src_hoff + H_ACTIVE) &&
                     (hy >= src_voff) && (hy < src_voff + V_ACTIVE);
    wire [4:0] s_r = in_active ? p_r(hx - src_hoff, hy - src_voff, fn) : 5'd0;
    wire [4:0] s_g = in_active ? p_g(hx - src_hoff, hy - src_voff, fn) : 5'd0;
    wire [4:0] s_b = in_active ? p_b(hx - src_hoff, hy - src_voff, fn) : 5'd0;
    wire       s_p = in_active ? p_p(hx - src_hoff, hy - src_voff, fn) : 1'b0;
    wire [7:0] phase_a = {s_g[2:0], s_r[4:0]};
    wire [7:0] phase_b = {s_g[3], s_p, s_g[4], s_b};
    wire       mux_sel;
    wire [7:0] mux_d;
    assign #4.5 mux_d = mux_sel ? phase_b : phase_a;

    // Время окончания каждого входного кадра — для оценки задержки
    real t_src_done [0:4095];
    real t_switch = 0.0;
    always @(posedge src_clk) begin
        if (hx == src_ht - 1) begin
            hx <= 0;
            if (hy == src_voff + V_ACTIVE - 1) t_src_done[fn % 4096] = $realtime;
            if (hy == src_vt - 1) begin
                hy <= 0;
                fn <= fn + 1;
                if (SWITCH_TO >= 0 && fn == SWITCH_AT) begin
                    set_raster(SWITCH_TO);
                    t_switch = $realtime;
                    $display("[%0t] растр источника сменён на %0d после входного кадра %0d",
                             $time, SWITCH_TO, fn);
                end
            end else hy <= hy + 1;
        end else hx <= hx + 1;
    end
    wire hsync_in = (hx < src_hs) ^ !src_hs_pos;
    wire vsync_in = (hy < src_vs) ^ !src_vs_pos;

    // Остановка PCLK: Evo выключили или переключили режим
    always @(posedge src_clk)
        if (STOP_AFTER != 0 && fn == STOP_AFTER && hy == src_vt / 2 && pclk_run) begin
            pclk_run = 1'b0;
            $display("[%0t] PCLK остановлен посреди входного кадра %0d", $time, fn);
        end

    // ------------------------------------------------------------------
    // Тракт и SDRAM
    // ------------------------------------------------------------------
    wire        sd_cs_n, sd_ras_n, sd_cas_n, sd_we_n, sd_dq_oe;
    wire [10:0] sd_a;
    wire [1:0]  sd_ba;
    wire [3:0]  sd_dqm;
    wire [31:0] sd_dq_out, sd_dq_in;
    wire [7:0]  vid_r, vid_g, vid_b;
    wire        vid_de, vid_hs, vid_vs;
    wire [9:0]  t_blue, t_green, t_red;
    wire        cap_overflow, fetch_late, have_frame, mem_ready;
    wire [15:0] frames_written, frames_rejected;

    wire        raster_locked, aligned_h, aligned_v, bb_valid;
    wire [10:0] bb_x0, bb_x1, bb_y0, bb_y1;
    wire [2:0]  raster_mode;
    wire [11:0] raster_line_cycles, raster_hs_cycles;
    wire [10:0] raster_lines;

    wire [15:0] chk_err;
    wire        chk_tick, pattern_on;

    // LINK: ведущий QSPI, как у ESP (режим 0, команда по IO0, адрес и данные по 4 линиям)
    reg        lk_sck = 1'b0, lk_cs_n = 1'b1, lk_moe = 1'b0;
    reg  [3:0] lk_mo = 4'hF;
    wire [3:0] lk_so;
    wire       lk_soe;
    wire [3:0] lk_io = lk_moe ? lk_mo : (lk_soe ? lk_so : 4'hF);   // подтяжки линий

    // Flash Tang: чистая (настройки не найдены) — тракт работает на DEF_*
    wire fl_sck, fl_cs_n, fl_mosi, fl_miso;
    spi_flash_model flash (.sck(fl_sck), .cs_n(fl_cs_n), .mosi(fl_mosi), .miso(fl_miso));
    // AVG_TEST: в ячейке 0 — настройки, как DEF_* стенда, и FRAME BLEND (бит 19 слова 1),
    // как их пишет ESP (tang_flash.cpp); ядро берёт их сразу после сброса
    initial if (AVG_TEST) begin : avg_rec
        reg [31:0] w [0:7];
        reg [31:0] sum;
        integer i, b;
        w[0] = 32'h5644_5354;
        w[1] = {12'd0, 1'b1, 2'(SIZE), ASPECT, 8'h01, AUTO_ALIGN, 1'b1, 2'd0, 4'd0};  // фаза 1, задержка 1
        for (i = 2; i < 7; i = i + 1) w[i] = 32'd0;
        sum = 32'd0;
        for (i = 0; i < 7; i = i + 1) sum = sum + w[i];
        w[7] = ~sum;
        #1;                                      // после начального стирания модели
        for (i = 0; i < 8; i = i + 1)
            for (b = 0; b < 4; b = b + 1) flash.mem_set[i * 4 + b] = w[i][31 - b * 8 -: 8];
    end
    // microSD Tang (мост fb_sdspi, ядро с Evo)
    wire tf_cs_n, tf_sck, tf_mosi, tf_miso;
    sd_card_model #(.BLOCKS(8), .NAC(4), .BUSY(4)) tfcard (
        .cs_n(tf_cs_n), .sck(tf_sck), .mosi(tf_mosi), .miso(tf_miso)
    );

    // SPI Evo → FT812 (только для SWITCH_TEST): обмен — выбор FT_CS_N и 24 такта SCK 7 МГц
    reg ft_cs_tb = 1'b1, ft_sck_tb = 1'b0, ft_mosi_tb = 1'b0;
    reg sw_done = 1'b0;                  // проверка автоперехода закончена
    // посылка FT812: n байтов fx[] старшим битом вперёд (MOSI меняется при SCK = 0, режим 0),
    // полупериод SCK hp нс
    reg [7:0] fx [0:7];
    // SCK 28 МГц, как у Evo, — при выводе 720p (такт пикселей 74,25 МГц видит каждый уровень);
    // при 27 МГц (576p) детектор такой SCK не различает — 7 МГц
    localparam real FT_HP = HD_OUT ? 17.8 : 71.0;
    task automatic ft_xact(input integer n, input real hp);
        integer i, b;
        begin
            ft_cs_tb = 1'b0;
            #(100);
            for (i = 0; i < n; i = i + 1)
                for (b = 7; b >= 0; b = b - 1) begin
                    ft_mosi_tb = fx[i][b];
                    #(hp);
                    ft_sck_tb = 1'b1; #(hp);
                    ft_sck_tb = 1'b0;
                end
            ft_mosi_tb = 1'b0;
            #(100);
            ft_cs_tb = 1'b1;
            #(300);
        end
    endtask
    task automatic ft_burst;
        integer i;
        begin
            ft_cs_tb = 1'b0;
            #(100);
            for (i = 0; i < 24; i = i + 1) begin
                ft_sck_tb = 1'b1; #(71);
                ft_sck_tb = 1'b0; #(71);
            end
            #(100);
            ft_cs_tb = 1'b1;
        end
    endtask

    ts_fb_core #(
        .TEST_PROFILES(1'b1),
        .H_ACTIVE(H_ACTIVE), .V_ACTIVE(V_ACTIVE),
        .OUT_W(OUT_W), .OUT_H(OUT_H), .IMG_X(IMG_X), .IMG_Y(IMG_Y),
        .H_FP(H_FP), .H_SYNC(H_SYNC), .H_BP(H_BP),
        .V_FP(V_FP), .V_SYNC(V_SYNC), .V_BP(V_BP),
        .HS_NEG(SYNC_NEG), .VS_NEG(SYNC_NEG),
        .PIX_KHZ(HD_OUT ? 74_250 : 27_000), .MEM_KHZ(HD_OUT ? 61_875 : 67_500),
        .SDRAM_INIT_US(MINI ? 20 : 200),
        .NOSIG_CYCLES(MINI ? 2_000 : 200_000),
        .PATTERN_PERIOD(MINI ? 3_000 : (HD_OUT ? 1_237_500 : 1_350_000)),
        .DEF_SRC(PATTERN ? (HAS_EVO ? 2'd0 : 2'd2) : 2'd1),  // с Evo — EVO, без Evo — AUTO (FT812 — TEST)
        .DEF_RD(RD_FIXED[1:0]),
        .DEF_ALIGN(AUTO_ALIGN),
        .CALIBRATE(CAL),
        .SCALE(SCALED), .HDMI(SCALED), .DEF_ASPECT(ASPECT), .DEF_SIZE(SIZE[1:0]),
        .DEF_AVG(1'b0),                   // AVG_TEST включает FRAME BLEND ячейкой во flash
        .FLASH_BOOT_US(20),
        .HAS_EVO(HAS_EVO), .HAS_FT(HAS_FT), .SWITCH_IDLE_MS(SWITCH_MS), .SD_FAST(SDF)
    ) dut (
        .clk_cap(clk_cap), .rst_cap_n(rst_n),
        .clk_mem(clk_mem), .rst_mem_n(rst_n),
        .pixel_clk(pixel_clk), .rst_pix_n(rst_n), .blank(1'b0), .btn(2'b00),
        .clk_sd(clk_sd), .rst_sd_n(rst_n),
        .link_sck(lk_sck), .link_cs_n(lk_cs_n),
        .link_io_in(lk_io), .link_io_out(lk_so), .link_io_oe(lk_soe),
        .ft_sck(ft_sck_tb), .ft_cs_n(ft_cs_tb), .ft_mosi(ft_mosi_tb), .ft_miso(), .ft_int_n(),
        .cp_sck(1'b0), .cp_cs_n(1'b1), .cp_mosi(1'b0), .cp_miso(), .cp_irq_n(),
        .flash_sck(fl_sck), .flash_cs_n(fl_cs_n), .flash_mosi(fl_mosi), .flash_miso(fl_miso),
        .sd_cs_n(tf_cs_n), .sd_sck(tf_sck), .sd_mosi(tf_mosi), .sd_miso(tf_miso),
        .mux_d(mux_d), .hsync_in(hsync_in), .vsync_in(vsync_in), .mux_sel(mux_sel),
        .sdram_cs_n(sd_cs_n), .sdram_ras_n(sd_ras_n), .sdram_cas_n(sd_cas_n),
        .sdram_we_n(sd_we_n), .sdram_a(sd_a), .sdram_ba(sd_ba), .sdram_dqm(sd_dqm),
        .sdram_dq_out(sd_dq_out), .sdram_dq_oe(sd_dq_oe), .sdram_dq_in(sd_dq_in), .sdram_dly(), .sdram_dly_df(1'b0),
        .vid_r(vid_r), .vid_g(vid_g), .vid_b(vid_b),
        .vid_de(vid_de), .vid_hs(vid_hs), .vid_vs(vid_vs),
        .tmds_blue(t_blue), .tmds_green(t_green), .tmds_red(t_red),
        .cap_overflow(cap_overflow), .frames_written(frames_written),
        .frames_rejected(frames_rejected), .fetch_late(fetch_late),
        .have_frame(have_frame), .mem_ready(mem_ready),
        .raster_locked(raster_locked), .raster_mode(raster_mode),
        .raster_line_cycles(raster_line_cycles), .raster_lines(raster_lines),
        .raster_hs_cycles(raster_hs_cycles),
        .aligned_h(aligned_h), .aligned_v(aligned_v),
        .bb_valid(bb_valid), .bb_x0(bb_x0), .bb_x1(bb_x1), .bb_y0(bb_y0), .bb_y1(bb_y1),
        .pattern_on(pattern_on), .chk_err(chk_err), .chk_tick(chk_tick)
    );

    // Автоподстройка окна: с какого входного кадра окно стоит по новому месту.
    // Окно меняется на такт позже строба кадра, а fb_capture защёлкивает его на следующем
    // стробе — значит, по-новому захватывается кадр, идущий после текущего.
    integer align_fn = -1;
    reg [10:0] h_off_prev = 0, v_off_prev = 0;
    always @(posedge clk_cap) begin
        if (dut.h_off !== h_off_prev || dut.v_off !== v_off_prev) begin
            if (raster_locked && (aligned_h || aligned_v)) begin
                align_fn = fn + 1;
                $display("[%0t] окно подстроено по картинке: смещение %0d/%0d, с входного кадра %0d",
                         $time, dut.h_off, dut.v_off, align_fn);
            end
            h_off_prev = dut.h_off;
            v_off_prev = dut.v_off;
        end
    end

    // Момент захвата растра прошивкой
    real t_lock = -1.0;
    always @(posedge clk_cap) begin
        if (raster_locked && t_lock < 0.0) t_lock = $realtime;
        if (!raster_locked) t_lock = -1.0;
    end


    // Клок микросхемы — инверсия клока логики, как в ts_fb_hdmi_top
    sdram_model #(.TCK_NS(SDF ? T_MEM / 2 : T_MEM), .CL(SDF ? 3 : 2)) sdram (
        .clk(SDF ? ~clk_sd : ~clk_mem), .cs_n(sd_cs_n), .ras_n(sd_ras_n), .cas_n(sd_cas_n), .we_n(sd_we_n),
        .a(sd_a), .ba(sd_ba), .dqm(sd_dqm),
        .dq_from_ctrl(sd_dq_out), .dq_oe(sd_dq_oe), .dq_to_ctrl(sd_dq_in)
    );

    // ------------------------------------------------------------------
    // Декодер TMDS — приёмник монитора
    // ------------------------------------------------------------------
    function automatic [10:0] tmds_dec(input [9:0] w);   // {de, c1, c0, d[7:0]}
        reg [7:0] d, q;
        integer k;
        begin
            case (w)
                10'b1101010100: tmds_dec = {1'b0, 2'b00, 8'h00};
                10'b0010101011: tmds_dec = {1'b0, 2'b01, 8'h00};
                10'b0101010100: tmds_dec = {1'b0, 2'b10, 8'h00};
                10'b1010101011: tmds_dec = {1'b0, 2'b11, 8'h00};
                default: begin
                    d = w[9] ? ~w[7:0] : w[7:0];
                    q[0] = d[0];
                    for (k = 1; k < 8; k = k + 1)
                        q[k] = w[8] ? (d[k] ^ d[k-1]) : ~(d[k] ^ d[k-1]);
                    tmds_dec = {1'b1, 2'b00, q};
                end
            endcase
        end
    endfunction

    wire [10:0] dec_b = tmds_dec(t_blue);
    wire [10:0] dec_g = tmds_dec(t_green);
    wire [10:0] dec_r = tmds_dec(t_red);
    // HDMI (720p): защитные полосы и пакет AVI — гашение; в пакете и его защитных полосах
    // HSYNC/VSYNC — младшие биты TERC4 канала 0, в защитной полосе видео — неактивны.
    // Строение потока HDMI проверяет tb_hdmi_tx; здесь символ HDMI узнаётся по hdmi_tx.
    function automatic integer terc4_dec(input [9:0] w);
        case (w)
            10'b1010011100: terc4_dec = 0;  10'b1001100011: terc4_dec = 1;
            10'b1011100100: terc4_dec = 2;  10'b1011100010: terc4_dec = 3;
            10'b0101110001: terc4_dec = 4;  10'b0100011110: terc4_dec = 5;
            10'b0110001110: terc4_dec = 6;  10'b0100111100: terc4_dec = 7;
            10'b1011001100: terc4_dec = 8;  10'b0100111001: terc4_dec = 9;
            10'b0110011100: terc4_dec = 10; 10'b1011000110: terc4_dec = 11;
            10'b1010001110: terc4_dec = 12; 10'b1001110001: terc4_dec = 13;
            10'b0101100011: terc4_dec = 14; 10'b1011000011: terc4_dec = 15;
            default:        terc4_dec = 0;
        endcase
    endfunction
    wire hdmi_sym;
    generate if (SCALED) begin : g_hdmi_sym
        assign hdmi_sym = dut.tmds.g_hdmi.ov;
    end else begin : g_no_hdmi
        assign hdmi_sym = 1'b0;
    end endgenerate
    // Синхроимпульсы передаются только в гашении; в активной части держим последние
    wire        m_de = dec_b[10] && !hdmi_sym;
    reg         m_hs = 1'b1, m_vs = 1'b1;
    always @(*) begin : sync_dec
        integer t;
        if (hdmi_sym) begin
            t = terc4_dec(t_blue);
            m_hs = t[0]; m_vs = t[1];
        end else if (!dec_b[10]) begin
            m_hs = dec_b[8]; m_vs = dec_b[9];        // c0 = HS, c1 = VS
        end
    end
    wire [7:0]  m_r = dec_r[7:0], m_g = dec_g[7:0], m_b = dec_b[7:0];

    // ------------------------------------------------------------------
    // Эталон сглаживания (fb_scanout): точка вывода (qx, qy) — смесь четырёх точек
    // источника по доле площади. Доли — точными дробями: точка вывода занимает
    // [qx·NX/DX, (qx+1)·NX/DX) точек источника (NX/DX = H_ACTIVE/PIC_W несократимо).
    // ------------------------------------------------------------------
    function automatic integer gcd(input integer a, input integer b);
        integer t;
        begin
            while (b != 0) begin t = a % b; a = b; b = t; end
            gcd = a;
        end
    endfunction
    localparam int GX = gcd(H_ACTIVE, PIC_W), GY = gcd(V_ACTIVE, PIC_H);
    localparam int NX = H_ACTIVE / GX, DX = PIC_W / GX, NY = V_ACTIVE / GY, DY = PIC_H / GY;

    // Вес следующей точки, 1/16: (доля за границей) · 16, с округлением через 65536/N
    function automatic integer wgt(input integer acc, input integer n, input integer d);
        integer over;
        begin
            over = acc + n;
            if (over <= d) wgt = 0;
            else begin
                wgt = ((over - d) * ((65536 + n / 2) / n) + 2048) / 4096;
                if (wgt > 16) wgt = 16;
            end
        end
    endfunction

    function automatic [7:0] blend(input integer tl, input integer tr, input integer bl,
                                   input integer br, input integer wx, input integer wy);
        integer top, bot;
        begin
            top = 16 * tl + (tr - tl) * wx;
            bot = 16 * bl + (br - bl) * wx;
            blend = (16 * top + (bot - top) * wy + 128) / 256;
        end
    endfunction

    // ------------------------------------------------------------------
    // Эталон палитры (как legacy_palette)
    // ------------------------------------------------------------------
    function automatic [7:0] expand(input pal, input [4:0] v);
        integer s;
        begin
            if (pal) expand = {v, 3'b000};
            else if (v >= 24) expand = 8'hFF;
            else begin s = v * 255 + 12; expand = s / 24; end
        end
    endfunction

    // ------------------------------------------------------------------
    // FRAME BLEND (AVG_TEST): какой входной кадр в каком буфере — номер из точки 0 строки 0,
    // когда её пакет уходит в SDRAM, — и какая пара взята на выходной кадр (переключение
    // читателя). Значения fb_core — до фронта: те же, по которым решает он сам.
    // ------------------------------------------------------------------
    integer buf_id [0:3];
    localparam int FIRST_CHUNK = (H_ACTIVE / 2 < 64) ? H_ACTIVE / 2 : 64;   // слов в первом запросе писателя
    integer sw_cur = -1, sw_prev = -1;
    reg     sw_have = 1'b0, sw_avg = 1'b0;
    initial for (int b = 0; b < 4; b = b + 1) buf_id[b] = -1;
    generate if (AVG_TEST) begin : g_avg_snoop
        always @(posedge clk_mem) begin
            // первый кусок строки 0 (до 64 слов) уходит в SDRAM: слово 0 — в буфере писателя
            if (dut.fb.wst == 2 && dut.fb.w_y == 0 && dut.fb.w_word == FIRST_CHUNK)
                buf_id[dut.fb.w_buf] = {dut.fb.wbuf[0][9:5], dut.fb.wbuf[0][4:0]};
            if (dut.fb.reader_switch) begin
                sw_have = dut.fb.have_frame;
                if (dut.fb.have_frame) begin
                    sw_cur  = buf_id[dut.fb.latest];
                    sw_avg  = dut.fb.avg_take;
                    sw_prev = dut.fb.avg_take ? buf_id[dut.fb.prev] : sw_cur;
                end
            end
        end
    end endgenerate

    // Самая долгая выборка подряд (запрос строки → строка в буфере): у FRAME BLEND строка
    // читается дважды — запас по времени строки вывода
    real    fetch_t0 = 0.0, fetch_max = 0.0;
    reg     fetch_busy = 1'b0;
    always @(posedge clk_mem) begin
        if (!fetch_busy && (dut.fb.req_pending || dut.fb.rst_q != 0)) begin
            fetch_busy = 1'b1; fetch_t0 = $realtime;
        end else if (fetch_busy && !dut.fb.req_pending && dut.fb.rst_q == 0 && !dut.fb.r_second) begin
            fetch_busy = 1'b0;
            if ($realtime - fetch_t0 > fetch_max) fetch_max = $realtime - fetch_t0;
        end
    end

    // ------------------------------------------------------------------
    // Проверка выхода
    // ------------------------------------------------------------------
    integer errors = 0;
    integer shown_prev = -2;                    // AVG_TEST: второй кадр пары
    reg     shown_avg = 1'b0;                   //   выходной кадр — среднее пары
    integer avg_frames = 0, avg_plain = 0;      //   кадров со смешением / без него после первого
    integer line_clk = 0, hs_width = 0, de_in_line = 0;
    integer lines_in_frame = 0, vs_lines = 0, de_lines = 0;
    integer ox = 0, oy = 0, frame_pix = 0;
    integer out_frames = 0, shown_id = -2, prev_id = -2;
    integer repeats = 0, skips = 0, black_frames = 0, valid_frames = 0;
    integer first_ids_ok = 1;
    integer frame_err = 0;
    reg     prev_hs = 1'b1, prev_vs = 1'b1, prev_de = 1'b0;
    real    t_vs_prev = -1.0, period_min = 1.0e12, period_max = 0.0;
    real    lat_sum = 0.0; integer lat_n = 0;
    integer stop_seen_frames = 0, last_id_before_stop = -2;
    integer ppm = 0;
    // Проверка автоподстройки: до неё картинка сдвинута и кадры не сверяются
    localparam bit SHIFTED = (SHIFT_X != 0) || (SHIFT_Y != 0);
    integer pre_align_frames = 0, aligned_frames = 0;
    reg     dump_ppm = 1'b0;
    initial dump_ppm = $test$plusargs("ppm");

    // Канал c (0 R, 1 G, 2 B) точки источника в выходном кадре: точка кадра shown_id, а при
    // FRAME BLEND, если в кадре shown_prev она другая, — среднее пары (fb_avg_q, PAL_SEL = 1)
    reg [4:0] avg_tab [0:4095];             // fb_avg_q массивом: case на каждую точку — медленно
    initial for (int i = 0; i < 4096; i = i + 1) avg_tab[i] = fb_avg_q(12'(i));
    function automatic [7:0] src8(input integer x, input integer y, input integer c);
        reg [15:0] l, p;
        begin
            l = px16(x, y, shown_id);
            p = px16(x, y, shown_prev);
            if (!shown_avg || l == p) src8 = expand(l[15], l[c*5 +: 5]);
            else src8 = {avg_tab[{l[15], l[c*5 +: 5], p[15], p[c*5 +: 5]}], 3'b000};
        end
    endfunction

    task automatic err(input string msg);
        begin
            if (errors < 25) $display("[%0t] ОШИБКА: %0s", $time, msg);
            errors = errors + 1;
        end
    endtask

    // Оверлеи ESP (OVL_TEST): 0 — холст 32×12 в (8, 4), ×1, непрозрачный; 1 — холст 16×8
    // в (24, 10), ×2, прозрачность 7, поверх 0. Точка холста — узор от координат.
    reg     ovl_live = 1'b0, ovl_chk = 1'b0;
    integer ovl_px_n = 0;
    function automatic integer OV_X(input integer k); OV_X = k ? 24 : 8;   endfunction
    function automatic integer OV_Y(input integer k); OV_Y = k ? 10 : 4;   endfunction
    function automatic integer OV_W(input integer k); OV_W = k ? 16 : 32;  endfunction
    function automatic integer OV_H(input integer k); OV_H = k ? 8 : 12;   endfunction
    function automatic integer OV_S(input integer k); OV_S = k ? 2 : 1;    endfunction
    function automatic integer OV_A(input integer k); OV_A = k ? 7 : 15;   endfunction
    function automatic integer OV_B(input integer k); OV_B = k ? 256 : 0;  endfunction  // слово региона
    function automatic [15:0] ovl_canvas(input integer k, input integer cx, input integer cy);
        reg [4:0] r; reg [5:0] g; reg [4:0] b;
        begin
            r = k ? 5'(cy * 3 + 7) : 5'(cx);
            g = k ? 6'(cx * 5) : 6'(cy * 4 + 1);
            b = k ? 5'(cx ^ cy) : 5'(cx + cy);
            ovl_canvas = {r, g, b};
        end
    endfunction
    function automatic bit ovl_in(input integer k, input integer x, input integer y);
        ovl_in = x >= OV_X(k) && x < OV_X(k) + OV_W(k) * OV_S(k) &&
                 y >= OV_Y(k) && y < OV_Y(k) + OV_H(k) * OV_S(k);
    endfunction
    // v + ⌊(c − v)·(a + 1)/16⌋ (сдвиг целого со знаком — округление вниз)
    function automatic [7:0] ovl_mix(input [7:0] v, input [7:0] c, input integer a);
        integer t, s;
        begin
            t = ($signed({1'b0, c}) - $signed({1'b0, v})) * (a + 1);
            s = t >>> 4;
            ovl_mix = v + s[7:0];
        end
    endfunction
    task automatic ovl_expect(input integer x, input integer y, inout [7:0] r, inout [7:0] g,
                              inout [7:0] b);
        integer k;
        reg [15:0] c;
        begin
            for (k = 0; k < 2; k = k + 1) if (ovl_in(k, x, y)) begin
                c = ovl_canvas(k, (x - OV_X(k)) / OV_S(k), (y - OV_Y(k)) / OV_S(k));
                r = ovl_mix(r, {c[15:11], c[15:13]}, OV_A(k));
                g = ovl_mix(g, {c[10:5], c[10:9]}, OV_A(k));
                b = ovl_mix(b, {c[4:0], c[4:2]}, OV_A(k));
            end
        end
    endtask

    // Испытательная таблица (PATTERN): сверка кадров после калибровки
    integer pat_frames = 0, pat_errs = 0, pat_frame_err = 0, cal_frames = 0;
    reg     pat_check = 1'b0;
    reg [15:0] pat_exp;

    // Кадр закончен (фронт VSYNC): итоги кадра
    task automatic end_of_frame;
        begin
            if (PATTERN) begin
                if (pat_check) begin
                    pat_frames = pat_frames + 1;
                    if (pat_frame_err != 0) begin
                        err($sformatf("кадр таблицы: %0d расхождений", pat_frame_err));
                        pat_errs = pat_errs + pat_frame_err;
                    end
                    if (ppm != 0) begin $fclose(ppm); ppm = 0; end
                end
                // Сверяются кадры со второго после конца калибровки и при показе таблицы
                if ((!CAL || dut.ui.cal_done) && pattern_on && dut.have_frame) cal_frames = cal_frames + 1;
                pat_check = cal_frames >= 2;
                ovl_chk   = ovl_live;
                pat_frame_err = 0;
                if (dump_ppm && cal_frames >= 2) begin
                    if (ppm != 0) $fclose(ppm);
                    ppm = $fopen($sformatf("pat_out_%02d.ppm", out_frames), "wb");
                    $fwrite(ppm, "P6\n%0d %0d\n255\n", OUT_W, OUT_H);
                end
            end
            if (out_frames >= 1 && PATTERN) begin
                if (frame_pix != OUT_W * OUT_H)
                    err("число активных пикселей в кадре неверно");
                if (lines_in_frame != V_TOTAL) err("число строк в кадре не равно V_TOTAL");
                if (de_lines != OUT_H) err("число активных строк неверно");
                if (vs_lines != V_SYNC) err("ширина кадрового импульса неверна");
            end else if (out_frames >= 1) begin
                if (frame_pix != OUT_W * OUT_H)
                    err("число активных пикселей в кадре неверно");
                if (lines_in_frame != V_TOTAL) err("число строк в кадре не равно V_TOTAL");
                if (de_lines != OUT_H) err("число активных строк неверно");
                if (vs_lines != V_SYNC) err("ширина кадрового импульса неверна");
                if (shown_id == -1) black_frames = black_frames + 1;
                else if (shown_id == -4 || (SHIFTED && shown_id >= 0 && (align_fn < 0 || shown_id < align_fn)))
                    pre_align_frames = pre_align_frames + 1;
                else if (shown_id >= 0) begin
                    valid_frames = valid_frames + 1;
                    if (SHIFTED) aligned_frames = aligned_frames + 1;
                    if (frame_err != 0) err("кадр собран не из одного входного кадра");
                    if (prev_id >= 0) begin
                        if (shown_id < prev_id) err("номер кадра уменьшился");
                        else if (shown_id == prev_id) repeats = repeats + 1;
                        else if (shown_id > prev_id + 1) skips = skips + (shown_id - prev_id - 1);
                    end
                    if (prev_id == -1 && black_frames == 0) err("чёрный кадр не перед первым");
                    prev_id = shown_id;
                end
                if (shown_id >= 0 && prev_id == -1) ;
            end
            if (ppm != 0 && !PATTERN) begin $fclose(ppm); ppm = 0; end
            out_frames = out_frames + 1;
            frame_pix = 0; lines_in_frame = 0; vs_lines = 0; de_lines = 0;
            oy = 0; frame_err = 0; shown_id = -2;
        end
    endtask

    // Синхроимпульс активен: у 576p — низкий уровень, у 720p — высокий
    wire hs_on = SYNC_NEG ? !m_hs : m_hs;
    wire vs_on = SYNC_NEG ? !m_vs : m_vs;
    reg  prev_hs_on = 1'b0, prev_vs_on = 1'b0;
    integer ix = 0, iy = 0, qx = 0, qy = 0;
    reg     inimg = 1'b0, first = 1'b0;

    always @(posedge pixel_clk) if (rst_n && mem_ready) begin
        line_clk = line_clk + 1;
        if (hs_on) hs_width = hs_width + 1;
        // Начало строки — по фронту активного уровня HSYNC
        if (hs_on && !prev_hs_on) begin
            if (out_frames >= 2) begin
                if (line_clk != H_TOTAL) err("длина строки не равна H_TOTAL");
            end
            line_clk = 0;
            lines_in_frame = lines_in_frame + 1;
            if (vs_on) vs_lines = vs_lines + 1;
        end
        if (!hs_on && prev_hs_on) begin
            if (out_frames >= 2 && hs_width != H_SYNC) err("ширина строчного импульса неверна");
            hs_width = 0;
        end
        // Кадр — по фронту активного уровня VSYNC
        if (vs_on && !prev_vs_on) begin
            if (t_vs_prev > 0.0 && out_frames >= 2) begin
                if ($realtime - t_vs_prev < period_min) period_min = $realtime - t_vs_prev;
                if ($realtime - t_vs_prev > period_max) period_max = $realtime - t_vs_prev;
            end
            t_vs_prev = $realtime;
            end_of_frame();
            if (!pclk_run) stop_seen_frames = stop_seen_frames + 1;
        end
        // Координаты в картинке; вне её — чёрное поле
        if (m_de) begin
            qx = (prev_de ? ox + 1 : 0) - PIC_X;
            qy = oy - PIC_Y;
            inimg = (qx >= 0) && (qx < PIC_W) && (qy >= 0) && (qy < PIC_H);
            ix = inimg ? qx * H_ACTIVE / PIC_W : -1;
            iy = inimg ? qy * V_ACTIVE / PIC_H : -1;
            first = inimg && qx == 0 && qy == 0;
            if (!inimg && (m_r !== 0 || m_g !== 0 || m_b !== 0) && out_frames >= 2) begin
                if (errors < 25) $display("[%0t] поле вне картинки x=%0d y=%0d: %02x %02x %02x",
                                          $time, qx + PIC_X, oy, m_r, m_g, m_b);
                err("поле вокруг картинки не чёрное");
            end
        end
        // Активные пиксели: испытательная таблица
        if (PATTERN && m_de) begin
            if (!prev_de) begin ox = 0; de_lines = de_lines + 1; end
            else ox = ox + 1;
            frame_pix = frame_pix + 1;
            if (ppm != 0) $fwrite(ppm, "%c%c%c", m_r, m_g, m_b);
            if (pat_check && inimg && !(iy >= FB_PAT_BAND_Y0 && iy < FB_PAT_BAND_Y1)) begin : pchk
                reg [7:0] er, eg, eb;
                pat_exp = fb_pattern_px(ix, iy, 8'd0, H_ACTIVE, V_ACTIVE);
                er = expand(pat_exp[15], pat_exp[4:0]);
                eg = expand(pat_exp[15], pat_exp[9:5]);
                eb = expand(pat_exp[15], pat_exp[14:10]);
                if (ovl_chk) ovl_expect(ox, oy, er, eg, eb);
                if (ovl_chk && ovl_in(0, ox, oy)) ovl_px_n = ovl_px_n + 1;
                if (m_r !== er || m_g !== eg || m_b !== eb) begin
                    if (pat_frame_err < 3)
                        $display("[%0t] таблица x=%0d y=%0d: %02x %02x %02x, ожидалось %02x %02x %02x",
                                 $time, ix, iy, m_r, m_g, m_b, er, eg, eb);
                    pat_frame_err = pat_frame_err + 1;
                end
            end
        end else if (PATTERN) begin
            if (prev_de) oy = oy + 1;
        end else
        // Активные пиксели: кадры Evo (сверяется только картинка, поле проверено выше)
        if (m_de) begin
            if (!prev_de) begin ox = 0; de_lines = de_lines + 1; end
            else ox = ox + 1;
            frame_pix = frame_pix + 1;
            if (ppm != 0 && inimg && !first) $fwrite(ppm, "%c%c%c", m_r, m_g, m_b);
            if (first && AVG_TEST) begin
                // Пара — из fb_core; без смешения номер в точке должен совпасть с буфером
                shown_avg  = sw_have && sw_avg;
                shown_prev = sw_prev;
                if (!sw_have) shown_id = -1;
                else begin
                    shown_id = sw_cur;
                    if (!shown_avg && !(m_b[7:3] == ~m_r[7:3] && {m_g[7:3], m_r[7:3]} == sw_cur[9:0]))
                        err($sformatf("FRAME BLEND: в точке не номер кадра %0d буфера", sw_cur));
                    if (shown_avg) avg_frames = avg_frames + 1;
                    else if (avg_frames != 0) avg_plain = avg_plain + 1;
                end
            end else if (first) begin
                // Номер кадра из строки 0: R=f[4:0], G=f[9:5], B=~f[4:0], PAL=1
                if (m_r == 0 && m_g == 0 && m_b == 0) shown_id = -1;
                else if (m_b[7:3] == ~m_r[7:3]) begin
                    shown_id = {m_g[7:3], m_r[7:3]};
                    if (dump_ppm) begin
                        ppm = $fopen($sformatf("fb_out_%02d.ppm", shown_id), "wb");
                        $fwrite(ppm, "P6\n%0d %0d\n255\n", PIC_W, PIC_H);
                    end
                    // Задержка: от конца входного кадра до начала его показа
                    lat_sum = lat_sum + ($realtime - t_src_done[shown_id % 4096]);
                    lat_n = lat_n + 1;
                end else if (SHIFTED) begin
                    shown_id = -4;          // окно ещё не подстроено: номер кадра не на месте
                end else begin
                    err("не удалось прочитать номер кадра");
                    shown_id = -3;
                end
            end
            if (ppm != 0 && first) $fwrite(ppm, "%c%c%c", m_r, m_g, m_b);
            if (!inimg) begin
                // поле: проверено выше
            end else if (shown_id == -1) begin
                if (m_r != 0 || m_g != 0 || m_b != 0) frame_err = frame_err + 1;
            end else if (shown_id >= 0) begin : expected
                integer x2, y2, wx, wy;
                reg [7:0] er, eg, eb;
                x2 = (ix + 1 < H_ACTIVE) ? ix + 1 : ix;
                y2 = (iy + 1 < V_ACTIVE) ? iy + 1 : iy;
                wx = wgt((qx * NX) % DX, NX, DX);
                wy = wgt((qy * NY) % DY, NY, DY);
                er = blend(src8(ix, iy, 0), src8(x2, iy, 0), src8(ix, y2, 0), src8(x2, y2, 0), wx, wy);
                eg = blend(src8(ix, iy, 1), src8(x2, iy, 1), src8(ix, y2, 1), src8(x2, y2, 1), wx, wy);
                eb = blend(src8(ix, iy, 2), src8(x2, iy, 2), src8(ix, y2, 2), src8(x2, y2, 2), wx, wy);
                if (m_r !== er || m_g !== eg || m_b !== eb) begin
                    if (frame_err < 3)
                        $display("[%0t] пиксель x=%0d y=%0d кадра %0d (веса %0d/%0d): %02x %02x %02x, ожидалось %02x %02x %02x",
                                 $time, ix, iy, shown_id, wx, wy, m_r, m_g, m_b, er, eg, eb);
                    frame_err = frame_err + 1;
                end
            end
        end else if (prev_de) begin
            oy = oy + 1;
        end
        prev_hs = m_hs; prev_vs = m_vs; prev_de = m_de;
        prev_hs_on = hs_on; prev_vs_on = vs_on;
    end

    // ------------------------------------------------------------------
    // LINK: транзакции ведущего (как драйвер ESP) и проверка регистров
    // ------------------------------------------------------------------
    localparam int LK_DUMMY = 8;
    reg [31:0] lk_words [0:15];
    integer    link_checks = 0;
    reg        link_done = 1'b0;

    task automatic lk_clock;
        begin #(LINK_NS / 2.0) lk_sck = 1'b1; #(LINK_NS / 2.0) lk_sck = 1'b0; end
    endtask

    // cmd 0x01 — запись nw слов из lk_words, 0x02 — чтение nw слов в lk_words
    task automatic link_xfer(input [7:0] cmd, input [31:0] addr, input integer nw);
        integer i, w;
        reg [31:0] v;
        begin
            lk_cs_n = 1'b0; lk_moe = 1'b1;
            #(LINK_NS);
            for (i = 7; i >= 0; i = i - 1) begin lk_mo = {3'b111, cmd[i]}; lk_clock(); end
            for (i = 7; i >= 0; i = i - 1) begin lk_mo = addr[i*4 +: 4]; lk_clock(); end
            if (cmd == 8'h01) begin
                for (w = 0; w < nw; w = w + 1)
                    for (i = 7; i >= 0; i = i - 1) begin lk_mo = lk_words[w][i*4 +: 4]; lk_clock(); end
            end else begin
                lk_moe = 1'b0;
                for (i = 0; i < LK_DUMMY; i = i + 1) lk_clock();
                for (w = 0; w < nw; w = w + 1) begin
                    v = 'x;
                    for (i = 0; i < 8; i = i + 1) begin
                        #(LINK_NS / 2.0) v = {v[27:0], lk_io}; lk_sck = 1'b1;
                        #(LINK_NS / 2.0) lk_sck = 1'b0;
                    end
                    lk_words[w] = v;
                end
            end
            #(LINK_NS / 2.0);
            lk_cs_n = 1'b1; lk_moe = 1'b0; lk_mo = 4'hF;
            #(LINK_NS * 2);
        end
    endtask

    task automatic link_expect(input string what, input [31:0] got, input [31:0] want);
        begin
            link_checks = link_checks + 1;
            if (got !== want) err($sformatf("LINK %0s: %08h, ожидалось %08h", what, got, want));
        end
    endtask

    // мост к microSD: операция SD_CMD и ожидание по SD_STAT (как ESP)
    reg [31:0] sd_st;
    task automatic sd_op(input integer o, input integer n);
        integer t;
        begin
            lk_words[0] = {9'd0, o[2:0], n[19:0]};
            link_xfer(8'h01, 32'h0000_0108, 1);
            t = 0;
            do begin
                link_xfer(8'h02, 32'h0000_0108, 1);
                t = t + 1;
            end while (lk_words[0][0] && t < 4000);
            sd_st = lk_words[0];
            if (t >= 4000) err("мост к microSD не закончил операцию");
        end
    endtask

    // окно FT812: ждать, пока FT_STAT 31 (окно занято) не снимется
    task automatic ft_wait_idle(input string what);
        integer n;
        begin
            n = 0;
            do begin
                link_xfer(8'h02, 32'h0000_005C, 1);
                n = n + 1;
            end while ((lk_words[0] & 32'h8000_0000) != 0 && n < 4000);
            if (n >= 4000) err($sformatf("окно FT812 не освободилось: %0s", what));
        end
    endtask

    // окно FT812: слово памяти по адресу — FT_ADDR, ожидание, FT_DATA
    task automatic ft_read_expect(input string what, input [31:0] addr, input [31:0] want);
        begin
            lk_words[0] = addr;
            link_xfer(8'h01, 32'h0000_0054, 1);
            ft_wait_idle(what);
            link_xfer(8'h02, 32'h0000_0058, 1);
            link_expect(what, lk_words[0], want);
        end
    endtask

    // Ядро FT812 (SWITCH_TEST): пока Evo обменивается с FT812, ядро остаётся; SWITCH_MS
    // тишины при M_SEL = 0 — запрос перезагрузки в слот TS-Config
    initial if (SWITCH_TEST && !HAS_EVO && HAS_FT) begin : sw_ft
        integer i;
        wait (rst_n);
        #(1000);
        // хост начал обмен ещё в слоте TS-Config (его ACTIVE туда и ушёл): ядро — сразу ACTIVE
        if (dut.g_ft.ft.pmode !== 2'd0) err("ядро FT812 без Evo: после загрузки не ACTIVE");
        for (i = 0; i < 8; i = i + 1) begin           // 2 мс обмена — дольше SWITCH_MS
            ft_burst;
            #(250_000);
        end
        if (dut.boot_req !== 1'b0) err("автовозврат: перезагрузка во время обмена с FT812");
        #(SWITCH_MS * 1_000_000 + 1_000_000);
        if (dut.boot_req !== 1'b1) err("автовозврат: нет перезагрузки после тишины");
        else $display("автовозврат: %0d мс без обмена с FT812 при M_SEL = 0 — перезагрузка в слот TS-Config", SWITCH_MS);
        sw_done = 1'b1;
    end

    initial if (LINK_TEST) begin
        wait (rst_n && dut.mem_ready);
        if (CAL) wait (dut.ui.cal_done);
        repeat (3) @(posedge chk_tick);
        // ID и версия одним чтением двух слов
        link_xfer(8'h02, 32'h0000_0000, 2);
        link_expect("ID", lk_words[0], 32'h5644_4334);
        link_expect("VERSION", lk_words[1], {HAS_EVO ? 8'h01 : 8'h02, 8'h01, 16'h0120});
        // Оверлеи ESP: холсты — окном 0x80000…, по 16 слов за транзакцию, затем регистры
        if (OVL_ON) begin : ovl
            integer k, cy, w, n, words;
            for (k = 0; k < 2; k = k + 1) begin
                words = ((OV_W(k) + 15) / 16) * 8;
                for (cy = 0; cy < OV_H(k); cy = cy + 1)
                    for (w = 0; w < words; w = w + 16) begin
                        for (n = 0; n < 16; n = n + 1)
                            lk_words[n] = {ovl_canvas(k, 2 * (w + n) + 1, cy), ovl_canvas(k, 2 * (w + n), cy)};
                        link_xfer(8'h01, 32'h0008_0000 + 4 * (OV_B(k) + cy * words + w), 16);
                    end
            end
            for (k = 0; k < 2; k = k + 1) begin
                lk_words[0] = {24'd0, 4'(OV_A(k)), 2'd0, OV_S(k) == 2, 1'b1};
                lk_words[1] = {6'd0, 10'(OV_Y(k)), 5'd0, 11'(OV_X(k))};
                lk_words[2] = {6'd0, 10'(OV_H(k)), 5'd0, 11'(OV_W(k))};
                lk_words[3] = 32'(OV_B(k));
                link_xfer(8'h01, 32'h0000_00C0 + 16 * k, 4);
            end
            link_xfer(8'h02, 32'h0000_00E4, 1);
            link_expect("OVL_STAT", lk_words[0], 32'd0);
            @(posedge ovl_frame_start);               // настройки взяты в этом кадре
            ovl_live = 1'b1;
        end
        // Почтовый ящик ESP → BL616: страница MB_PAGE и окно MBOX 0x200…0x3FC (16 слов за раз)
        begin : mbox
            integer k;
            lk_words[0] = 32'd2;
            link_xfer(8'h01, 32'h0000_0034, 1);
            for (k = 0; k < 16; k = k + 1) lk_words[k] = 32'hB000_0000 + k;
            link_xfer(8'h01, 32'h0000_0240, 16);     // слова 16…31 страницы 2
            lk_words[0] = 32'd0;
            link_xfer(8'h01, 32'h0000_0034, 1);
            lk_words[0] = 32'hB601_1234;
            link_xfer(8'h01, 32'h0000_0200, 1);      // слово 0 (заголовок)
            #(200);
            link_xfer(8'h02, 32'h0000_0034, 1);
            link_expect("MB_PAGE", lk_words[0], 32'd0);
            link_expect("ящик: слово 0", dut.cp_link.mb_mem[0], 32'hB601_1234);
            for (k = 0; k < 16; k = k + 1)
                link_expect($sformatf("ящик: слово %0d", 256 + 16 + k), dut.cp_link.mb_mem[256 + 16 + k],
                            32'hB000_0000 + k);
        end
        // Flash: настройки прочитаны, в чистой flash не найдены; у ядра FT812 (1.31+) моста нет
        link_xfer(8'h02, 32'h0000_004C, 1);
        link_expect("FLASH_STATUS", lk_words[0] & 32'hFF_00_07, HAS_EVO ? 32'h00_00_02 : 32'd0);
        // SCRATCH: запись и чтение
        lk_words[0] = 32'hA5C3_0F96;
        link_xfer(8'h01, 32'h0000_003C, 1);
        #(200);
        link_xfer(8'h02, 32'h0000_003C, 1);
        link_expect("SCRATCH", lk_words[0], 32'hA5C3_0F96);
        // FRAME BLEND (1.32): CTRL 17 пишется и читается; на таблице кадры не смешиваются (CHECK 18)
        link_xfer(8'h02, 32'h0000_0040, 1);
        lk_words[0] = (lk_words[0] & ~32'h0000_0200) | 32'h0002_0000;
        link_xfer(8'h01, 32'h0000_0040, 1);
        #(200);
        link_xfer(8'h02, 32'h0000_0040, 1);
        link_expect("CTRL: FRAME BLEND", lk_words[0] & 32'h0002_0000, 32'h0002_0000);
        if (HAS_EVO) begin
            // у ядра FT812 два кадра ожидания задели бы проверку автовозврата (SWITCH_MS = 1)
            repeat (2) @(posedge chk_tick);
            link_xfer(8'h02, 32'h0000_0024, 1);
            link_expect("CHECK 18: на таблице без смешения", lk_words[0] & 32'h0004_0000, 32'd0);
        end
        link_xfer(8'h02, 32'h0000_0040, 1);
        lk_words[0] = lk_words[0] & ~32'h0002_0200;
        link_xfer(8'h01, 32'h0000_0040, 1);
        // BOOT (multiboot): читается {B00, разрешён автопереход, HAS_FT, HAS_EVO, слот
        // перехода 1, слот 0}; запись не «BOOT» не действует, «BOOT» — запрос перезагрузки
        // (RECONFIG_N тянет верхний модуль). Автопереходы проверяет SWITCH_TEST.
        link_xfer(8'h02, 32'h0000_00F0, 1);
        link_expect("BOOT", lk_words[0], {12'hB00, 2'b00, HAS_FT, HAS_EVO, 8'h01, 8'h00});
        if (!SWITCH_TEST) begin
            lk_words[0] = 32'h424F_4F55;
            link_xfer(8'h01, 32'h0000_00F0, 1);
            #(200);
            if (dut.boot_req !== 1'b0) err("BOOT: перезагрузка от неверного слова");
            lk_words[0] = 32'h424F_4F54;
            link_xfer(8'h01, 32'h0000_00F0, 1);
            #(200);
            if (dut.boot_req !== 1'b1) err("BOOT: «BOOT» не запросил перезагрузку");
            #(200);   // запрос — и в STATUS BL616 (1.13): бит 7 флагов, IRQ_N
            if (dut.boot_m_s[1] !== 1'b1 || dut.cp_irq_n !== 1'b0)
                err("BOOT: запрос не дошёл до STATUS BL616");
        end else if (HAS_EVO && !HAS_FT) begin
            // Ядро без FT812: обмен Evo с FT812 без «ARM1» не действует, после — перезагрузка
            ft_burst;
            #(2000);
            if (dut.boot_req !== 1'b0) err("автопереход: перезагрузка без ARM1");
            lk_words[0] = 32'h4152_4D31;
            link_xfer(8'h01, 32'h0000_00F0, 1);
            #(200);
            link_xfer(8'h02, 32'h0000_00F0, 1);
            link_expect("BOOT после ARM1", lk_words[0] & 32'h0004_0000, 32'h0004_0000);
            // источник TEST: CORERST при сбросе Evo вернёт его в AUTO
            link_xfer(8'h02, 32'h0000_0040, 1);
            lk_words[0] = (lk_words[0] & ~32'h0000_0203) | 32'd2;
            link_xfer(8'h01, 32'h0000_0040, 1);
            #(200);
            link_xfer(8'h02, 32'h0000_0040, 1);
            link_expect("CTRL: источник TEST", lk_words[0] & 32'h3, 32'd2);
            // команды хоста без ACTIVE (так FT812 гасят при сбросе Evo) — перехода нет
            fx[0] = 8'h68; fx[1] = 8'h00; fx[2] = 8'h00; ft_xact(3, FT_HP);   // CORERST
            fx[0] = 8'h50; ft_xact(3, 71.0);                                  // PWRDOWN, 7 МГц
            fx[0] = 8'h44; ft_xact(3, FT_HP);                                 // CLKEXT
            fx[0] = 8'h61; fx[1] = 8'hC8; ft_xact(3, FT_HP);                  // CLKSEL ×8
            #(2000);
            if (dut.boot_req !== 1'b0) err("автопереход: команды хоста без ACTIVE перезагрузили ПЛИС");
            link_xfer(8'h02, 32'h0000_0040, 1);
            link_expect("CTRL: после CORERST источник AUTO", lk_words[0] & 32'h3, 32'd0);
            link_xfer(8'h02, 32'h0000_0050, 2);
            link_expect("FT_SEEN после CLKSEL", lk_words[0], 32'h0061_C800);
            link_expect("FT_COUNT после команд хоста", lk_words[1], 32'h0001_0518);   // запусков 1 (до ARM1), посылок 5, битов 24
            // запуск чипа — переход: ACTIVE или (SW_READ) чтение REG_ID на 28 МГц
            if (SW_READ) begin
                fx[0] = 8'h30; fx[1] = 8'h20; fx[2] = 8'h00; fx[3] = 8'h00; fx[4] = 8'h00;
                ft_xact(5, FT_HP);
            end else begin
                ft_burst;
            end
            #(2000);
            if (dut.boot_req !== 1'b1) err($sformatf("автопереход: %s после ARM1 не перезагрузил ПЛИС",
                                                     SW_READ ? "чтение REG_ID" : "ACTIVE"));
            else $display("автопереход: %s после ARM1 — перезагрузка в слот FT812; команды хоста без ACTIVE — нет",
                          SW_READ ? "чтение REG_ID" : "ACTIVE");
            sw_done = 1'b1;
        end
        // Хост FT812 — ESP (FT_CFG бит 2): обмен Evo до ft_spi не доходит, MISO и INT_N к Evo
        // отключены; бит снят — обмен Evo снова доходит
        if (HAS_FT && !SWITCH_TEST) begin : esp_host
            reg tg0;
            lk_words[0] = 32'd4;
            link_xfer(8'h01, 32'h0000_005C, 1);
            #(200);
            link_xfer(8'h02, 32'h0000_005C, 1);
            link_expect("FT_STAT: хост — ESP", lk_words[0] & 32'h0000_1000, 32'h0000_1000);
            tg0 = dut.ft_tg;
            ft_burst;
            #(1000);
            if (dut.ft_tg !== tg0) err("хост — ESP: обмен Evo дошёл до ft_spi");
            if (dut.ft_miso !== 1'b0 || dut.ft_int_n !== 1'b1)
                err("хост — ESP: MISO или INT_N к Evo не отключены");
            // FT_HOST: ESP будит FT812 командой ACTIVE (00), затем REG_ID читается окном
            if (dut.g_ft.ft.pmode !== 2'd2) err("FT812 после сброса не в SLEEP");
            lk_words[0] = 32'd0;
            link_xfer(8'h01, 32'h0000_00E8, 1);
            #(500);
            if (dut.g_ft.ft.pmode !== 2'd0) err("FT_HOST: ACTIVE не разбудил FT812");
            lk_words[0] = 32'h0030_2000;             // REG_ID
            link_xfer(8'h01, 32'h0000_0054, 1);
            #(3000);
            link_xfer(8'h02, 32'h0000_0058, 1);
            link_expect("REG_ID окном после ACTIVE", lk_words[0] & 32'hFF, 32'h7C);
            // Окно FT812 с 1.29: слова записи (FT_DATA, FT_BURST) — через очередь на 512 слов.
            // FT_STAT 14 — потеря: слово во время чтения (FT_ADDR ещё не ответил) или в полную
            // очередь; принятый FT_ADDR отметку снимает.
            // (а) Слово во время чтения: отклик чтения держится подменой ft_rs_s
            begin : lost
                reg [1:0] rs_hold;
                rs_hold = dut.ft_rs_s;
                lk_words[0] = 32'h0000_1000;
                link_xfer(8'h01, 32'h0000_0054, 1);
                force dut.ft_rs_s = rs_hold;          // ответ на чтение «не приходит»
                lk_words[0] = 32'h1234_5678;
                link_xfer(8'h01, 32'h0000_0058, 1);
                #(200);
                link_xfer(8'h02, 32'h0000_005C, 1);
                link_expect("FT_STAT 14: слово во время чтения отмечено", lk_words[0] & 32'h4000, 32'h4000);
                link_expect("FT_STAT 31: чтение идёт — окно занято", lk_words[0] & 32'h8000_0000, 32'h8000_0000);
                release dut.ft_rs_s;
                ft_wait_idle("чтение после подмены отклика");
                lk_words[0] = 32'h0000_1000;
                link_xfer(8'h01, 32'h0000_0054, 1);
                ft_wait_idle("FT_ADDR после потери");
                link_xfer(8'h02, 32'h0000_005C, 1);
                link_expect("FT_STAT 14: FT_ADDR снял отметку", lk_words[0] & 32'h4000, 32'h0);
                // FT_DATA — по адресу окна, адрес += 4
                lk_words[0] = 32'h1234_5678;
                link_xfer(8'h01, 32'h0000_0058, 1);
                lk_words[0] = 32'h9ABC_DEF0;
                link_xfer(8'h01, 32'h0000_0058, 1);
                ft_wait_idle("два слова FT_DATA");
                ft_read_expect("окно: первое слово FT_DATA", 32'h0000_1000, 32'h1234_5678);
                ft_read_expect("окно: второе слово FT_DATA", 32'h0000_1004, 32'h9ABC_DEF0);
            end
            // (б) FT_BURST: FT_ADDR, затем одна транзакция LINK в 16 слов на 0x40000 — слова
            // подряд с адреса окна; FT_FREE в покое — 512 (код Грея 0x300)
            begin : burst
                integer k;
                lk_words[0] = 32'h0000_1100;
                link_xfer(8'h01, 32'h0000_0054, 1);
                ft_wait_idle("FT_ADDR перед FT_BURST");
                for (k = 0; k < 16; k++) lk_words[k] = 32'hB000_0000 + k * 32'h0101_0101;
                link_xfer(8'h01, 32'h0004_0000, 16);
                ft_wait_idle("FT_BURST, 16 слов");
                for (k = 0; k < 16; k++)
                    ft_read_expect($sformatf("FT_BURST: слово %0d", k), 32'h0000_1100 + 4 * k,
                                   32'hB000_0000 + k * 32'h0101_0101);
                link_xfer(8'h02, 32'h0000_0038, 1);
                link_expect("FT_FREE в покое (Грей 512)", lk_words[0], 32'h0000_0300);
            end
            // (в) Переполнение: вывод очереди стоит (порт слов не отвечает), 33 транзакции по 16
            // слов — 512 мест заняты (FT_FREE 0), последние 16 слов теряются (FT_STAT 14); вывод
            // пошёл — очередь дописана, окно свободно
            begin : wq_full
                integer t, k;
                lk_words[0] = 32'h0000_2000;
                link_xfer(8'h01, 32'h0000_0054, 1);
                ft_wait_idle("FT_ADDR перед переполнением");
                force dut.ft_p_ack = 1'b0;
                for (t = 0; t < 33; t++) begin
                    for (k = 0; k < 16; k++) lk_words[k] = 32'hC000_0000 + 16 * t + k;
                    link_xfer(8'h01, 32'h0004_0000 + 64 * t, 16);
                end
                #(500);
                link_xfer(8'h02, 32'h0000_0038, 1);
                link_expect("FT_FREE: очередь полна", lk_words[0], 32'h0);
                link_xfer(8'h02, 32'h0000_005C, 1);
                link_expect("FT_STAT 14: слово в полную очередь отмечено", lk_words[0] & 32'h4000, 32'h4000);
                link_expect("FT_STAT 31: очередь не записана — окно занято", lk_words[0] & 32'h8000_0000, 32'h8000_0000);
                release dut.ft_p_ack;
                ft_wait_idle("очередь после переполнения");
                link_xfer(8'h02, 32'h0000_0038, 1);
                link_expect("FT_FREE: очередь записана", lk_words[0], 32'h0000_0300);
                ft_read_expect("переполнение: слово 0",   32'h0000_2000, 32'hC000_0000);
                ft_read_expect("переполнение: слово 1",   32'h0000_2004, 32'hC000_0001);
                ft_read_expect("переполнение: слово 255", 32'h0000_23FC, 32'hC000_00FF);
                ft_read_expect("переполнение: слово 511", 32'h0000_27FC, 32'hC000_01FF);
                lk_words[0] = 32'h0000_2800;          // слово 512 потеряно
                link_xfer(8'h01, 32'h0000_0054, 1);
                ft_wait_idle("чтение слова 512");
                link_xfer(8'h02, 32'h0000_0058, 1);
                link_checks = link_checks + 1;
                if (lk_words[0] === 32'hC000_0200) err("переполнение: слово 512 записано, хотя потеряно");
                link_xfer(8'h02, 32'h0000_005C, 1);
                link_expect("FT_STAT 14: FT_ADDR снял отметку после переполнения", lk_words[0] & 32'h4000, 32'h0);
            end
            lk_words[0] = 32'd0;
            link_xfer(8'h01, 32'h0000_005C, 1);
            #(200);
            ft_burst;
            #(1000);
            if (dut.ft_tg === tg0) err("хост — Evo: обмен Evo не дошёл до ft_spi");
            else $display("хост FT812 — ESP: SPI Evo отключён; снова Evo — обмен доходит");
        end
        // Поправка окна: H +5, V −3 — должна дойти до настроек
        lk_words[0] = {16'd0, 8'hFD, 8'h05};
        link_xfer(8'h01, 32'h0000_0044, 1);
        #(200);
        link_expect("ADJUST → H", {24'd0, dut.ui.cfg_h_adj}, 32'h05);
        link_expect("ADJUST → V", {24'd0, dut.ui.cfg_v_adj}, 32'hFD);
        link_xfer(8'h02, 32'h0000_0044, 1);
        link_expect("ADJUST", lk_words[0], {16'd0, 8'hFD, 8'h05});
        // Поправка легла в таблицу текущего растра; запись в таблицу другого растра
        // текущую не меняет
        link_xfer(8'h02, 32'h0000_0060 + 4 * dut.p_mode, 1);
        link_expect("ADJ_TAB текущего растра", lk_words[0], {16'd0, 8'hFD, 8'h05});
        lk_words[0] = {16'd0, 8'h11, 8'h22};
        link_xfer(8'h01, 32'h0000_0060 + 4 * ((dut.p_mode + 1) % 8), 1);
        #(200);
        link_xfer(8'h02, 32'h0000_0060 + 4 * ((dut.p_mode + 1) % 8), 1);
        link_expect("ADJ_TAB другого растра", lk_words[0], {16'd0, 8'h11, 8'h22});
        link_xfer(8'h02, 32'h0000_0044, 1);
        link_expect("ADJUST после записи другого растра", lk_words[0], {16'd0, 8'hFD, 8'h05});
        lk_words[0] = 32'd0;
        link_xfer(8'h01, 32'h0000_0060 + 4 * ((dut.p_mode + 1) % 8), 1);
        // Мост к flash: JEDEC ID модели
        lk_words[0] = 32'h9F00_0000;
        link_xfer(8'h01, 32'h0000_0080, 1);
        lk_words[0] = {16'd0, 8'd3, 8'd1};
        link_xfer(8'h01, 32'h0000_0048, 1);
        #(20_000);
        link_xfer(8'h02, 32'h0000_0080, 1);
        link_expect("мост: JEDEC ID", lk_words[0] & 32'hFFFF_FF00, HAS_EVO ? 32'hEF40_1700 : 32'd0);
        // Мост к microSD (1.30, только ядро с Evo): CMD0 и блок 2 — окнами LINK, как ESP
        if (HAS_EVO) begin : sd
            integer k, n, bad;
            lk_words[0] = {23'd0, 1'b1, 8'd1};                // CS активен, SCK = пиксели / 4
            link_xfer(8'h01, 32'h0000_0100, 1);
            lk_words[0] = 32'd0;
            link_xfer(8'h01, 32'h0000_0104, 1);               // PTR 0
            link_xfer(8'h02, 32'h0000_0100, 2);
            link_expect("SD_CTRL", lk_words[0] & 32'h1FF, 32'h101);
            link_expect("SD_PTR", lk_words[1], 32'd0);
            lk_words[0] = 32'h4000_0000; lk_words[1] = 32'h0095_FFFF;
            link_xfer(8'h01, 32'h0000_2000, 2);               // CMD0 — в окно TX
            sd_op(0, 6); sd_op(2, 16);
            link_expect("SD: CMD0 R1", sd_st & 32'hFFFF_FF03, 32'h5D01_0100);
            lk_words[0] = 32'h5100_0000; lk_words[1] = 32'h02FF_FFFF;
            link_xfer(8'h01, 32'h0000_2000, 2);               // CMD17, блок 2
            sd_op(0, 6); sd_op(2, 16);
            link_expect("SD: CMD17 R1", sd_st & 32'hFFFF_FF03, 32'h5D01_0000);
            lk_words[0] = {5'd0, 11'd514, 16'd0};
            link_xfer(8'h01, 32'h0000_0104, 1);
            sd_op(3, 10000);
            link_expect("SD: маркер блока", sd_st & 32'hFFFF_FF03, 32'h5D01_FE00);
            bad = 0;
            for (k = 0; k < 128; k = k + 16) begin
                link_xfer(8'h02, 32'h0000_2000 + 4 * k, 16);
                for (n = 0; n < 16; n = n + 1)
                    if (lk_words[n] !== {tfcard.mem[1024 + 4 * (k + n)], tfcard.mem[1025 + 4 * (k + n)],
                                         tfcard.mem[1026 + 4 * (k + n)], tfcard.mem[1027 + 4 * (k + n)]})
                        bad = bad + 1;
            end
            link_expect("SD: блок 2 окном RX — слов не тех", bad, 0);
            lk_words[0] = 32'd0;
            link_xfer(8'h01, 32'h0000_0100, 1);               // CS неактивен
            #(200);
            if (tf_cs_n !== 1'b1) err("SD: CS остался активным");
        end else begin
            link_xfer(8'h02, 32'h0000_0108, 1);
            link_expect("SD_STAT без моста", lk_words[0], 32'd0);
        end
        // Состояние: таблица в SDRAM, SDRAM готова, есть кадр; регистры подряд 0x08…0x24
        link_xfer(8'h02, 32'h0000_0008, 8);
        link_expect("STATUS: таблица/SDRAM/кадр", lk_words[0] & 32'h0E00, 32'h0E00);
        link_expect("SYNC: карта калибровки", (lk_words[2] >> 16) & 32'hF, CAL ? CAL_EXP : 4'd0);
        link_expect("CHECK: расхождений нет", lk_words[7] & 32'hFFFF, 32'd0);
        $display("[%0t] LINK: ID %08h, STATUS %08h, проверок %0d", $time, 32'h5644_4334,
                 lk_words[0], link_checks);
        // Вернуть поправку
        lk_words[0] = 32'd0;
        link_xfer(8'h01, 32'h0000_0044, 1);
        link_done = 1'b1;
    end

    // ------------------------------------------------------------------
    // Ход теста
    // ------------------------------------------------------------------
    real ratio;
    initial begin
        if ($test$plusargs("dump")) begin
            $dumpfile("tb_ts_fb.vcd");
            $dumpvars(0, tb_ts_fb);
        end
        #200;
        rst_n = 1'b1;
        wait (out_frames == OUT_FRAMES + 1);
        #100;

        ratio = (1.0e9 / (src_ht * src_vt * 35.714)) /
                (1.0e9 / (H_TOTAL * V_TOTAL * T_PIX));
        $display("---- итоги ----");
        $display("выходных кадров: %0d (чёрных до первого кадра: %0d, с картинкой: %0d)",
                 out_frames - 1, black_frames, valid_frames);
        $display("период выходного кадра: %.3f…%.3f мкс (расчёт %.3f)",
                 period_min / 1000.0, period_max / 1000.0, H_TOTAL * V_TOTAL * T_PIX / 1000.0);
        $display("входных кадров записано: %0d, отброшено сбойных: %0d",
                 frames_written, frames_rejected);
        $display("повторов кадра: %0d, пропусков: %0d; вход/выход по частоте = %.4f",
                 repeats, skips, ratio);
        if (lat_n > 0)
            $display("задержка от конца входного кадра до начала показа: в среднем %.1f мкс",
                     lat_sum / lat_n / 1000.0);
        $display("растр: захвачен=%0d, режим %0d (ожидался %0d), строка %0d тактов, %0d строк, HS %0d тактов, активный уровень HS/VS низкий=%0d/%0d",
                 raster_locked, raster_mode, exp_mode,
                 raster_line_cycles, raster_lines, raster_hs_cycles, dut.hs_low, dut.vs_low);
        if (SWITCH_TO >= 0 && t_lock > 0.0)
            $display("после смены растра захват через %.2f мс", (t_lock - t_switch) / 1.0e6);
        $display("SDRAM: ACT %0d, READ %0d, WRITE %0d, REFRESH %0d, макс. интервал регенерации %.2f мкс, ошибок протокола %0d",
                 sdram.n_act, sdram.n_rd, sdram.n_wr, sdram.n_ref, sdram.max_refi / 1000.0, sdram.errors);

        $display("самая долгая выборка строк подряд: %.2f мкс (строка вывода %.2f мкс)",
                 fetch_max / 1000.0, H_TOTAL * T_PIX / 1000.0);
        if (AVG_TEST) begin
            $display("FRAME BLEND: кадров со смешением %0d, без смешения после первого смешанного %0d; настройка из flash = %0d",
                     avg_frames, avg_plain, dut.ui.cfg_avg);
            if (dut.ui.cfg_avg !== 1'b1) err("FRAME BLEND: настройка из flash не взята");
            if (avg_frames < 2) err("FRAME BLEND: мало кадров со смешением");
        end
        if (sdram.errors != 0) err("модель SDRAM нашла нарушения протокола");
        if (cap_overflow)      err("FIFO захвата переполнялся");
        if (fetch_late)        err("выборка строки не успела");
        if (PATTERN) begin
            $display("калибровка SD READ: карта годных точек %b (ожидалась %b), выбрана %0d, отказ %0d",
                     dut.ui.cal_ok, CAL_EXP, dut.ui.cfg_rd, dut.ui.cal_fail);
            $display("кадров таблицы сверено: %0d, расхождений: %0d; сверка в прошивке за последний кадр: %0d",
                     pat_frames, pat_errs, chk_err);
            if (CAL) begin
                if (!dut.ui.cal_done || dut.ui.cal_fail) err("калибровка не завершилась");
                if (dut.ui.cal_ok !== CAL_EXP)          err("карта годных точек не та");
                if (dut.ui.cfg_rd !== RD_EXPECT[1:0])   err("выбрана не та точка выборки");
            end
            if (LINK_TEST && !link_done) err("обмен по LINK не завершился");
            if (LINK_TEST) $display("LINK: проверок %0d", link_checks);
            if (SWITCH_TEST && HAS_FT != HAS_EVO && !sw_done)
                err("автопереход: проверка не успела закончиться (мало OUT_FRAMES)");
            if (OVL_ON) begin
                $display("оверлеи: сверено точек оверлея 0 — %0d", ovl_px_n);
                if (ovl_px_n < 100) err("оверлеи: мало сверенных точек (оверлей не показан?)");
            end
            if (pat_frames < 3) err("мало сверенных кадров таблицы");
            if (chk_err != 0)   err("сверка таблицы в прошивке нашла расхождения");
            $display("ошибок: %0d", errors);
            if (errors == 0) $display("ТЕСТ ПРОЙДЕН");
            else             $display("ТЕСТ НЕ ПРОЙДЕН");
            $finish;
        end
        if (valid_frames < 2)  err("слишком мало кадров с картинкой");
        if (!raster_locked)    err("растр не захвачен");
        $display("границы картинки: x %0d…%0d, y %0d…%0d; окно %0d/%0d, подстроено по x/y: %0d/%0d",
                 bb_x0, bb_x1, bb_y0, bb_y1, dut.h_off, dut.v_off, aligned_h, aligned_v);
        if (SHIFTED) begin
            $display("кадров до подстройки: %0d, после: %0d", pre_align_frames, aligned_frames);
            if (align_fn < 0)       err("окно не подстроилось по картинке");
            if (aligned_frames < 2) err("мало точных кадров после подстройки");
        end
        if (raster_mode != exp_mode) err("определён не тот режим");
        if (dut.hs_low != !src_hs_pos || dut.vs_low != !src_vs_pos) err("полярность определена неверно");
        if (SWITCH_TO >= 0) begin
            // при смене растра пропуски и повторы ожидаемы: кадры на время перезахвата держатся
        end else if (STOP_AFTER == 0) begin
            if (ratio < 1.0 && skips != 0) err("пропуски кадров при медленном входе");
            if (ratio > 1.0 && repeats != 0 && ratio > 1.05) err("повторы при заметно более быстром входе");
        end else begin
            if (stop_seen_frames < 2) err("после остановки PCLK выход не продолжил развёртку");
        end
        $display("ошибок: %0d", errors);
        if (errors == 0) $display("ТЕСТ ПРОЙДЕН");
        else             $display("ТЕСТ НЕ ПРОЙДЕН");
        $finish;
    end

    initial begin
        #(MINI ? 60_000_000 : 1_000_000_000);
        $display("ТАЙМАУТ: выходных кадров %0d", out_frames);
        $display("ТЕСТ НЕ ПРОЙДЕН");
        $finish;
    end
endmodule
