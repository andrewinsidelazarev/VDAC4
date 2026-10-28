// SPDX-License-Identifier: Apache-2.0
//
// Настройки и калибровка выборки SDRAM. Домен пиксельного клока.
//
// Меню рисует ESP (оверлей, fb_ovl) и ведёт его плагин WC с клавиатуры Evo; кнопки Tang
// (S1, S2) меню не нужны и прошивкой не читаются.
//
// НАСТРОЙКИ (меняет ESP по LINK — регистры CTRL, ADJUST, ADJ_TAB):
//   MUX PHASE   — с какой фазы мультиплексора начинается строка (0/1);
//   MUX DELAY   — задержка выборки мультиплексора, 1…4 такта 56 МГц;
//   MUX CLOCK   — фаза клока приёма 0…15 (1/16 периода 56 МГц, ts_pll_capture);
//   H/V ADJUST  — поправка окна захвата, −64…+63; своя для каждого растра (mode
//                 raster_detect, 0…7): при смене растра действует его поправка;
//   AUTO ALIGN  — автоподстройка окна по границам картинки;
//   SD READ     — точка выборки чтения SDRAM 0…3 (sdram_burst), калибровка заново — CTRL[9];
//   ASPECT      — 4:3 / 16:9; SIZE — 100 / 95 / 90 % высоты (fb_scanout, SCALE);
//   FRAME BLEND — выводить среднее двух последних кадров Evo (fb_core, AVG): мерцание демо
//                 (кадры A/B через один) становится ровным цветом;
//   SOURCE      — AUTO (таблица, пока нет Evo) / EVO / TEST.
// После сброса действуют DEF_*, через ~2 мс их заменяют настройки из flash Tang (fb_flash,
// ext_load_*), если они там есть: MUX PHASE/DELAY/CLOCK, AUTO ALIGN, ASPECT, SIZE, FRAME BLEND
// и поправки всех растров.
// Источник и SD READ не сохраняются: источник после включения всегда DEF_SRC, SD READ
// калибруется заново. Записывает настройки во flash ESP (по LINK).
//
// КАЛИБРОВКА SD READ (после сброса, если CALIBRATE=1, и по CTRL[9]). На время калибровки
// источник — испытательная таблица. Когда опубликованы два кадра таблицы, для каждой точки
// выборки 0…3 выжидаются два кадра и берутся ошибки сверки двух следующих (fb_scanout).
// Точка годится, если ошибок нет. Из годных выбирается середина: при двух соседних —
// та, что ближе к 1–2. Если годных нет или таблица не появилась за CAL_TIMEOUT_MS —
// остаётся DEF_RD и cal_fail = 1 (STATUS).
//
module fb_ui #(
    parameter int TICK           = 27_000,   // тактов на 1 мс
    parameter int CAL_TIMEOUT_MS = 3_000,
    parameter logic [1:0] DEF_SRC   = 2'd0,  // 0 AUTO, 1 EVO, 2 TEST
    parameter bit         DEF_PHASE = 1'b1,
    parameter logic [1:0] DEF_DELAY = 2'd0,  // задержка выборки мультиплексора − 1
    parameter logic [3:0] DEF_CLOCK = 4'd0,  // фаза клока приёма
    parameter logic [1:0] DEF_RD    = 2'd1,
    parameter bit         DEF_ALIGN = 1'b1,
    parameter bit         DEF_ASPECT = 1'b0, // 0 — 4:3, 1 — 16:9
    parameter logic [1:0] DEF_SIZE  = 2'd0,  // 0 — 100 %, 1 — 95 %, 2 — 90 %
    parameter bit         DEF_AVG   = 1'b0,  // FRAME BLEND
    parameter bit         CALIBRATE = 1'b1,
    // 0 — калибровки SD READ нет совсем, и по CTRL[9] тоже (ядро FT812 с 1.38: точку выборки
    // подбирает sdram_train, SD READ ему не нужен) — синтез убирает автомат калибровки
    parameter bit         HAS_CAL = 1'b1
) (
    input  wire              clk,
    input  wire              rst_n,

    // Состояние
    input  wire              chk_tick,       // конец активной части выходного кадра
    input  wire [15:0]       chk_err,        // ошибки сверки таблицы за этот кадр
    input  wire              pattern_on,     // SDRAM сейчас получает таблицу
    input  wire [15:0]       frames_written,

    // Запись настроек от ESP по LINK (регистры CTRL и ADJUST, см. ts_fb_core)
    input  wire              ext_ctrl_we,
    input  wire              ext_src_auto,   // сброс Evo (CORERST к FT812): источник TEST → AUTO
    input  wire [17:0]       ext_ctrl,       // {avg, size[1:0], aspect, фаза клока[3:0], калибровка, меню, rd[1:0], align, delay[1:0], phase, src[1:0]}
    input  wire              ext_adj_we,     // поправка текущего растра
    input  wire signed [7:0] ext_h_adj,
    input  wire signed [7:0] ext_v_adj,
    input  wire              ext_tab_we,     // поправка растра ext_tab_idx
    input  wire [2:0]        ext_tab_idx,
    input  wire [15:0]       ext_tab_data,   // {V, H}
    input  wire              ext_load_stb,   // настройки из flash
    input  wire [11:0]       ext_load_cfg,   // {avg, size[1:0], aspect, align, phase, delay[1:0], clock[3:0]}
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
    output reg               cfg_avg,        // FRAME BLEND
    output reg  [1:0]        cfg_rd,

    // Калибровка
    output reg               cal_busy,
    output reg  [1:0]        cal_point,
    output reg  [3:0]        cal_ok,
    output reg               cal_done,
    output reg               cal_fail
);
    localparam [16:0] TICK_LAST = TICK - 1;
    localparam [1:0]  SRC_TEST  = 2'd2;

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
    // Настройки: от ESP (LINK) и из flash
    // ------------------------------------------------------------------
    reg cal_start;

    // Поправки окна по растрам
    reg signed [7:0] tab_h [0:7];
    reg signed [7:0] tab_v [0:7];
    assign cfg_h_adj = tab_h[mode];
    assign cfg_v_adj = tab_v[mode];
    genvar gm;
    generate for (gm = 0; gm < 8; gm = gm + 1) begin : g_tab
        assign adj_tab[gm*16 +: 16] = {tab_v[gm], tab_h[gm]};
    end endgenerate

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
            cfg_avg   <= DEF_AVG;
            cal_start <= CALIBRATE && HAS_CAL;
        end else begin
            cal_start <= 1'b0;
            if (ext_ctrl_we) begin
                set_src   <= (ext_ctrl[1:0] == 2'd3) ? 2'd0 : ext_ctrl[1:0];
                cfg_phase <= ext_ctrl[2];
                cfg_delay <= ext_ctrl[4:3];
                cfg_clock <= ext_ctrl[13:10];
                cfg_align <= ext_ctrl[5];
                cfg_aspect <= ext_ctrl[14];
                cfg_size   <= (ext_ctrl[16:15] == 2'd3) ? 2'd0 : ext_ctrl[16:15];
                cfg_avg    <= ext_ctrl[17];
                cal_start <= ext_ctrl[9] && HAS_CAL;
            end
            // Сброс Evo — выход из испытательной таблицы к картинке TS-Config (Tang при сбросе
            // больше не перезагружается, а источник не хранится и после загрузки — AUTO)
            if (ext_src_auto && set_src == 2'd2)
                set_src <= 2'd0;
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
                cfg_avg    <= ext_load_cfg[11];
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
