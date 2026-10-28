// SPDX-License-Identifier: Apache-2.0
//
// Прошивка Tang Nano 20K для carrier A3.1: видео TS-Config / Base Conf → кадровый буфер →
// HDMI 720p50 (картинка в центре точка в точку) или 576p50 (параметр HD); ядро FT812 (HAS_EVO = 0)
// — 60 Гц: кадр FT812 идёт 59 Гц, и вывод 50 Гц брал бы из него не больше 50 кадров в секунду;
// с 1.41 — 1366×768 (VESA DMT, сокращённое гашение, 72 МГц), до 1.40 — 720p60.
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
// НАЛАДКА НА ЖЕЛЕЗЕ — меню настроек ESP (оверлей, плагин WC) через регистры LINK (fb_ui):
//   фаза и задержка мультиплексора, поправка окна, точка выборки чтения SDRAM.
//   После включения точка выборки SDRAM калибруется сама по испытательной таблице.
//   Найденные значения переносятся в параметры DEF_* ниже.
//
// Кнопки Tang не используются и ПЛИС не сбрасывают: сброс — только по захвату PLL. Если PLL
// захвата не захватила PCLK (Evo включили позже Tang), она перезапускается раз в ~50 мс.
//
module ts_fb_hdmi_top #(
    // Выход: 1 — 1280×720p50 (VIC 19; у ядра FT812 — 720p60, VIC 4), картинка Evo 720×576 в центре
    // точка в точку:
    // телевизор, растягивающий кадр за края экрана, из неё ничего не обрежет;
    // 0 — 720×576p50 (VIC 17/18), картинка во весь кадр. Ограничения .sdc — для HD = 1.
    parameter bit         HD        = 1'b1,
    parameter logic [1:0] DEF_SRC   = 2'd0,  // 0 AUTO, 1 EVO, 2 TEST
    parameter bit         DEF_PHASE = 1'b1,  // MUX PHASE: 1 — чисто на Rev 0 вместе с MUX CLOCK = B
    parameter int         DEF_DELAY = 1,     // MUX DELAY, 1…4
    parameter logic [3:0] DEF_CLOCK = 4'd11, // MUX CLOCK, 0…15: B — чисто на Rev 0 (2026-10-16)
    parameter logic [1:0] DEF_RD    = 2'd1,  // SD READ, если калибровка не нашла точку
    parameter bit         CALIBRATE = 1'b1,
    // Multiboot (регистр BOOT): слот flash Tang этого битстрима и слот перехода. Сам адрес
    // перехода — опция сборки -multiboot_spi_flash_address; слот 1 собирает ts_fb_slot1_top.
    parameter logic [7:0] BOOT_SLOT = 8'd0,
    parameter logic [7:0] BOOT_NEXT = 8'd1,
    // Состав ядра (ts_fb_core): оба — общее ядро; HAS_FT = 0 — TS-Config (слот 0),
    // HAS_EVO = 0 — FT812 (слот 1). Сборки задают их переменной core_kind скриптов.
    parameter bit         HAS_EVO   = 1'b1,
    parameter bit         HAS_FT    = 1'b1,
    // Ядро видео (HAS_EVO = 0, HAS_FT = 0): MJPEG FTVIEW4 декодирует ПЛИС; 720p50, SDRAM на clk_mem
    parameter bit         HAS_VIDEO = 1'b0
) (
    input  wire        clk27,        // вывод 4: кварц Tang 27 МГц

    // RECONFIG_N (FPGA9, на плате — только контрольная точка TP1): ядро само тянет его к 0,
    // чтобы ПЛИС загрузила битстрим по адресу перехода (use_reconfign_as_gpio, UG290 табл. 4-3)
    output tri         reconfig_n,

    // Видео с carrier
    input  wire        pclk_in,      // FPGA80 (GCLKT_0): PCLK Evo, ~28 МГц
    input  wire [7:0]  mux_d,
    input  wire        hsync_in,
    input  wire        vsync_in,
    input  wire        m_sel,
    output wire        mux_sel,

    // FT812: SPI хоста Evo и прерывание
    output wire        ft_int_n,     // FPGA74 → U11 → IDE15 (при M_SEL = 1)
    input  wire        ft_sck,       // FPGA20: SCK Z-контроллера Evo (общий с SD и ESP)
    input  wire        ft_cs_n,      // FPGA17: выбор FT812 (порт #77 бит 2)
    input  wire        ft_mosi,      // FPGA19
    output wire        ft_miso,      // FPGA79 → U6/U8 → IDE27 (на Tang — DIN WS2812)

    // Сопроцессор FT812 на BL616 модуля Tang (rev 3923; BL616 — ведущий SPI)
    input  wire        cp_sck,       // FPGA13 ← BL616 GPIO1
    input  wire        cp_cs_n,      // FPGA86 ← GPIO0
    input  wire        cp_mosi,      // FPGA76 ← GPIO27
    output wire        cp_miso,      // FPGA75 → GPIO30
    output wire        cp_irq_n,     // FPGA69 → GPIO13: есть работа сопроцессору

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

    // microSD Tang (режим SPI): мост для ESP (fb_sdspi, только TS-Config)
    output wire        sd_cs_n,      // FPGA81 (DAT3)
    output wire        sd_mosi,      // FPGA82 (CMD)
    output wire        sd_sck,       // FPGA83 (CLK)
    input  wire        sd_miso,      // FPGA84 (DAT0)

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
    wire clk_serial, clk_mem, pixel_clk, clk_cap, clk_sd;
    wire clk_mem_x, clk_sd_x;          // делители первой PLL (у ядра FT812 не нужны)
    wire lock_xtal, lock_cap, lock_mem;
    wire [3:0] cap_clk_phase;          // MUX CLOCK из меню (домен пикселей, меняется редко)
    reg  cap_pll_reset;

    // Ядро FT812 (с 1.41) — 1366×768 с сокращённым гашением (VESA DMT, 72 МГц, 60,00 Гц): кадр FT
    // 1024×768 — точка в точку, без масштаба 768 → 720. Ядра с Evo и видео — 720p50, как были.
    localparam bit OUT768 = HD && !HAS_EVO && !HAS_VIDEO;
    fb_pll_xtal #(.HD(HD), .OUT768(OUT768)) pll_xtal (
        .clkin(clk27), .reset(1'b0),
        .clk_serial(clk_serial), .clk_mem(clk_mem_x), .clk_sd(clk_sd_x), .lock(lock_xtal)
    );
    ts_clkdiv5 clkdiv (.hclkin(clk_serial), .resetn(lock_xtal), .clkout(pixel_clk));
    // Вторая PLL: в ядре с Evo — захват (×2 от PCLK), в ядре FT812 (с 1.38) — SDRAM и clk_mem от
    // кварца (fb_pll_sd, SD_FAST ниже); захвата там нет
    wire [3:0] sd_phase;
    wire       sdram_clk_p;
    generate if (HAS_EVO) begin : g_pll_cap
        ts_pll_capture pll_capture (
            .clkin(pclk_in), .reset(cap_pll_reset), .phase(cap_clk_phase),
            .clkout(clk_cap), .lock(lock_cap)
        );
        assign sdram_clk_p = 1'b0;
        assign clk_mem = clk_mem_x;
        assign clk_sd = clk_sd_x;
        assign lock_mem = lock_xtal;
    end else if (HAS_VIDEO) begin : g_no_pll2
        // ядро видео: ни захвата, ни сдвига клока SDRAM (она на clk_mem, как у TS-Config)
        assign clk_cap = 1'b0;
        assign lock_cap = 1'b0;
        assign sdram_clk_p = 1'b0;
        assign clk_mem = clk_mem_x;
        assign clk_sd = clk_sd_x;
        assign lock_mem = lock_xtal;
    end else begin : g_pll_sd
        assign clk_cap = 1'b0;
        assign lock_cap = 1'b0;
        fb_pll_sd pll_sd (.clkin(clk27), .phase(sd_phase), .clk_sd(clk_sd), .clk_mem(clk_mem),
                          .clkoutp(sdram_clk_p), .lock(lock_mem));
    end endgenerate

    // Домены кварца и захвата сбрасываются независимо: без PCLK вывод работает.
    reg [3:0] rst_mem_sr, rst_pix_sr, rst_cap_sr;
    always_ff @(posedge clk_mem or negedge lock_mem)
        if (!lock_mem) rst_mem_sr <= '0; else rst_mem_sr <= {rst_mem_sr[2:0], 1'b1};
    // SDRAM ядра FT812 на 2 × clk_mem (ts_fb_core SD_FAST, ядра 1.24–1.26) на плате читалась с
    // ошибками: клок микросхемы был инверсией клока логики, а окно подбиралось только точкой
    // выборки с шагом полтакта (4 нс) — линии DQ17 и DQ19 не попадали ни в одну (20.10.2026,
    // чистый замер: SD READ 2 — 2/16 слов неверно). 1.27–1.32 — на clk_mem (61,875 МГц). 1.33–1.34 —
    // снова 123,75 МГц, клок микросхемы — CLKOUTP ещё одной PLL, которую питал clk_sd (к сдвигу
    // добавлялась неизвестная задержка подвода clk_sd; сдвиг — вручную; на плате сбоила). С 1.38 —
    // как у ядер ZX Next и NES на этой же SDRAM: clk_sd (121,5 МГц), clk_mem и клок микросхемы —
    // выходы одной PLL от кварца (fb_pll_sd), DQ берёт IDDR у вывода, сдвиг и точку выборки
    // подбирает само ядро после включения (sdram_train). Ядро с Evo (TS-Config) — по-прежнему на
    // clk_mem. Ядро видео — на clk_mem: шины ему хватает, а этот путь на плате проверен
    localparam bit SD_FAST = !HAS_EVO && !HAS_VIDEO;
    reg [3:0] rst_sd_sr;
    generate if (SD_FAST) begin : g_rst_sd
        always_ff @(posedge clk_sd or negedge lock_mem)
            if (!lock_mem) rst_sd_sr <= '0; else rst_sd_sr <= {rst_sd_sr[2:0], 1'b1};
    end else begin : g_no_rst_sd
        always_comb rst_sd_sr = '0;
    end endgenerate
    always_ff @(posedge pixel_clk or negedge lock_xtal)
        if (!lock_xtal) rst_pix_sr <= '0; else rst_pix_sr <= {rst_pix_sr[2:0], 1'b1};
    always_ff @(posedge clk_cap or negedge lock_cap)
        if (!lock_cap) rst_cap_sr <= '0; else rst_cap_sr <= {rst_cap_sr[2:0], 1'b1};

    // Перезапуск PLL захвата (ядро с Evo), пока она без захвата: импульс 1024 такта пикселей
    // (14 мкс при 74,25 МГц) раз в 2^21 тактов (28 мс)
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
    wire [31:0] dq_in;                  // DQ чтения — прямо с выводов
    wire [2:0]  sd_dly;                 // не используется (задержки DQ с 1.38 нет)
    wire [3:0]  link_io_out;
    wire        link_io_oe;
    wire [9:0]  t_blue, t_green, t_red;
    wire        boot_req;

    // Развёртка выхода (CEA-861; у ядра FT812 — VESA DMT)
    localparam int OUT_W  = OUT768 ? 1366 : HD ? 1280 : 720;
    localparam int OUT_H  = OUT768 ? 768  : HD ? 720  : 576;
    // 720p50 — 74,25 МГц, 1980 тактов в строке. Ядро FT812 — 60 Гц: FT812 рисует 1024×768@59
    // (16,93 мс), и кадр берётся выводом раз в кадр развёртки; с 1.41 — 1366×768 RB: строка
    // 1366 + 14 + 56 + 64 = 1500, кадр 768 + 1 + 3 + 28 = 800, 72 МГц (до 1.40 — 720p60, 1650 × 750).
    localparam int H_FP   = OUT768 ? 14 : HD ? 440 : 12;
    localparam int H_SYNC = OUT768 ? 56 : HD ? 40  : 64;
    localparam int H_BP   = OUT768 ? 64 : HD ? 220 : 68;
    localparam int V_FP   = OUT768 ? 1  : 5;
    localparam int V_SYNC = OUT768 ? 3  : 5;
    localparam int V_BP   = OUT768 ? 28 : HD ? 20  : 39;
    localparam int PIX_KHZ = OUT768 ? 72_000 : HD ? 74_250 : 27_000;
    localparam int MEM_KHZ = SD_FAST ? 60_750 : HD ? 61_875 : 67_500;   // FT812 — fb_pll_sd ÷2

    ts_fb_core #(
        .OUT_W(OUT_W), .OUT_H(OUT_H), .IMG_X((OUT_W - 720) / 2), .IMG_Y((OUT_H - 576) / 2),
        .H_FP(H_FP), .H_SYNC(H_SYNC), .H_BP(H_BP), .V_FP(V_FP), .V_SYNC(V_SYNC), .V_BP(V_BP),
        .HS_NEG(!HD), .VS_NEG(!HD), .PIX_KHZ(PIX_KHZ), .MEM_KHZ(MEM_KHZ),
        .DEF_SRC(DEF_SRC), .DEF_PHASE(DEF_PHASE), .DEF_DELAY(DEF_DELAY), .DEF_CLOCK(DEF_CLOCK),
        .DEF_RD(DEF_RD), .CALIBRATE(CALIBRATE), .SCALE(HD), .OUT768(OUT768), .HDMI(HD),
        .BOOT_SLOT(BOOT_SLOT), .BOOT_NEXT(BOOT_NEXT), .HAS_EVO(HAS_EVO), .HAS_FT(HAS_FT),
        .HAS_VIDEO(HAS_VIDEO), .SD_FAST(SD_FAST)
    ) core (
        .clk_cap(clk_cap), .rst_cap_n(rst_cap_sr[3]),
        .clk_mem(clk_mem), .rst_mem_n(rst_mem_sr[3]),
        .pixel_clk(pixel_clk), .rst_pix_n(rst_pix_sr[3]),
        .clk_sd(clk_sd), .rst_sd_n(rst_sd_sr[3]),
        .blank(m_sel_sync[1]), .btn(btn), .boot_req(boot_req), .cap_clk_phase(cap_clk_phase),
        .link_sck(link_sck), .link_cs_n(link_cs_n),
        .link_io_in(link_io), .link_io_out(link_io_out), .link_io_oe(link_io_oe),
        .ft_sck(ft_sck), .ft_cs_n(ft_cs_n), .ft_mosi(ft_mosi), .ft_miso(ft_miso),
        .ft_int_n(ft_int_n),
        .cp_sck(cp_sck), .cp_cs_n(cp_cs_n), .cp_mosi(cp_mosi), .cp_miso(cp_miso),
        .cp_irq_n(cp_irq_n),
        .flash_sck(flash_sck), .flash_cs_n(flash_cs_n), .flash_mosi(flash_mosi),
        .flash_miso(flash_miso),
        .sd_cs_n(sd_cs_n), .sd_sck(sd_sck), .sd_mosi(sd_mosi), .sd_miso(sd_miso),
        .mux_d(mux_d), .hsync_in(hsync_in), .vsync_in(vsync_in), .mux_sel(mux_sel),
        .sdram_cs_n(O_sdram_cs_n), .sdram_ras_n(O_sdram_ras_n), .sdram_cas_n(O_sdram_cas_n),
        .sdram_we_n(O_sdram_wen_n), .sdram_a(O_sdram_addr), .sdram_ba(O_sdram_ba),
        .sdram_dqm(O_sdram_dqm), .sdram_dq_out(dq_out), .sdram_dq_oe(dq_oe),
        .sdram_dq_in(dq_in), .sdram_dly(sd_dly), .sdram_dly_df(1'b0), .sdram_phase(sd_phase),
        .vid_r(), .vid_g(), .vid_b(), .vid_de(), .vid_hs(), .vid_vs(),
        .tmds_blue(t_blue), .tmds_green(t_green), .tmds_red(t_red),
        .cap_overflow(), .frames_written(), .frames_rejected(), .fetch_late(),
        .have_frame(), .mem_ready(), .raster_locked(), .raster_mode(),
        .raster_line_cycles(), .raster_lines(), .raster_hs_cycles(),
        .aligned_h(), .aligned_v(), .bb_valid(), .bb_x0(), .bb_x1(), .bb_y0(), .bb_y1(),
        .pattern_on(), .chk_err(), .chk_tick()
    );

    assign IO_sdram_dq = dq_oe ? dq_out : 32'hzzzz_zzzz;
    // Чтение DQ — прямо с выводов: у ядра FT812 его берёт IDDR у вывода (sdram_burst IOB_CAP);
    // IODELAYA 1.33–1.34 нет — тонкую точку выборки задаёт сдвиг клока микросхемы
    assign dq_in = IO_sdram_dq;
    assign link_io     = link_io_oe ? link_io_out : 4'hz;
    assign O_sdram_cke = 1'b1;
    // Клок микросхемы: у TS-Config — инверсия клока логики (команды и данные меняются на спаде
    // клока микросхемы и устойчивы на её фронте); у ядра FT812 — CLKOUTP fb_pll_sd со сдвигом,
    // который после включения подбирает sdram_train (ts_fb_core).
    assign O_sdram_clk = SD_FAST ? sdram_clk_p : ~clk_mem;

    tmds_serializer_gowin serializer (
        .pixel_clk(pixel_clk), .serial_clk(clk_serial), .reset(~rst_pix_sr[3]),
        .channel0(t_blue), .channel1(t_green), .channel2(t_red),
        .tmds_clk_p(tmds_clk_p), .tmds_clk_n(tmds_clk_n),
        .tmds_data_p(tmds_data_p), .tmds_data_n(tmds_data_n)
    );

    // LINK_IRQ_N идёт прямо на GPIO9 ESP: не ведём его вовсе (неактивную единицу держит
    // подтяжка 10 кОм SD DAT1 на Tang) — когда Evo выключена и ESP обесточена, а Tang питается
    // от USB, вывод ПЛИС не тянет ток в обесточенную ESP.
    assign link_irq_n = 1'bz;
    assign pa_en      = 1'b0;
    // Открытый сток: в покое Z (внутренняя подтяжка ПЛИС держит 1), по BOOT — 0
    assign reconfig_n = boot_req ? 1'b0 : 1'bz;
endmodule
