// SPDX-License-Identifier: Apache-2.0
//
// Прошивка Tang Nano 20K для carrier A3.1: видео TS-Config / Base Conf → кадровый буфер →
// HDMI 720p50 (картинка в центре точка в точку) или 576p50 (параметр HD).
//
// ЧЕМ ОТЛИЧАЕТСЯ ОТ ts_hdmi_top (генлок)
//   Генлок выводит строки с той же частотой, что приходят: строчная 31,25 кГц стандартна,
//   а кадровая остаётся 48,83 Гц. Здесь выход тактируется кварцем Tang, и развёртка —
//   настоящий 720×576p50 CEA-861 (27,000 МГц, 864×625, 50,000 Гц). Между входом и
//   выходом — три кадра в SDRAM; примерно раз в 42 кадра один входной кадр
//   показывается дважды. Задержка — до одного кадра больше, чем у генлока.
//   Пропадание PCLK не срывает развёртку: остаётся последний целый кадр, а через секунду
//   без Evo выводится испытательная таблица.
//
// НАЛАДКА НА ЖЕЛЕЗЕ — кнопками Tang S1/S2 через экранное меню (fb_ui):
//   фаза и задержка мультиплексора, поправка окна, точка выборки чтения SDRAM.
//   После включения точка выборки SDRAM калибруется сама по испытательной таблице.
//   Найденные значения переносятся в параметры DEF_* ниже.
//
// Кнопки ПЛИС не сбрасывают: сброс — только по захвату PLL. Если PLL захвата не захватила
// PCLK (Evo включили позже Tang), она перезапускается раз в ~50 мс.
//
module ts_fb_hdmi_top #(
    // Выход: 1 — 1280×720p50 (VIC 19), картинка Evo 720×576 в центре точка в точку:
    // телевизор, растягивающий кадр за края экрана, из неё ничего не обрежет;
    // 0 — 720×576p50 (VIC 17/18), картинка во весь кадр. Ограничения .sdc — для HD = 1.
    parameter bit         HD        = 1'b1,
    parameter logic [1:0] DEF_SRC   = 2'd0,  // 0 AUTO, 1 EVO, 2 TEST
    parameter bit         DEF_PHASE = 1'b1,  // MUX PHASE: 1 — чисто на Rev 0 вместе с MUX CLOCK = B
    parameter int         DEF_DELAY = 1,     // MUX DELAY, 1…4
    parameter logic [3:0] DEF_CLOCK = 4'd11, // MUX CLOCK, 0…15: B — чисто на Rev 0 (2026-10-16)
    parameter logic [1:0] DEF_RD    = 2'd1,  // SD READ, если калибровка не нашла точку
    parameter bit         CALIBRATE = 1'b1
) (
    input  wire        clk27,        // вывод 4: кварц Tang 27 МГц

    // Видео с carrier
    input  wire        pclk_in,      // FPGA80 (GCLKT_0): PCLK Evo, ~28 МГц
    input  wire [7:0]  mux_d,
    input  wire        hsync_in,
    input  wire        vsync_in,
    input  wire        m_sel,
    output wire        mux_sel,

    // Обратная линия FT812: неактивна
    output wire        ft_int_n,

    // LINK к ESP32-S3 (QSPI, ESP — ведущий): регистры настроек и состояния (fb_link)
    input  wire        link_sck,     // FPGA77 (GCLKT_1)
    input  wire        link_cs_n,    // FPGA18, подтяжка R10 на плате
    inout  wire [3:0]  link_io,      // FPGA71, 72, 55, 31
    output tri         link_irq_n,   // FPGA85: не используется

    // SPI-flash Tang (выводы MSPI; после загрузки ПЛИС — обычные, use_mspi_as_gpio):
    // настройки кадрового буфера и мост к flash для ESP (fb_flash)
    output wire        flash_sck,    // FPGA59 MCLK
    output wire        flash_cs_n,   // FPGA60 MCS_N
    output wire        flash_mosi,   // FPGA61 MO
    input  wire        flash_miso,   // FPGA62 MI

    // Бортовое
    input  wire [1:0]  btn,          // S1 (FPGA88), S2 (FPGA87)
    output wire        pa_en,

    // HDMI
    output wire        tmds_clk_p,
    output wire        tmds_clk_n,
    output wire [2:0]  tmds_data_p,
    output wire [2:0]  tmds_data_n,

    // Встроенная SDRAM GW2AR-18 (имена портов фиксированы Gowin, выводы не назначаются)
    output wire        O_sdram_clk,
    output wire        O_sdram_cke,
    output wire        O_sdram_cs_n,
    output wire        O_sdram_cas_n,
    output wire        O_sdram_ras_n,
    output wire        O_sdram_wen_n,
    output wire [3:0]  O_sdram_dqm,
    output wire [10:0] O_sdram_addr,
    output wire [1:0]  O_sdram_ba,
    inout  wire [31:0] IO_sdram_dq
);
    // ------------------------------------------------------------------
    // Такты
    // ------------------------------------------------------------------
    wire clk_serial, clk_mem, pixel_clk, clk_cap;
    wire lock_xtal, lock_cap;
    wire [3:0] cap_clk_phase;          // MUX CLOCK из меню (домен пикселей, меняется редко)
    reg  cap_pll_reset;

    fb_pll_xtal #(.HD(HD)) pll_xtal (
        .clkin(clk27), .reset(1'b0),
        .clk_serial(clk_serial), .clk_mem(clk_mem), .lock(lock_xtal)
    );
    ts_clkdiv5 clkdiv (.hclkin(clk_serial), .resetn(lock_xtal), .clkout(pixel_clk));
    ts_pll_capture pll_capture (
        .clkin(pclk_in), .reset(cap_pll_reset), .phase(cap_clk_phase),
        .clkout(clk_cap), .lock(lock_cap)
    );

    // Домены кварца и захвата сбрасываются независимо: без PCLK вывод работает.
    reg [3:0] rst_mem_sr, rst_pix_sr, rst_cap_sr;
    always_ff @(posedge clk_mem or negedge lock_xtal)
        if (!lock_xtal) rst_mem_sr <= '0; else rst_mem_sr <= {rst_mem_sr[2:0], 1'b1};
    always_ff @(posedge pixel_clk or negedge lock_xtal)
        if (!lock_xtal) rst_pix_sr <= '0; else rst_pix_sr <= {rst_pix_sr[2:0], 1'b1};
    always_ff @(posedge clk_cap or negedge lock_cap)
        if (!lock_cap) rst_cap_sr <= '0; else rst_cap_sr <= {rst_cap_sr[2:0], 1'b1};

    // Перезапуск PLL захвата, пока она без захвата: импульс 1024 такта пикселей (14 мкс
    // при 74,25 МГц) раз в 2^21 тактов (28 мс)
    reg [1:0]  lock_cap_s;
    reg [20:0] cap_wd;
    always_ff @(posedge pixel_clk or negedge rst_pix_sr[3]) begin
        if (!rst_pix_sr[3]) begin
            lock_cap_s <= '0; cap_wd <= '0; cap_pll_reset <= 1'b0;
        end else begin
            lock_cap_s <= {lock_cap_s[0], lock_cap};
            if (lock_cap_s[1]) begin
                cap_wd <= '0; cap_pll_reset <= 1'b0;
            end else begin
                cap_wd <= cap_wd + 21'd1;
                cap_pll_reset <= (cap_wd[20:10] == 11'h7FF);
            end
        end
    end

    // Режим FT812: на линиях RGB не видео — выводим чёрное поле
    reg [1:0] m_sel_sync;
    always_ff @(posedge pixel_clk) m_sel_sync <= {m_sel_sync[0], m_sel};

    // ------------------------------------------------------------------
    // Тракт
    // ------------------------------------------------------------------
    wire [31:0] dq_out;
    wire        dq_oe;
    wire [3:0]  link_io_out;
    wire        link_io_oe;
    wire [9:0]  t_blue, t_green, t_red;

    // Развёртка выхода (CEA-861)
    localparam int OUT_W  = HD ? 1280 : 720;
    localparam int OUT_H  = HD ? 720  : 576;
    localparam int H_FP   = HD ? 440  : 12;
    localparam int H_SYNC = HD ? 40   : 64;
    localparam int H_BP   = HD ? 220  : 68;
    localparam int V_FP   = HD ? 5    : 5;
    localparam int V_SYNC = HD ? 5    : 5;
    localparam int V_BP   = HD ? 20   : 39;
    localparam int PIX_KHZ = HD ? 74_250 : 27_000;
    localparam int MEM_KHZ = HD ? 61_875 : 67_500;

    ts_fb_core #(
        .OUT_W(OUT_W), .OUT_H(OUT_H), .IMG_X((OUT_W - 720) / 2), .IMG_Y((OUT_H - 576) / 2),
        .H_FP(H_FP), .H_SYNC(H_SYNC), .H_BP(H_BP), .V_FP(V_FP), .V_SYNC(V_SYNC), .V_BP(V_BP),
        .HS_NEG(!HD), .VS_NEG(!HD), .PIX_KHZ(PIX_KHZ), .MEM_KHZ(MEM_KHZ),
        .DEF_SRC(DEF_SRC), .DEF_PHASE(DEF_PHASE), .DEF_DELAY(DEF_DELAY), .DEF_CLOCK(DEF_CLOCK),
        .DEF_RD(DEF_RD), .CALIBRATE(CALIBRATE), .SCALE(HD), .HDMI(HD)
    ) core (
        .clk_cap(clk_cap), .rst_cap_n(rst_cap_sr[3]),
        .clk_mem(clk_mem), .rst_mem_n(rst_mem_sr[3]),
        .pixel_clk(pixel_clk), .rst_pix_n(rst_pix_sr[3]),
        .blank(m_sel_sync[1]), .btn(btn), .cap_clk_phase(cap_clk_phase),
        .link_sck(link_sck), .link_cs_n(link_cs_n),
        .link_io_in(link_io), .link_io_out(link_io_out), .link_io_oe(link_io_oe),
        .flash_sck(flash_sck), .flash_cs_n(flash_cs_n), .flash_mosi(flash_mosi),
        .flash_miso(flash_miso),
        .mux_d(mux_d), .hsync_in(hsync_in), .vsync_in(vsync_in), .mux_sel(mux_sel),
        .sdram_cs_n(O_sdram_cs_n), .sdram_ras_n(O_sdram_ras_n), .sdram_cas_n(O_sdram_cas_n),
        .sdram_we_n(O_sdram_wen_n), .sdram_a(O_sdram_addr), .sdram_ba(O_sdram_ba),
        .sdram_dqm(O_sdram_dqm), .sdram_dq_out(dq_out), .sdram_dq_oe(dq_oe),
        .sdram_dq_in(IO_sdram_dq),
        .vid_r(), .vid_g(), .vid_b(), .vid_de(), .vid_hs(), .vid_vs(),
        .tmds_blue(t_blue), .tmds_green(t_green), .tmds_red(t_red),
        .cap_overflow(), .frames_written(), .frames_rejected(), .fetch_late(),
        .have_frame(), .mem_ready(), .raster_locked(), .raster_mode(),
        .raster_line_cycles(), .raster_lines(), .raster_hs_cycles(),
        .aligned_h(), .aligned_v(), .bb_valid(), .bb_x0(), .bb_x1(), .bb_y0(), .bb_y1(),
        .pattern_on(), .chk_err(), .chk_tick()
    );

    assign IO_sdram_dq = dq_oe ? dq_out : 32'hzzzz_zzzz;
    assign link_io     = link_io_oe ? link_io_out : 4'hz;
    assign O_sdram_cke = 1'b1;
    // Клок микросхемы — инверсия клока логики: команды и данные меняются на спаде
    // клока микросхемы и устойчивы на её фронте. Выборку чтения подбирает калибровка.
    assign O_sdram_clk = ~clk_mem;

    tmds_serializer_gowin serializer (
        .pixel_clk(pixel_clk), .serial_clk(clk_serial), .reset(~rst_pix_sr[3]),
        .channel0(t_blue), .channel1(t_green), .channel2(t_red),
        .tmds_clk_p(tmds_clk_p), .tmds_clk_n(tmds_clk_n),
        .tmds_data_p(tmds_data_p), .tmds_data_n(tmds_data_n)
    );

    // FT_INT_N идёт на вход U11 (TXU0101) — держим неактивным уровнем, чтобы вход не плавал.
    // LINK_IRQ_N идёт прямо на GPIO9 ESP: не ведём его вовсе (неактивную единицу держит
    // подтяжка 10 кОм SD DAT1 на Tang) — когда Evo выключена и ESP обесточена, а Tang питается
    // от USB, вывод ПЛИС не тянет ток в обесточенную ESP.
    assign ft_int_n   = 1'b1;
    assign link_irq_n = 1'bz;
    assign pa_en      = 1'b0;
endmodule
