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
//   OSD=1 — экранное меню поверх картинки; сверка картинки отключается, остаётся выгрузка
//     (+ppm) — для просмотра меню глазами.
//
// Модель SDRAM тактируется, как на плате, инверсией клока логики и выдаёт данные чтения
// с паспортными tAC/tOH ('x между словами): неверная точка выборки чтения ловится.
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
    parameter bit OSD         = 1'b0,
    parameter logic [3:0] CAL_MAP = 4'b0011,  // годные точки при tAC 6 нс, tOH 2,5 нс
    parameter int RD_EXPECT   = 1,
    parameter int RD_FIXED    = 1,             // точка выборки без калибровки
    parameter bit LINK_TEST   = PATTERN,       // обмен с регистрами по LINK, как его ведёт ESP
    parameter real LINK_NS    = 25.0,          // период LINK_SCK (40 МГц)
    parameter int BORDER      = 0,             // MINI: чёрное поле вокруг картинки (окно вывода)
    parameter bit HD          = 1'b0,          // MINI=0: выход 1280×720p50, картинка на всю высоту
    parameter bit ASPECT      = 1'b0,          // HD: 0 — 4:3 (960×720), 1 — 16:9 (1280×720)
    parameter int SIZE        = 0              // HD: высота 0 — 100 %, 1 — 95 %, 2 — 90 %
);
    `include "fb_pattern_fn.svh"
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

    function automatic [4:0] p_r(input integer x, input integer y, input integer f);
        if (y == 0 && x < 8) p_r = f[4:0];
        else if (use_img)    p_r = img_px(x, y, f) >> 0;
        else                 p_r = (x/4 + f) % 32;
    endfunction
    function automatic [4:0] p_g(input integer x, input integer y, input integer f);
        if (y == 0 && x < 8) p_g = f[9:5];
        else if (use_img)    p_g = img_px(x, y, f) >> 5;
        else                 p_g = (y/4 + 3*f) % 32;
    endfunction
    function automatic [4:0] p_b(input integer x, input integer y, input integer f);
        if (y == 0 && x < 8) p_b = ~f[4:0];
        else if (use_img)    p_b = img_px(x, y, f) >> 10;
        else                 p_b = ((x + y)/8 + 5*f) % 32;
    endfunction
    function automatic p_p(input integer x, input integer y, input integer f);
        reg [15:0] w;
        begin
            if (y == 0 && x < 8) p_p = 1'b1;          // PAL_SEL=1: расширение обратимо
            else if (use_img) begin w = img_px(x, y, f); p_p = w[15]; end
            else              p_p = ((x/16 + y/16 + f) % 2);
        end
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
        .OSD(OSD),
        .DEF_SRC(PATTERN ? 2'd0 : 2'd1),          // с Evo — сразу EVO, без Evo — AUTO
        .DEF_RD(RD_FIXED[1:0]),
        .DEF_ALIGN(AUTO_ALIGN),
        .CALIBRATE(CAL),
        .SCALE(SCALED), .HDMI(SCALED), .DEF_ASPECT(ASPECT), .DEF_SIZE(SIZE[1:0]),
        .FLASH_BOOT_US(20)
    ) dut (
        .clk_cap(clk_cap), .rst_cap_n(rst_n),
        .clk_mem(clk_mem), .rst_mem_n(rst_n),
        .pixel_clk(pixel_clk), .rst_pix_n(rst_n), .blank(1'b0), .btn(2'b00),
        .link_sck(lk_sck), .link_cs_n(lk_cs_n),
        .link_io_in(lk_io), .link_io_out(lk_so), .link_io_oe(lk_soe),
        .flash_sck(fl_sck), .flash_cs_n(fl_cs_n), .flash_mosi(fl_mosi), .flash_miso(fl_miso),
        .mux_d(mux_d), .hsync_in(hsync_in), .vsync_in(vsync_in), .mux_sel(mux_sel),
        .sdram_cs_n(sd_cs_n), .sdram_ras_n(sd_ras_n), .sdram_cas_n(sd_cas_n),
        .sdram_we_n(sd_we_n), .sdram_a(sd_a), .sdram_ba(sd_ba), .sdram_dqm(sd_dqm),
        .sdram_dq_out(sd_dq_out), .sdram_dq_oe(sd_dq_oe), .sdram_dq_in(sd_dq_in),
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
    sdram_model #(.TCK_NS(T_MEM), .CL(2)) sdram (
        .clk(~clk_mem), .cs_n(sd_cs_n), .ras_n(sd_ras_n), .cas_n(sd_cas_n), .we_n(sd_we_n),
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
    // Проверка выхода
    // ------------------------------------------------------------------
    integer errors = 0;
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

    task automatic err(input string msg);
        begin
            if (errors < 25) $display("[%0t] ОШИБКА: %0s", $time, msg);
            errors = errors + 1;
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
                pat_check = (cal_frames >= 2) && !OSD;
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
            if (pat_check && inimg && !(iy >= FB_PAT_BAND_Y0 && iy < FB_PAT_BAND_Y1)) begin
                pat_exp = fb_pattern_px(ix, iy, 8'd0, H_ACTIVE, V_ACTIVE);
                if (m_r !== expand(pat_exp[15], pat_exp[4:0]) ||
                    m_g !== expand(pat_exp[15], pat_exp[9:5]) ||
                    m_b !== expand(pat_exp[15], pat_exp[14:10])) begin
                    if (pat_frame_err < 3)
                        $display("[%0t] таблица x=%0d y=%0d: %02x %02x %02x, ожидалось %02x %02x %02x",
                                 $time, ix, iy, m_r, m_g, m_b, expand(pat_exp[15], pat_exp[4:0]),
                                 expand(pat_exp[15], pat_exp[9:5]), expand(pat_exp[15], pat_exp[14:10]));
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
            if (first) begin
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
                er = blend(expand(p_p(ix, iy, shown_id), p_r(ix, iy, shown_id)),
                           expand(p_p(x2, iy, shown_id), p_r(x2, iy, shown_id)),
                           expand(p_p(ix, y2, shown_id), p_r(ix, y2, shown_id)),
                           expand(p_p(x2, y2, shown_id), p_r(x2, y2, shown_id)), wx, wy);
                eg = blend(expand(p_p(ix, iy, shown_id), p_g(ix, iy, shown_id)),
                           expand(p_p(x2, iy, shown_id), p_g(x2, iy, shown_id)),
                           expand(p_p(ix, y2, shown_id), p_g(ix, y2, shown_id)),
                           expand(p_p(x2, y2, shown_id), p_g(x2, y2, shown_id)), wx, wy);
                eb = blend(expand(p_p(ix, iy, shown_id), p_b(ix, iy, shown_id)),
                           expand(p_p(x2, iy, shown_id), p_b(x2, iy, shown_id)),
                           expand(p_p(ix, y2, shown_id), p_b(ix, y2, shown_id)),
                           expand(p_p(x2, y2, shown_id), p_b(x2, y2, shown_id)), wx, wy);
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

    initial if (LINK_TEST) begin
        wait (rst_n && dut.mem_ready);
        if (CAL) wait (dut.ui.cal_done);
        repeat (3) @(posedge chk_tick);
        // ID и версия одним чтением двух слов
        link_xfer(8'h02, 32'h0000_0000, 2);
        link_expect("ID", lk_words[0], 32'h5644_4334);
        link_expect("VERSION", lk_words[1], {8'h01, 8'h01, 16'h0103});
        // UI_KEY: меню — с клавиатуры Evo через ESP, как кнопками Tang
        begin : ui_key
            reg [3:0] c0;
            reg [1:0] d0;
            lk_words[0] = 32'd5;                         // показать меню
            link_xfer(8'h01, 32'h0000_0050, 1);
            #(200);
            link_expect("UI_KEY показать", {31'd0, dut.ui.osd_on}, 32'd1);
            c0 = dut.ui.cursor;
            lk_words[0] = 32'd1;                         // следующий пункт
            link_xfer(8'h01, 32'h0000_0050, 1);
            #(200);
            link_expect("UI_KEY следующий", {28'd0, dut.ui.cursor}, {28'd0, c0 + 4'd1});
            d0 = dut.ui.cfg_delay;                       // пункт 1 — MUX DELAY
            lk_words[0] = 32'd3;
            link_xfer(8'h01, 32'h0000_0050, 1);
            #(200);
            link_expect("UI_KEY +1", {30'd0, dut.ui.cfg_delay}, {30'd0, d0 + 2'd1});
            lk_words[0] = 32'd4;
            link_xfer(8'h01, 32'h0000_0050, 1);
            #(200);
            link_expect("UI_KEY −1", {30'd0, dut.ui.cfg_delay}, {30'd0, d0});
            lk_words[0] = 32'd2;                         // предыдущий
            link_xfer(8'h01, 32'h0000_0050, 1);
            #(200);
            link_expect("UI_KEY предыдущий", {28'd0, dut.ui.cursor}, {28'd0, c0});
            lk_words[0] = 32'd6;                         // скрыть
            link_xfer(8'h01, 32'h0000_0050, 1);
            #(200);
            link_expect("UI_KEY скрыть", {31'd0, dut.ui.osd_on}, 32'd0);
        end
        // Flash: настройки прочитаны, в чистой flash не найдены
        link_xfer(8'h02, 32'h0000_004C, 1);
        link_expect("FLASH_STATUS", lk_words[0] & 32'hFF_00_07, 32'h00_00_02);
        // SCRATCH: запись и чтение
        lk_words[0] = 32'hA5C3_0F96;
        link_xfer(8'h01, 32'h0000_003C, 1);
        #(200);
        link_xfer(8'h02, 32'h0000_003C, 1);
        link_expect("SCRATCH", lk_words[0], 32'hA5C3_0F96);
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
        link_expect("мост: JEDEC ID", lk_words[0] & 32'hFFFF_FF00, 32'hEF40_1700);
        // Состояние: таблица в SDRAM, SDRAM готова, есть кадр; регистры подряд 0x08…0x24
        link_xfer(8'h02, 32'h0000_0008, 8);
        link_expect("STATUS: таблица/SDRAM/кадр", lk_words[0] & 32'h0E00, 32'h0E00);
        link_expect("SYNC: карта калибровки", (lk_words[2] >> 16) & 32'hF, CAL ? CAL_MAP : 4'd0);
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

        if (sdram.errors != 0) err("модель SDRAM нашла нарушения протокола");
        if (cap_overflow)      err("FIFO захвата переполнялся");
        if (fetch_late)        err("выборка строки не успела");
        if (PATTERN) begin
            $display("калибровка SD READ: карта годных точек %b (ожидалась %b), выбрана %0d, отказ %0d",
                     dut.ui.cal_ok, CAL_MAP, dut.ui.cfg_rd, dut.ui.cal_fail);
            $display("кадров таблицы сверено: %0d, расхождений: %0d; сверка в прошивке за последний кадр: %0d",
                     pat_frames, pat_errs, chk_err);
            if (CAL) begin
                if (!dut.ui.cal_done || dut.ui.cal_fail) err("калибровка не завершилась");
                if (dut.ui.cal_ok !== CAL_MAP)          err("карта годных точек не та");
                if (dut.ui.cfg_rd !== RD_EXPECT[1:0])   err("выбрана не та точка выборки");
            end
            if (LINK_TEST && !link_done) err("обмен по LINK не завершился");
            if (LINK_TEST) $display("LINK: проверок %0d", link_checks);
            if (!OSD && pat_frames < 3) err("мало сверенных кадров таблицы");
            if (!OSD && chk_err != 0)   err("сверка таблицы в прошивке нашла расхождения");
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
