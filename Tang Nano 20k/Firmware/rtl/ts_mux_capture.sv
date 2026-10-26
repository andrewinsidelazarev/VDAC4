// SPDX-License-Identifier: Apache-2.0
//
// Приём видео TS-Config через мультиплексоры 2×8 платы A3.1.
//
// Что приходит на carrier (docs/a3-interface-pinmap.md):
//   U1/U2 — два SN74LVC257A, они же переводят 5 В → 3,3 В. Общий вход выбора SEL
//   приходит от ПЛИС. За один пиксель ПЛИС обязана прочитать обе фазы:
//
//     SEL=0 (фаза A): D0=R0 D1=R1 D2=R2 D3=R3 D4=R4 D5=G0 D6=G1 D7=G2
//     SEL=1 (фаза B): D0=B0 D1=B1 D2=B2 D3=B3 D4=B4 D5=G4 D6=PAL_SEL D7=G3
//
//   HSYNC/VSYNC идут отдельными линиями через U3 и от SEL не зависят.
//   PCLK (pin 36 IDE, он же !fclk основной ПЛИС Evo) номинально 28 МГц — это и есть
//   частота смены пикселя, поэтому фазы чередуются с частотой 56 МГц, а сам SEL
//   является меандром 28 МГц.
//
// Как устроен приём здесь.
//   Модуль работает на clk_cap = 56 МГц (PLL ×2 от входного PCLK), то есть ровно два
//   такта на пиксель. SEL переключается каждый такт. Данные с мультиплексора
//   действительны не сразу: к задержке 74LVC257 добавляются шлейф IDE, дорожки и
//   входной буфер ПЛИС. Поэтому выборка берётся на sample_delay+1 тактов позже, чем
//   был выставлен соответствующий уровень SEL, а какому уровню SEL принадлежит
//   пришедшая выборка, отслеживает сдвиговый регистр sel_hist.
//
//   ВАЖНО: конкретное значение задержки и фаза PLL проверяются на железе.
//   В проекте запас setup/hold пока не измерен (docs/tsconfig-tang20k-consolidated-notes.md,
//   раздел 10.2), поэтому оба значения — входы: у генлока это параметры верхнего модуля,
//   у кадрового буфера их меняет меню настроек ESP по LINK (CTRL, fb_ui).
//
module ts_mux_capture (
    input  wire       clk_cap,    // 56 МГц, синхронно с входным PCLK
    input  wire       rst_n,

    // На сколько тактов 56 МГц выборка отстаёт от смены SEL: sample_delay + 1 (1…4).
    // 1 такт = 17,86 нс при 56 МГц: этого хватает на задержку мультиплексора и шлейфа.
    input  wire [1:0] sample_delay,
    // С какой фазы начинается строка. Делитель PLL стартует с произвольной
    // чётности, поэтому «какая половина такта PCLK — фаза A» заранее неизвестно.
    // Раз в строку, по фронту HSYNC, чередование принудительно выравнивается на
    // это значение. Если на экране цвет размазан на полпикселя (R от одного
    // пикселя, B от соседнего) — значение надо инвертировать.
    // 1 — то, при котором сходится симуляция тракта (tb_ts_video_chain). На железе фаза
    // делителя PLL может встать иначе: признак — размазанный по горизонтали цвет.
    input  wire       sel_phase,

    input  wire [7:0] mux_d,      // выходы U1/U2 (уже 3,3 В)
    input  wire       hsync_in,   // HS через U3
    input  wire       vsync_in,   // VS через U3
    // Полярность синхроимпульсов (1 — активен низкий уровень). У генлок-тракта это
    // константы, у кадрового буфера их определяет raster_detect по длительностям уровней.
    input  wire       hs_active_low,
    input  wire       vs_active_low,

    output wire       mux_sel,    // меандр 28 МГц на вывод 1 U1/U2

    // Собранный пиксель в домене clk_cap: действителен один такт из двух
    output reg        pix_valid,
    output reg  [4:0] pix_r,
    output reg  [4:0] pix_g,
    output reg  [4:0] pix_b,
    output reg        pix_pal,

    // Нормализованные (активный уровень = 1) и синхронизированные импульсы
    output reg        hs_active,
    output reg        vs_active,
    output reg        hs_start,   // строб начала строки (фронт активного уровня)
    output reg        vs_start,   // строб начала кадра
    // Синхронизированные уровни как есть, до учёта полярности
    output wire       hs_level,
    output wire       vs_level
);

    // ------------------------------------------------------------------
    // Генерация SEL. Меандр 28 МГц: переключаем каждый такт 56 МГц.
    // sel_drive — то, что уходит наружу; sel_hist хранит историю, чтобы знать,
    // какой фазе принадлежит выборка, пришедшая через sample_delay+1 тактов.
    // ------------------------------------------------------------------
    reg sel_drive;
    reg [3:0] sel_hist;

    // Признак начала строки во внутреннем виде: нужен на такт раньше выходного hs_start,
    // потому что по нему выравнивается чередование фаз SEL.
    wire hs_start_int;

    always_ff @(posedge clk_cap or negedge rst_n) begin
        if (!rst_n) begin
            sel_drive <= 1'b0;
            sel_hist  <= '0;
        end else begin
            // Начало строки: восстанавливаем известную чётность фаз.
            sel_drive <= hs_start_int ? sel_phase : ~sel_drive;
            sel_hist  <= {sel_hist[2:0], sel_drive};
        end
    end

    assign mux_sel = sel_drive;

    // Фаза, которой принадлежит текущая выборка данных.
    wire sample_phase = sel_hist[sample_delay];

    // ------------------------------------------------------------------
    // Входные регистры. Один такт на защёлкивание шины и синхронизацию HS/VS.
    // Для HS/VS двухступенчатая синхронизация: они приходят из чужого домена
    // (тактируются fclk основной ПЛИС Evo), а мы работаем от PLL, привязанного
    // к PCLK — частота общая, но фаза относительно данных не гарантирована.
    // ------------------------------------------------------------------
    reg [7:0] mux_q;
    reg       hs_meta, hs_sync, hs_prev;
    reg       vs_meta, vs_sync, vs_prev;

    wire hs_norm = hs_active_low ? ~hs_sync : hs_sync;
    wire vs_norm = vs_active_low ? ~vs_sync : vs_sync;
    assign hs_level = hs_sync;
    assign vs_level = vs_sync;

    assign hs_start_int = hs_norm & ~hs_prev;

    always_ff @(posedge clk_cap or negedge rst_n) begin
        if (!rst_n) begin
            mux_q   <= '0;
            hs_meta <= 1'b0; hs_sync <= 1'b0; hs_prev <= 1'b0;
            vs_meta <= 1'b0; vs_sync <= 1'b0; vs_prev <= 1'b0;
        end else begin
            mux_q   <= mux_d;
            hs_meta <= hsync_in; hs_sync <= hs_meta; hs_prev <= hs_norm;
            vs_meta <= vsync_in; vs_sync <= vs_meta; vs_prev <= vs_norm;
        end
    end

    // ------------------------------------------------------------------
    // Сборка пикселя. Фаза A запоминается, по приходу фазы B выдаётся пиксель.
    // Раскладка бит — ровно из таблицы docs/a3-interface-pinmap.md.
    // ------------------------------------------------------------------
    reg [7:0] phase_a;
    reg       phase_a_loaded;

    always_ff @(posedge clk_cap or negedge rst_n) begin
        if (!rst_n) begin
            phase_a        <= '0;
            phase_a_loaded <= 1'b0;
            pix_valid      <= 1'b0;
            pix_r          <= '0;
            pix_g          <= '0;
            pix_b          <= '0;
            pix_pal        <= 1'b0;
            hs_active      <= 1'b0;
            vs_active      <= 1'b0;
            hs_start       <= 1'b0;
            vs_start       <= 1'b0;
        end else begin
            pix_valid <= 1'b0;
            hs_active <= hs_norm;
            vs_active <= vs_norm;
            hs_start  <= hs_norm & ~hs_prev;
            vs_start  <= vs_norm & ~vs_prev;

            if (hs_start_int) begin
                // Новая строка: незавершённая половинка пикселя отбрасывается.
                phase_a_loaded <= 1'b0;
            end else if (sample_phase == 1'b0) begin
                // Фаза A: младшие пять бит — R, старшие три — G0..G2
                phase_a        <= mux_q;
                phase_a_loaded <= 1'b1;
            end else if (phase_a_loaded) begin
                // Фаза B: B0..B4, G4, PAL_SEL, G3
                pix_r     <= phase_a[4:0];
                pix_g     <= {mux_q[5], mux_q[7], phase_a[7:5]}; // G4 G3 G2 G1 G0
                pix_b     <= mux_q[4:0];
                pix_pal   <= mux_q[6];
                pix_valid <= 1'b1;
            end
        end
    end

endmodule
