// SPDX-License-Identifier: Apache-2.0
//
// Кнопки Tang Nano 20K, настройки и калибровка выборки SDRAM. Домен пиксельного клока.
//
// КНОПКИ S1 (FPGA88) и S2 (FPGA87). Активный уровень заранее не закладывается: через
// IDLE_MS после сброса текущий уровень каждой кнопки принимается за «не нажата».
// Поэтому прошивка не зависит от того, к какому питанию тянут кнопки на модуле, и ни одна
// кнопка не сбрасывает ПЛИС. Дребезг подавляется: уровень принимается, если он держится
// DEBOUNCE_MS подряд.
//
//   S1 коротко  — следующий пункт меню (если меню скрыто — показать его);
//   S1 долго    — скрыть / показать меню (LONG1_MS);
//   S2 коротко  — значение пункта +1 (если меню скрыто — показать его);
//   S2 долго    — значение −1, дальше −1 каждые REPEAT_MS, пока кнопка нажата.
// Меню скрывается само через HIDE_MS без нажатий, если идёт картинка Evo.
//
// НАЖАТИЯ ОТ ESP (LINK, регистр UI_KEY; плагин WC управляет этим же меню с клавиатуры
// Evo): ext_key 1 — следующий пункт (как S1 коротко), 2 — предыдущий, 3 — значение +1
// (S2 коротко), 4 — значение −1 (S2 долго), 5 — показать меню, 6 — скрыть.
//
// ПУНКТЫ МЕНЮ (cursor):
//   0 MUX PHASE   — с какой фазы мультиплексора начинается строка (0/1);
//   1 MUX DELAY   — задержка выборки мультиплексора, 1…4 такта 56 МГц;
//   2 MUX CLOCK   — фаза клока приёма 0…15 (1/16 периода 56 МГц, ts_pll_capture):
//                   сдвигает момент выборки относительно смены пикселя Evo;
//   3/4 H/V ADJUST — поправка окна захвата, −64…+63; своя для каждого растра (mode
//                   raster_detect, 0…7): при смене растра действует его поправка;
//   5 AUTO ALIGN  — автоподстройка окна по границам картинки;
//   6 SD READ     — точка выборки чтения SDRAM 0…3 (sdram_burst);
//   7 SD CALIBRATE — запустить калибровку заново;
//   8 ASPECT      — 4:3 (пропорции сохраняются) / 16:9 (растянуть на всю ширину);
//   9 SIZE        — высота картинки: 100 / 95 / 90 % кадра (для телевизоров, обрезающих
//                   края; fb_scanout, SCALE);
//  10 SOURCE      — AUTO (таблица, пока нет Evo) / EVO / TEST. Последним: после включения
//                   курсор стоит на первом пункте, и нажатия S2 не переключают источник.
// После сброса действуют DEF_*, через ~2 мс их заменяют настройки из flash Tang (fb_flash,
// ext_load_*), если они там есть: MUX PHASE/DELAY/CLOCK, AUTO ALIGN, ASPECT, SIZE и поправки
// всех растров.
// Источник и SD READ не сохраняются: источник после включения всегда DEF_SRC, SD READ
// калибруется заново. Записывает настройки во flash ESP (по LINK).
//
// КАЛИБРОВКА SD READ (после сброса, если CALIBRATE=1, и по пункту 7). На время калибровки
// источник — испытательная таблица. Когда опубликованы два кадра таблицы, для каждой точки
// выборки 0…3 выжидаются два кадра и берутся ошибки сверки двух следующих (fb_scanout).
// Точка годится, если ошибок нет. Из годных выбирается середина: при двух соседних —
// та, что ближе к 1–2. Если годных нет или таблица не появилась за CAL_TIMEOUT_MS —
// остаётся DEF_RD, в меню FAIL.
//
module fb_ui #(
    parameter int TICK           = 27_000,   // тактов на 1 мс
    parameter int IDLE_MS        = 20,
    parameter int DEBOUNCE_MS    = 10,
    parameter int LONG1_MS       = 700,
    parameter int LONG2_MS       = 500,
    parameter int REPEAT_MS      = 150,
    parameter int HIDE_MS        = 20_000,
    parameter int CAL_TIMEOUT_MS = 3_000,
    parameter logic [1:0] DEF_SRC   = 2'd0,  // 0 AUTO, 1 EVO, 2 TEST
    parameter bit         DEF_PHASE = 1'b1,
    parameter logic [1:0] DEF_DELAY = 2'd0,  // задержка выборки мультиплексора − 1
    parameter logic [3:0] DEF_CLOCK = 4'd0,  // фаза клока приёма
    parameter logic [1:0] DEF_RD    = 2'd1,
    parameter bit         DEF_ALIGN = 1'b1,
    parameter bit         DEF_ASPECT = 1'b0, // 0 — 4:3, 1 — 16:9
    parameter logic [1:0] DEF_SIZE  = 2'd0,  // 0 — 100 %, 1 — 95 %, 2 — 90 %
    parameter bit         CALIBRATE = 1'b1,
    parameter bit         OSD_START = 1'b1
) (
    input  wire              clk,
    input  wire              rst_n,
    input  wire [1:0]        btn,            // сырые уровни S1, S2

    // Состояние
    input  wire              chk_tick,       // конец активной части выходного кадра
    input  wire [15:0]       chk_err,        // ошибки сверки таблицы за этот кадр
    input  wire              pattern_on,     // SDRAM сейчас получает таблицу
    input  wire [15:0]       frames_written,
    input  wire              picture_ok,     // показывается картинка Evo

    // Запись настроек от ESP по LINK (регистры CTRL и ADJUST, см. ts_fb_core)
    input  wire              ext_ctrl_we,
    input  wire [16:0]       ext_ctrl,       // {size[1:0], aspect, фаза клока[3:0], калибровка, меню, rd[1:0], align, delay[1:0], phase, src[1:0]}
    input  wire              ext_adj_we,     // поправка текущего растра
    input  wire signed [7:0] ext_h_adj,
    input  wire signed [7:0] ext_v_adj,
    input  wire              ext_tab_we,     // поправка растра ext_tab_idx
    input  wire [2:0]        ext_tab_idx,
    input  wire [15:0]       ext_tab_data,   // {V, H}
    input  wire              ext_key_we,     // нажатие от ESP (UI_KEY)
    input  wire [2:0]        ext_key,
    input  wire              ext_load_stb,   // настройки из flash
    input  wire [10:0]       ext_load_cfg,   // {size[1:0], aspect, align, phase, delay[1:0], clock[3:0]}
    input  wire [127:0]      ext_load_adj,   // растр m — [m*16 +: 16] = {V, H}
    input  wire [2:0]        mode,           // растр сейчас (raster_detect)

    // Настройки
    output reg  [1:0]        set_src,        // как выбрано в меню
    output wire [1:0]        cfg_src,        // действующее (на время калибровки — TEST)
    output reg               cfg_phase,
    output reg  [1:0]        cfg_delay,
    output reg  [3:0]        cfg_clock,
    output wire signed [7:0] cfg_h_adj,      // поправка текущего растра
    output wire signed [7:0] cfg_v_adj,
    output wire [127:0]      adj_tab,        // поправки всех растров, как ext_load_adj
    output reg               cfg_align,
    output reg               cfg_aspect,     // 0 — 4:3, 1 — 16:9
    output reg  [1:0]        cfg_size,       // 0 — 100 %, 1 — 95 %, 2 — 90 %
    output reg  [1:0]        cfg_rd,

    // Меню
    output reg  [3:0]        cursor,
    output reg               osd_on,
    output reg               cal_busy,
    output reg  [1:0]        cal_point,
    output reg  [3:0]        cal_ok,
    output reg               cal_done,
    output reg               cal_fail
);
    localparam [16:0] TICK_LAST = TICK - 1;
    localparam [1:0]  SRC_TEST  = 2'd2;
    localparam [3:0]  MENU_SD_READ = 4'd6;
    localparam [3:0]  MENU_LAST    = 4'd10;

    assign cfg_src = cal_busy ? SRC_TEST : set_src;

    // ------------------------------------------------------------------
    // Миллисекунды
    // ------------------------------------------------------------------
    reg [16:0] tcnt;
    reg        ms;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin tcnt <= '0; ms <= 1'b0; end
        else begin
            ms <= (tcnt == TICK_LAST);
            tcnt <= (tcnt == TICK_LAST) ? 17'd0 : tcnt + 17'd1;
        end
    end

    // ------------------------------------------------------------------
    // Кнопки: синхронизация, уровень покоя, дребезг, длительность нажатия
    // ------------------------------------------------------------------
    reg [1:0]  b_s1, b_s2 /* synthesis syn_srlstyle = "registers" */;
    reg [1:0]  b_idle;
    reg        idle_known;
    reg [15:0] idle_cnt;
    reg [1:0]  b_raw, b_stable;
    reg [7:0]  deb [0:1];
    reg [15:0] held [0:1];
    reg [15:0] rep;
    reg s1_short, s1_long, s2_inc, s2_dec;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            b_s1 <= '0; b_s2 <= '0; b_idle <= '0; idle_known <= 1'b0; idle_cnt <= '0;
            b_raw <= '0; b_stable <= '0;
            deb[0] <= '0; deb[1] <= '0; held[0] <= '0; held[1] <= '0; rep <= '0;
            s1_short <= 1'b0; s1_long <= 1'b0; s2_inc <= 1'b0; s2_dec <= 1'b0;
        end else begin
            b_s1 <= btn;
            b_s2 <= b_s1;
            s1_short <= 1'b0; s1_long <= 1'b0; s2_inc <= 1'b0; s2_dec <= 1'b0;
            if (!idle_known) begin
                if (ms) idle_cnt <= idle_cnt + 16'd1;
                if (idle_cnt == IDLE_MS[15:0]) begin
                    b_idle     <= b_s2;
                    idle_known <= 1'b1;
                end
            end else begin
                b_raw <= b_s2 ^ b_idle;                 // 1 — нажата
                if (ms) begin : per_ms
                    integer i;
                    for (i = 0; i < 2; i = i + 1) begin
                        if (b_raw[i] == b_stable[i]) deb[i] <= '0;
                        else if (deb[i] == DEBOUNCE_MS[7:0] - 8'd1) begin
                            deb[i]      <= '0;
                            b_stable[i] <= b_raw[i];
                            if (b_raw[i]) held[i] <= '0;
                            else begin
                                // отпускание: короткое нажатие
                                if (i == 0 && held[0] < LONG1_MS[15:0]) s1_short <= 1'b1;
                                if (i == 1 && held[1] < LONG2_MS[15:0]) s2_inc   <= 1'b1;
                            end
                        end else deb[i] <= deb[i] + 8'd1;
                        if (b_stable[i] && held[i] != 16'hFFFF) held[i] <= held[i] + 16'd1;
                    end
                    if (b_stable[0] && held[0] == LONG1_MS[15:0] - 16'd1) s1_long <= 1'b1;
                    if (b_stable[1]) begin
                        if (held[1] == LONG2_MS[15:0] - 16'd1) begin
                            s2_dec <= 1'b1;
                            rep    <= '0;
                        end else if (held[1] >= LONG2_MS[15:0]) begin
                            if (rep == REPEAT_MS[15:0] - 16'd1) begin
                                s2_dec <= 1'b1;
                                rep    <= '0;
                            end else rep <= rep + 16'd1;
                        end
                    end
                end
            end
        end
    end

    // ------------------------------------------------------------------
    // Меню и настройки
    // ------------------------------------------------------------------
    reg [15:0] hide_cnt;
    reg        cal_start;

    // Поправки окна по растрам
    reg signed [7:0] tab_h [0:7];
    reg signed [7:0] tab_v [0:7];
    assign cfg_h_adj = tab_h[mode];
    assign cfg_v_adj = tab_v[mode];
    genvar gm;
    generate for (gm = 0; gm < 8; gm = gm + 1) begin : g_tab
        assign adj_tab[gm*16 +: 16] = {tab_v[gm], tab_h[gm]};
    end endgenerate

    // События меню: кнопки Tang и нажатия от ESP
    wire k_next = s1_short || (ext_key_we && ext_key == 3'd1);
    wire k_prev = ext_key_we && ext_key == 3'd2;
    wire k_inc  = s2_inc   || (ext_key_we && ext_key == 3'd3);
    wire k_dec  = s2_dec   || (ext_key_we && ext_key == 3'd4);
    wire k_show = ext_key_we && ext_key == 3'd5;
    wire k_hide = ext_key_we && ext_key == 3'd6;
    wire k_any  = s1_short || s1_long || s2_inc || s2_dec || ext_key_we;

    function automatic [1:0] wrap3(input [1:0] v, input up);
        wrap3 = up ? ((v == 2'd2) ? 2'd0 : v + 2'd1) : ((v == 2'd0) ? 2'd2 : v - 2'd1);
    endfunction

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            set_src   <= DEF_SRC;
            cfg_phase <= DEF_PHASE;
            cfg_delay <= DEF_DELAY;
            cfg_clock <= DEF_CLOCK;
            for (int m = 0; m < 8; m = m + 1) begin tab_h[m] <= '0; tab_v[m] <= '0; end
            cfg_align <= DEF_ALIGN;
            cfg_aspect <= DEF_ASPECT;
            cfg_size  <= DEF_SIZE;
            cursor    <= '0;
            osd_on    <= OSD_START;
            hide_cnt  <= '0;
            cal_start <= CALIBRATE;
        end else begin
            cal_start <= 1'b0;
            if (k_any) hide_cnt <= '0;
            else if (!osd_on || !picture_ok || cal_busy) hide_cnt <= '0;
            else if (ms) begin
                if (hide_cnt == HIDE_MS[15:0] - 16'd1) osd_on <= 1'b0;
                else hide_cnt <= hide_cnt + 16'd1;
            end

            if (k_show) begin
                osd_on <= 1'b1;
            end else if (k_hide) begin
                osd_on <= 1'b0;
            end else if (!osd_on) begin
                if (k_next || k_prev || s1_long || k_inc || (ext_key_we && k_dec)) osd_on <= 1'b1;
            end else if (s1_long) begin
                osd_on <= 1'b0;
            end else if (k_next) begin
                cursor <= (cursor == MENU_LAST) ? 4'd0 : cursor + 4'd1;
            end else if (k_prev) begin
                cursor <= (cursor == 4'd0) ? MENU_LAST : cursor - 4'd1;
            end else if (k_inc || k_dec) begin
                case (cursor)
                    4'd0: cfg_phase <= ~cfg_phase;
                    4'd1: cfg_delay <= k_inc ? cfg_delay + 2'd1 : cfg_delay - 2'd1;
                    4'd2: cfg_clock <= k_inc ? cfg_clock + 4'd1 : cfg_clock - 4'd1;
                    4'd3: if (k_inc ? cfg_h_adj != 8'sd63 : cfg_h_adj != -8'sd64)
                              tab_h[mode] <= k_inc ? cfg_h_adj + 8'sd1 : cfg_h_adj - 8'sd1;
                    4'd4: if (k_inc ? cfg_v_adj != 8'sd63 : cfg_v_adj != -8'sd64)
                              tab_v[mode] <= k_inc ? cfg_v_adj + 8'sd1 : cfg_v_adj - 8'sd1;
                    4'd5: cfg_align <= ~cfg_align;
                    4'd6: ;                              // SD READ — в блоке калибровки
                    4'd7: cal_start <= 1'b1;
                    4'd8: cfg_aspect <= ~cfg_aspect;
                    4'd9: cfg_size <= wrap3(cfg_size, k_inc);
                    default: set_src <= wrap3(set_src, k_inc);
                endcase
            end

            // Запись от ESP — после кнопок, то есть важнее их
            if (ext_ctrl_we) begin
                set_src   <= (ext_ctrl[1:0] == 2'd3) ? 2'd0 : ext_ctrl[1:0];
                cfg_phase <= ext_ctrl[2];
                cfg_delay <= ext_ctrl[4:3];
                cfg_clock <= ext_ctrl[13:10];
                cfg_align <= ext_ctrl[5];
                cfg_aspect <= ext_ctrl[14];
                cfg_size   <= (ext_ctrl[16:15] == 2'd3) ? 2'd0 : ext_ctrl[16:15];
                osd_on    <= ext_ctrl[8];
                cal_start <= ext_ctrl[9];
            end
            if (ext_adj_we) begin
                tab_h[mode] <= ext_h_adj;
                tab_v[mode] <= ext_v_adj;
            end
            if (ext_tab_we) begin
                tab_h[ext_tab_idx] <= ext_tab_data[7:0];
                tab_v[ext_tab_idx] <= ext_tab_data[15:8];
            end
            // Настройки из flash — один раз после сброса
            if (ext_load_stb) begin
                cfg_clock <= ext_load_cfg[3:0];
                cfg_delay <= ext_load_cfg[5:4];
                cfg_phase <= ext_load_cfg[6];
                cfg_align <= ext_load_cfg[7];
                cfg_aspect <= ext_load_cfg[8];
                cfg_size   <= (ext_load_cfg[10:9] == 2'd3) ? 2'd0 : ext_load_cfg[10:9];
                for (int m = 0; m < 8; m = m + 1) begin
                    tab_h[m] <= ext_load_adj[m*16 +: 8];
                    tab_v[m] <= ext_load_adj[m*16 + 8 +: 8];
                end
            end
        end
    end

    // ------------------------------------------------------------------
    // Калибровка точки выборки чтения SDRAM
    // ------------------------------------------------------------------
    typedef enum logic [2:0] { C_IDLE, C_WAIT, C_SETTLE, C_MEAS } cstate_t;
    cstate_t   cst;
    reg [15:0] cal_ms;
    reg [15:0] fw0;
    reg        fw0_set;
    reg [1:0]  frames;
    reg        bad;

    // Выбор по карте годных точек (бит p — точка p): середина годного участка
    function automatic [1:0] pick(input [3:0] ok);
        case (ok)
            4'b0001: pick = 2'd0;
            4'b0010, 4'b0011, 4'b0110, 4'b0111, 4'b1111, 4'b1010, 4'b1011: pick = 2'd1;
            4'b0100, 4'b0101, 4'b1100, 4'b1110, 4'b1101: pick = 2'd2;
            4'b1000, 4'b1001: pick = 2'd3;
            default: pick = DEF_RD;
        endcase
    endfunction

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            cst <= C_IDLE; cal_busy <= 1'b0; cal_point <= '0; cal_ok <= '0;
            cal_done <= 1'b0; cal_fail <= 1'b0; cfg_rd <= DEF_RD;
            cal_ms <= '0; fw0 <= '0; fw0_set <= 1'b0; frames <= '0; bad <= 1'b0;
        end else begin
            if (cst == C_IDLE && osd_on && cursor == MENU_SD_READ && (k_inc || k_dec) &&
                !k_show && !k_hide)
                cfg_rd <= k_inc ? cfg_rd + 2'd1 : cfg_rd - 2'd1;
            if (cst == C_IDLE && ext_ctrl_we && !ext_ctrl[9])
                cfg_rd <= ext_ctrl[7:6];

            case (cst)
            C_IDLE: if (cal_start) begin
                cst <= C_WAIT; cal_busy <= 1'b1; cal_done <= 1'b0; cal_fail <= 1'b0;
                cal_ok <= '0; cal_point <= '0; cal_ms <= '0; fw0_set <= 1'b0;
            end
            C_WAIT: begin
                // Ждём два опубликованных кадра таблицы
                if (ms) cal_ms <= cal_ms + 16'd1;
                if (pattern_on && !fw0_set) begin fw0 <= frames_written; fw0_set <= 1'b1; end
                if (fw0_set && frames_written - fw0 >= 16'd2) begin
                    cst <= C_SETTLE; cfg_rd <= 2'd0; cal_point <= 2'd0; frames <= '0;
                end else if (cal_ms == CAL_TIMEOUT_MS[15:0]) begin
                    cst <= C_IDLE; cal_busy <= 1'b0; cal_fail <= 1'b1; cal_done <= 1'b1;
                    cfg_rd <= DEF_RD;
                end
            end
            C_SETTLE: if (chk_tick) begin
                // Кадр, в котором сменилась точка, и следующий пропускаются
                if (frames == 2'd1) begin cst <= C_MEAS; frames <= '0; bad <= 1'b0; end
                else frames <= frames + 2'd1;
            end
            C_MEAS: if (chk_tick) begin
                if (frames == 2'd1) begin : point_done
                    logic ok_now;
                    ok_now = !bad && chk_err == 16'd0;
                    cal_ok[cal_point] <= ok_now;
                    if (cal_point == 2'd3) begin
                        cst      <= C_IDLE;
                        cal_busy <= 1'b0;
                        cal_done <= 1'b1;
                        cal_fail <= !(cal_ok[2:0] != 3'd0 || ok_now);
                        cfg_rd   <= pick({ok_now, cal_ok[2:0]});
                    end else begin
                        cal_point <= cal_point + 2'd1;
                        cfg_rd    <= cal_point + 2'd1;
                        cst       <= C_SETTLE;
                        frames    <= '0;
                    end
                end else begin
                    if (chk_err != 16'd0) bad <= 1'b1;
                    frames <= frames + 2'd1;
                end
            end
            default: cst <= C_IDLE;
            endcase
        end
    end
endmodule
