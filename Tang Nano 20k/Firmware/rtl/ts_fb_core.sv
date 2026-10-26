// SPDX-License-Identifier: Apache-2.0
//
// Видеотракт TS-Config → кадровый буфер SDRAM → стандартная развёртка → слова TMDS.
// Всё, кроме PLL, сериализатора и выводов, — чтобы моделировалось без примитивов Gowin.
//
// Три независимых домена:
//   clk_cap   56 МГц — от PCLK Evo (PLL ×2): приём двух фаз мультиплексора;
//   clk_mem   67,5 МГц — от кварца 27 МГц: SDRAM, запись и выборка кадров;
//   pixel_clk 27 МГц — от кварца: развёртка 576p50, TMDS, кнопки и экранное меню.
// Между ними: асинхронный FIFO (захват → SDRAM), переключатель запроса строки
// (вывод → SDRAM), двухклоковый строчный буфер (SDRAM → вывод), синхронизаторы
// настроек (fb_cdc: меню → захват и SDRAM) и состояния (захват и SDRAM → меню).
//
// Если PCLK Evo пропал, выход продолжает идти от кварца и показывает последний
// целый кадр: монитор не теряет синхронизацию. Если Evo нет дольше NOSIG_CYCLES
// (около секунды) и источник AUTO, в SDRAM вместо картинки Evo пишется испытательная
// таблица (fb_pattern) — по ней проверяются HDMI и SDRAM без Evo.
//
// Наладка — меню ESP (рисует его в оверлей fb_ovl, ведёт плагин WC с клавиатуры Evo; кнопки
// Tang не используются): фаза и задержка мультиплексора, поправка окна, точка выборки
// SDRAM, источник. После сброса калибруется точка выборки чтения SDRAM (CALIBRATE).
// Настройки и всё состояние доступны ESP по LINK (fb_link) — карта регистров ниже, у
// мультиплексора чтения.
// Настройки хранятся во flash Tang (fb_flash): ядро само загружает их после сброса,
// а пишет их туда ESP через мост к flash — по LINK.
//
module ts_fb_core #(
    parameter bit TEST_PROFILES = 1'b0,    // профили уменьшенных растров тестбенча
    // Размер картинки (кадр Evo; масштабирования нет)
    parameter int H_ACTIVE = 720,
    parameter int V_ACTIVE = 576,
    // Выходная развёртка, по умолчанию 576p50; картинка — окном с (IMG_X, IMG_Y)
    parameter int OUT_W = H_ACTIVE, parameter int OUT_H = V_ACTIVE,
    parameter int IMG_X = 0,        parameter int IMG_Y = 0,
    parameter int H_FP = 12, parameter int H_SYNC = 64, parameter int H_BP = 68,
    parameter int V_FP = 5,  parameter int V_SYNC = 5,  parameter int V_BP = 39,
    parameter bit HS_NEG = 1'b1,    parameter bit VS_NEG = 1'b1,
    // SDRAM
    parameter int MEM_KHZ = 67_500,
    parameter int SDRAM_INIT_US = 200,
    // Источник: таблица вместо Evo, если растра нет NOSIG_CYCLES тактов clk_mem (1 с)
    parameter int NOSIG_CYCLES   = MEM_KHZ * 1000,
    parameter int PATTERN_PERIOD = MEM_KHZ * 20,     // новый кадр таблицы — раз в 20 мс
    // Кнопки и калибровка (такты пиксельного клока на 1 мс и пр. — см. fb_ui)
    parameter int PIX_KHZ        = 27_000,
    parameter int CAL_TIMEOUT_MS = 3_000,
    // Значения по умолчанию (до настроек из flash)
    parameter logic [1:0] DEF_SRC   = 2'd0,  // 0 AUTO, 1 EVO, 2 TEST
    parameter bit         DEF_PHASE = 1'b1,  // фаза мультиплексора в начале строки
    parameter int         DEF_DELAY = 1,     // задержка выборки мультиплексора, такты 56 МГц (1…4)
    parameter logic [3:0] DEF_CLOCK = 4'd0,  // фаза клока приёма, 1/16 периода 56 МГц
    parameter logic [1:0] DEF_RD    = 2'd1,  // точка выборки чтения SDRAM (sdram_burst)
    parameter bit         DEF_ALIGN = 1'b1,
    parameter bit         DEF_ASPECT = 1'b0, // 0 — 4:3, 1 — 16:9 (только SCALE)
    parameter logic [1:0] DEF_SIZE  = 2'd0,  // высота картинки: 0 — 100 %, 1 — 95 %, 2 — 90 %
    parameter bit         DEF_AVG   = 1'b0,  // FRAME BLEND (только ядро с Evo без FT812)
    parameter bit         CALIBRATE = 1'b1,
    // Картинка на всю высоту кадра 1280×720 (fb_scanout); 0 — точка в точку с (IMG_X, IMG_Y)
    parameter bit         SCALE = 1'b0,
    // Выход HDMI с пакетом AVI InfoFrame (hdmi_tx); 0 — DVI
    parameter bit         HDMI = 1'b0,
    // Flash Tang: сектор настроек и ожидание перед их чтением после сброса
    parameter logic [23:0] FLASH_SET_ADDR = 24'h7F_F000,
    parameter int          FLASH_BOOT_US  = 1_000,
    // Multiboot: слот flash Tang (1 МБ) этого битстрима и слот, который грузит RECONFIG_N
    // (адрес перехода в заголовке битстрима — опция сборки, scripts/gowin_fb*.tcl); только
    // для чтения регистром BOOT
    parameter logic [7:0]  BOOT_SLOT = 8'd0,
    parameter logic [7:0]  BOOT_NEXT = 8'd1,
    // Состав ядра (multiboot, docs/ft812-emulation.md, шаг 7): HAS_EVO — захват Evo
    // (TS-Config), HAS_FT — эмулятор FT812. Оба — общее ядро (до 1.11; тестбенч); только Evo —
    // ядро слота 0, оно уходит в слот FT812 по первому обмену Evo с FT812, если ESP разрешила
    // (BOOT «ARM1»); только FT812 — ядро слота 1, оно возвращается в слот 0, когда M_SEL = 0 и
    // обмена с FT812 нет SWITCH_IDLE_MS
    parameter bit          HAS_EVO = 1'b1,
    parameter bit          HAS_FT  = 1'b1,
    parameter int          SWITCH_IDLE_MS = 500,    // с 1.22; было 3000 (возврат ~6 с вместе с загрузкой)
    // Контроллер SDRAM на clk_sd = 2 × clk_mem (123,75 МГц, CL 3) за мостом sd_bridge (ядро FT812,
    // с 1.24): вывод 720p60 и выгрузка кадра FT держат шину вдвое меньше. 0 — контроллер на clk_mem
    parameter bit          SD_FAST = 1'b0
) (
    input  wire        clk_cap,
    input  wire        rst_cap_n,
    input  wire        clk_mem,
    input  wire        rst_mem_n,
    input  wire        pixel_clk,
    input  wire        rst_pix_n,
    input  wire        clk_sd,         // 2 × clk_mem, тот же PLL (только SD_FAST)
    input  wire        rst_sd_n,
    input  wire        blank,          // домен pixel_clk: 1 — чёрное поле (режим FT812)
    input  wire [1:0]  btn,            // кнопки S1, S2 — не используются (меню ведёт плагин WC)
    output reg         boot_req,       // 1 — перезагрузить ПЛИС из flash (RECONFIG_N к 0)
    output wire [3:0]  cap_clk_phase,  // домен пикселей: фаза PLL захвата (MUX CLOCK)

    // LINK к ESP (QSPI, ESP — ведущий)
    input  wire        link_sck,
    input  wire        link_cs_n,
    input  wire [3:0]  link_io_in,
    output wire [3:0]  link_io_out,
    output wire        link_io_oe,

    // FT812: SPI хоста Evo (порт #77 бит 2) и прерывание
    input  wire        ft_sck,
    input  wire        ft_cs_n,
    input  wire        ft_mosi,
    output wire        ft_miso,
    output wire        ft_int_n,

    // Сопроцессор FT812 на BL616 (SPI, BL616 — ведущий) и его прерывание
    input  wire        cp_sck,
    input  wire        cp_cs_n,
    input  wire        cp_mosi,
    output wire        cp_miso,
    output wire        cp_irq_n,

    // SPI-flash Tang (выводы MSPI)
    output wire        flash_sck,
    output wire        flash_cs_n,
    output wire        flash_mosi,
    input  wire        flash_miso,

    // microSD Tang (режим SPI): мост для ESP (fb_sdspi), только в ядре с Evo
    output wire        sd_cs_n,
    output wire        sd_sck,
    output wire        sd_mosi,
    input  wire        sd_miso,

    // Видео с carrier
    input  wire [7:0]  mux_d,
    input  wire        hsync_in,
    input  wire        vsync_in,
    output wire        mux_sel,

    // SDRAM (DQ разделена: в верхнем модуле — на двунаправленный вывод)
    output wire        sdram_cs_n,
    output wire        sdram_ras_n,
    output wire        sdram_cas_n,
    output wire        sdram_we_n,
    output wire [10:0] sdram_a,
    output wire [1:0]  sdram_ba,
    output wire [3:0]  sdram_dqm,
    output wire [31:0] sdram_dq_out,
    output wire        sdram_dq_oe,
    input  wire [31:0] sdram_dq_in,
    // тонкая задержка чтения DQ (IODELAYA в верхнем модуле, только SD_FAST): {SDTAP, SETN, VALUE}
    output wire [2:0]  sdram_dly,
    input  wire        sdram_dly_df,    // DF задержки DQ0 (переполнение или исчерпание)

    // Выход: и цвет (для отладки/теста), и готовые слова TMDS
    output wire [7:0]  vid_r,
    output wire [7:0]  vid_g,
    output wire [7:0]  vid_b,
    output wire        vid_de,
    output wire        vid_hs,
    output wire        vid_vs,
    output wire [9:0]  tmds_blue,
    output wire [9:0]  tmds_green,
    output wire [9:0]  tmds_red,

    // Состояние (домены источников; для теста)
    output wire        cap_overflow,
    output wire [15:0] frames_written,
    output wire [15:0] frames_rejected,
    output wire        fetch_late,
    output wire        have_frame,
    output wire        mem_ready,
    output wire        raster_locked,
    output wire [2:0]  raster_mode,
    output wire [11:0] raster_line_cycles,
    output wire [10:0] raster_lines,
    output wire [11:0] raster_hs_cycles,
    output wire        aligned_h,
    output wire        aligned_v,
    output wire        bb_valid,
    output wire [10:0] bb_x0,
    output wire [10:0] bb_x1,
    output wire [10:0] bb_y0,
    output wire [10:0] bb_y1,
    output reg         pattern_on,     // clk_mem: в SDRAM пишется таблица
    output wire [15:0] chk_err,        // pixel_clk: ошибки сверки таблицы за кадр
    output wire        chk_tick
);
    localparam logic [1:0] SRC_AUTO = 2'd0, SRC_EVO = 2'd1, SRC_TEST = 2'd2;
    localparam logic [1:0] DEF_DELAY_CODE = DEF_DELAY - 1;

    // ------------------------------------------------------------------
    // Настройки, кнопки, калибровка (pixel_clk)
    // ------------------------------------------------------------------
    wire [1:0]        set_src, cfg_src, cfg_delay, cfg_rd, cal_point;
    wire              cfg_phase, cfg_align, cfg_aspect, cfg_avg, cal_busy, cal_done, cal_fail;
    wire signed [7:0] cfg_h_adj, cfg_v_adj;
    wire [3:0]        cfg_clock;
    wire [1:0]        cfg_size;
    assign cap_clk_phase = cfg_clock;
    wire [3:0]        cal_ok;

    // Состояние в домене пикселей
    wire [15:0] p_frames_written, p_frames_rejected;
    wire        p_fetch_late, p_have_frame, p_mem_ready, p_pattern_on, p_avg_shown;
    wire        p_locked, p_hs_low, p_vs_low, p_aligned_h, p_aligned_v, p_bb_valid, p_overflow;
    wire [2:0]  p_mode;
    wire [11:0] p_line_cycles, p_hs_cycles;
    wire [10:0] p_lines, p_h_off, p_v_off, p_bb_x0, p_bb_x1, p_bb_y0, p_bb_y1;
    wire [15:0] pclk_khz;
    wire [31:0] p_rd_status;            // растеризатор FT812
    wire [15:0] p_rd_time;
    wire [10:0] p_rd_w;
    wire [9:0]  p_rd_h;

    // ------------------------------------------------------------------
    // LINK: регистры для ESP (домен пикселей). Адрес — байтовый, регистр 32 бита.
    //   0x00 ID       R   0x56444334 ("VDC4")
    //   0x04 VERSION  R   {личность 8'h01 (VDAC2, кадровый буфер), протокол LINK 8'h01, версия 16'h010B}
    //                     (1.1 — flash Tang и поправки по растрам; 1.2 — ASPECT и SIZE; 1.3 — UI_KEY;
    //                     1.4 — FT812: SPI хоста, регистры, RAM_CMD, RAM_DL, окно FT_*;
    //                     1.5 — RAM_G FT812 в SDRAM, 2/3 МБ, FT_CFG;
    //                     1.6 — растеризатор FT812, кадр FT через масштаб 720p при M_SEL = 1;
    //                     1.7 — связь с сопроцессором FT812 на BL616: cp_spi, cp_link, CP_INFO;
    //                     1.8 — почтовый ящик ESP → BL616: MB_PAGE, окно MBOX;
    //                     1.9 — PALETTED, REPEAT, тонкие RECTS и линии в растеризаторе FT812;
    //                     1.10 — оверлеи ESP вместо экранного меню: OVL*, окно 0x80000…;
    //                     кнопки Tang не читаются; 1.11 — multiboot: регистр BOOT;
    //                     1.30 — мост к microSD Tang для ESP: SD_*, только TS-Config;
    //                     1.32 — FRAME BLEND: CTRL 17, CHECK 18, бит 19 настроек во flash)
    //   0x08 STATUS   R   [15:0] флаги: 0 растр захвачен, 3:1 режим raster_detect, 4/5 HS/VS
    //                     активны низким, 6/7 окно по картинке H/V, 8 границы картинки есть,
    //                     9 в SDRAM таблица, 10 SDRAM готова, 11 есть кадр, 12 M_SEL,
    //                     13 калибровка идёт, 14 калибровка закончена, 15 калибровка не нашла точку;
    //                     [31:16] PCLK, кГц
    //   0x0C RASTER   R   [11:0] длина строки, такты 56 МГц; [26:16] строк в кадре
    //   0x10 SYNC     R   [11:0] ширина HS, такты 56 МГц; [19:16] карта годных точек SD READ
    //   0x14 WINDOW   R   [10:0] окно H; [26:16] окно V (с поправками)
    //   0x18 PIC_X    R   [10:0] левая; [26:16] правая граница не чёрной картинки
    //   0x1C PIC_Y    R   [10:0] верхняя; [26:16] нижняя
    //   0x20 FRAMES   R   [15:0] записано кадров; [31:16] отброшено сбойных
    //   0x24 CHECK    R   [15:0] расхождений таблицы за кадр; 16 строка не успела; 17 FIFO переполнялся;
    //                     18 (1.32) выводится среднее двух кадров (FRAME BLEND)
    //   0x28 FT_RSTAT R   растеризатор: [15:0] кадров нарисовано, [19:16] обходчик не умеет (0 линии,
    //                     1 MACRO, 2 стек CALL, 3 предел шагов), [23:20] исполнитель не умеет (0 REPEAT,
    //                     1 BILINEAR, 2 формат), [25:24] состояние, 28 включён, 29 кадр объявлен,
    //                     30 пишется буфер 1
    //   0x2C FT_RTIME R   [15:0] время последнего кадра, такты 61,875 МГц / 1024 (~16,5 мкс);
    //                     [26:16] ширина, [31:27] — 0
    //   0x30 CP_INFO  R   слово состояния прошивки BL616 (bl616/app/main.c): [31:24] 0xC0,
    //                     [23:16] версия, [15:12] ошибка сопроцессора, [11:8] 0 — ждёт, 1 — работает,
    //                     2 — в сбросе, [7:0] счётчик (растёт раз в 0,5 с — прошивка жива)
    //   0x34 MB_PAGE  RW  [1:0] страница почтового ящика ESP → BL616 в окне MBOX
    //   0x38 FT_FREE  R   (1.29) [9:0] свободных мест очереди записи окна FT812 (из 512), код
    //                     Грея: за такт меняется один разряд — чтение посреди смены не рвёт число
    //   0x3C SCRATCH  RW  проверка канала: что записано, то и читается
    //   0x40 CTRL     RW  [1:0] источник (0 AUTO, 1 EVO, 2 TEST); 2 MUX PHASE; [4:3] MUX DELAY − 1;
    //                     5 AUTO ALIGN; [7:6] SD READ; 8 — 0 (было меню); 9 — запуск калибровки
    //                     (при записи; читается 1, пока она идёт); [13:10] MUX CLOCK;
    //                     14 ASPECT (0 — 4:3, 1 — 16:9); [16:15] SIZE (0 — 100 %, 1 — 95 %, 2 — 90 %);
    //                     17 FRAME BLEND (1.32): среднее двух кадров Evo — только ядро TS-Config
    //                     без FT812, у других бит хранится и читается, но ничего не меняет
    //   0x44 ADJUST   RW  [7:0] H ADJUST, [15:8] V ADJUST (со знаком) — текущего растра
    //   0x48 FLASH_CMD  W  мост к flash: [6:0] передать байтов буфера, [14:8] принять в буфер,
    //                     16 — оставить CS активным (fb_flash); читается как FLASH_STATUS
    //   0x4C FLASH_STATUS R 0 занят, 1 настройки прочитаны, 2 найдены, 3 CS удерживается;
    //                     [15:8] ячейка загруженных настроек; [23:16] первая чистая ячейка
    //   0x50 FT_SEEN   R  первые 32 бита последней посылки Evo к FT812 (меньше 32 — младшие)
    //   0x54 FT_ADDR   W  окно в память FT812: [21:2] адрес слова; запись запускает его чтение;
    //                     чтение — FT_COUNT: [23:16] посылок, запустивших чип (ACTIVE или
    //                     память — по ним ядро без FT812 переходит в слот FT), [15:8] посылок,
    //                     [5:0] битов последней (63 — больше)
    //   0x58 FT_DATA   RW чтение — слово, прочитанное по FT_ADDR; запись — слово по FT_ADDR,
    //                     затем FT_ADDR += 4 (правила хост-машины FT812: регистры, REG_CMDB_WRITE).
    //                     С 1.29 слово записи идёт в очередь окна на 512 слов (FT_FREE); раньше окно
    //                     держало одно слово, и следующее до его записи терялось
    //   0x5C FT_STAT   R  31 окно занято, 30 INT_N, 29 показывается банк RAM_DL 1,
    //                     28 сопроцессор в сбросе, 27 RAM_G 3 МБ, 26 ответ RAM_G для SPI опоздал,
    //                     25 очередь RAM_G хоста переполнялась, 24 очередь связи с BL616
    //                     переполнялась, [23:16] состояние ft_core, 14 запись окна потеряна (с 1.25;
    //                     с 1.29 — слово при полной очереди или во время чтения, FT_ADDR при
    //                     занятом окне; сброс — принятой записью FT_ADDR). Окно занято (31), пока
    //                     идёт чтение или очередь записи не дошла до памяти FT812 вся;
    //                     13 без дизеринга, 12 хост — ESP, [11:0] REG_CMD_WRITE
    //        FT_CFG    W  0 — RAM_G 3 МБ (шрифты ПЗУ вне адресов FT812), иначе 2 МБ;
    //                     3 — окно FT_ADDR/FT_DATA пишет в образ ПЗУ (0x200000…, режим 2 МБ):
    //                     так ESP заливает шрифты ПЗУ после загрузки ядра (1.18);
    //                     1 — кадр FT812 без дизеринга; 2 — хост FT812 — ESP: SPI Evo к FT812
    //                     отключён (MISO 0, INT_N для Evo неактивен), автовозврат ядра FT812 —
    //                     только по M_SEL = 0
    //   0x60…0x7C ADJ_TAB RW поправки растров 0…7: [7:0] H, [15:8] V (0x60 + 4 × режим)
    //   0x80…0xBC FLASH_BUF RW буфер моста, 64 байта (байт 0 — старший байт 0x80)
    //   0xC0, 0xD0 OVL0_CTRL, OVL1_CTRL W  оверлей k (fb_ovl): 0 вкл., 1 — ×2 (точка холста — 2×2
    //                     точки вывода), [7:4] прозрачность (15 — непрозрачный)
    //   0xC4, 0xD4 OVLk_POS W  [10:0] x, [25:16] y на выводе (левый верхний угол)
    //   0xC8, 0xD8 OVLk_SIZE W [10:0] ширина холста (≤ 1024), [25:16] высота
    //   0xCC, 0xDC OVLk_BASE W [16:0] начало холста — слово региона оверлеев (кратно 8)
    //                     Настройки берутся в начале кадра вывода.
    //   0xE4 OVL_STAT R   0 — очередь записи оверлеев переполнялась
    //   0xE8 FT_HOST  W   команда хоста FT812 от ESP (1.12): [7:0] команда (00 ACTIVE, 44 CLKEXT,
    //                     61/62 CLKSEL, 68 CORERST, …), [15:8] параметр — как посылка хоста по SPI
    //   0xF0 BOOT     RW  чтение: [7:0] слот flash этого битстрима (по 1 МБ: слот 0 — TS-Config),
    //                     [15:8] слот, который загрузит RECONFIG_N, [16] захват Evo, [17] FT812,
    //                     [18] автопереход разрешён, [19] перезагрузка запрошена (1.13), [31:20]
    //                     0xB00; запись 0x424F4F54 («BOOT») — перезагрузка ПЛИС в тот слот
    //                     (multiboot). С 1.13 запрос уходит и в STATUS BL616 (бит 7 флагов):
    //                     перезагружает BL616 командой JTAG Reconfig — свой RECONFIG_N ПЛИС не
    //                     перезагружает
    //   0x40000…0x7FFFF FT_BURST W (1.29) слова в очередь окна FT812 — как FT_DATA, но
    //                     транзакцией любой длины (адрес LINK внутри окна не важен: слово идёт по
    //                     адресу окна, затем тот += 4). Места — FT_FREE; лишнее — потеря (FT_STAT 14)
    //   0x80000…0xFFFFF OVL_MEM W  регион оверлеев (512 КБ, SDRAM 0x108000…): слово (адрес −
    //                     0x80000) / 4, точки RGB565 по две в слове ([15:0] — левая); запись
    //                     подряд одной транзакцией любой длины
    //   0x100 SD_CTRL  RW  (1.30, только ядро с Evo) мост к microSD Tang (fb_sdspi): [7:0]
    //                     полупериод SCK − 1 в тактах пикселей, 8 — CS активен; чтение — и
    //                     [31:16] такт, десятки кГц
    //   0x104 SD_PTR   RW  [8:0] слово буферов, с которого идёт обмен; [26:16] байтов блока READ
    //   0x108 SD_CMD   W   [19:0] N, [22:20] операция: 0 XFER (TX → карта, ответ → RX), 1 FILL
    //                     (0xFF), 2 WAIT (0xFF, пока карта шлёт 0xFF), 3 READ (WAIT, за маркером
    //                     0xFE — блок в RX), 4 BUSY (0xFF, пока карта занята)
    //        SD_STAT   R   0 занят, 1 не дождались, [15:8] последний байт (WAIT/READ — R1 или
    //                     маркер), [31:16] 0x5D01; без моста (ядро FT812) — 0
    //   0x2000…0x27FC SD_TX W / SD_RX R  буферы моста, по 512 слов (байт 0 — старший байт слова):
    //                     запись — то, что уйдёт карте, чтение — её ответ (из BSRAM по фронту SCK)
    //   0x200…0x3FC MBOX W  почтовый ящик ESP → BL616 (cp_link, 512 слов): слово
    //                     MB_PAGE × 128 + (адрес − 0x200) / 4; BL616 читает его по 0x3F0000…
    //                     (протокол — bl616/app/fw_update.c)
    // ------------------------------------------------------------------
    wire        lk_wr_stb_w;              // слово записи (регистр или окно оверлеев)
    wire [17:0] lk_wr_word;
    wire        lk_wr_stb = lk_wr_stb_w && lk_wr_word[17:8] == 10'd0;   // регистры 0…255
    wire [7:0]  lk_wr_addr = lk_wr_word[7:0];
    wire        ov_stb    = lk_wr_stb_w && lk_wr_word[17];              // 0x80000…0xFFFFF
    wire [11:0] lk_rd_addr;                // номер слова чтения (окна — с 0x2000)
    wire [31:0] lk_wr_data;
    reg  [31:0] lk_rd_data;
    reg  [31:0] lk_scratch;

    fb_link link (
        .sck(link_sck), .cs_n(link_cs_n),
        .io_in(link_io_in), .io_out(link_io_out), .io_oe(link_io_oe),
        .clk(pixel_clk), .rst_n(rst_pix_n),
        .wr_stb(lk_wr_stb_w), .wr_addr(lk_wr_word), .wr_data(lk_wr_data),
        .rd_addr(lk_rd_addr), .rd_data(lk_rd_data)
    );

    // Оверлеи ESP: настройки (только запись; берутся в начале кадра вывода)
    localparam int NOVL = 2;
    reg  [NOVL-1:0]    ovl_on, ovl_x2;
    reg  [NOVL*4-1:0]  ovl_alpha;
    reg  [NOVL*11-1:0] ovl_x, ovl_w;
    reg  [NOVL*10-1:0] ovl_y, ovl_h;
    reg  [NOVL*17-1:0] ovl_base;
    wire               ovl_ovf;
    always_ff @(posedge pixel_clk or negedge rst_pix_n) begin
        if (!rst_pix_n) begin
            ovl_on <= '0; ovl_x2 <= '0; ovl_alpha <= '1; ovl_x <= '0; ovl_w <= '0;
            ovl_y <= '0; ovl_h <= '0; ovl_base <= '0;
        end else if (lk_wr_stb && lk_wr_addr[7:5] == 3'b001 && lk_wr_addr[4:3] == 2'b10) begin
            // 0xC0…0xDC: регистр lk_wr_addr[1:0] оверлея lk_wr_addr[2]
            for (int k = 0; k < NOVL; k++) if (lk_wr_addr[2] == k[0]) begin
                case (lk_wr_addr[1:0])
                    2'd0: begin
                        ovl_on[k]           <= lk_wr_data[0];
                        ovl_x2[k]           <= lk_wr_data[1];
                        ovl_alpha[k*4 +: 4] <= lk_wr_data[7:4];
                    end
                    2'd1: begin ovl_x[k*11 +: 11] <= lk_wr_data[10:0]; ovl_y[k*10 +: 10] <= lk_wr_data[25:16]; end
                    2'd2: begin ovl_w[k*11 +: 11] <= lk_wr_data[10:0]; ovl_h[k*10 +: 10] <= lk_wr_data[25:16]; end
                    default: ovl_base[k*17 +: 17] <= {lk_wr_data[16:3], 3'd0};
                endcase
            end
        end
    end

    always_ff @(posedge pixel_clk or negedge rst_pix_n) begin
        if (!rst_pix_n) lk_scratch <= '0;
        else if (lk_wr_stb && lk_wr_addr == 8'h0F) lk_scratch <= lk_wr_data;
    end

    // FT_CFG бит 2: хост FT812 — ESP (окно FT_ADDR/FT_DATA, правила хоста те же), SPI Evo к
    // FT812 отключён: ни FT_CS_N, ни такты Evo до ft_spi/ft_core не доходят, MISO — 0, INT_N
    // для Evo неактивен; прерывание FT812 ESP видит в FT_STAT
    reg         ft_cfg_esp;

    // BOOT (0xF0): запись «BOOT» (0x424F4F54) — перезагрузка ПЛИС из flash Tang с адреса
    // перехода в заголовке этого битстрима (multiboot); «ARM1» / «ARM0» (0x41524D31/30) —
    // разрешить / запретить автопереход ядра без FT812 в слот FT (ESP разрешает, проверив, что
    // там битстрим); другие значения не действуют.
    //
    // Обмен Evo с FT812 — не меньше 16 фронтов SCK за один выбор FT_CS_N (а не помеха на CS).
    // SCK до 28 МГц: каждый его уровень длится больше такта 74,25 МГц.
    //
    // Автопереход ядра без FT812 — только когда программа запускает чип: ACTIVE (00 00 00) или
    // обращение к памяти FT812 (адрес — старшие биты 00 или 10). Команды хоста без ACTIVE
    // (CORERST, PWRDOWN, SLEEP, CLKEXT, CLKSEL — так FT812 гасят при сбросе Evo) переход не
    // включают: прежде Tang уходил в ядро FT812 и обратно при каждом сбросе Evo (18.10.2026).
    // Первые 32 бита посылки и их число — в домене SCK (режим 0: MOSI по фронту SCK); в домене
    // пикселей они берутся по концу посылки (CS вверх — SCK стоит). LINK 0x14 — эти биты
    // последней посылки, 0x15 — {запусков чипа, посылок, битов} (байтовые 0x50, 0x54).
    // начальные значения: до первой посылки фронта CS вверх ещё не было
    reg         fs_fresh = 1'b1;             // CS был снят: следующий фронт SCK — первый бит
    reg  [31:0] fs_sr = '0;
    reg  [5:0]  fs_n = '0;
    always_ff @(posedge ft_sck or posedge ft_cs_n)
        if (ft_cs_n) fs_fresh <= 1'b1;
        else         fs_fresh <= 1'b0;
    always_ff @(posedge ft_sck)
        if (!ft_cs_n) begin
            if (fs_fresh) begin
                fs_n  <= 6'd1;
                fs_sr <= {31'd0, ft_mosi};
            end else begin
                if (fs_n != 6'd63) fs_n <= fs_n + 6'd1;
                if (fs_n < 6'd32)  fs_sr <= {fs_sr[30:0], ft_mosi};
            end
        end

    reg  [2:0]  fa_cs_s, fa_sck_s;
    reg  [4:0]  fa_cnt;
    reg         ft_act;                      // импульс: идёт обмен с FT812
    reg         ft_start;                    // импульс: посылка запустила чип (см. выше)
    reg         evo_rst;                     // импульс: CORERST (68 00 00) — так FT812 гасит
                                             // BIOS при сбросе Evo: источник TEST → AUTO
    reg  [31:0] ft_seen;                     // LINK 0x50 FT_SEEN
    reg  [7:0]  ft_xacts, ft_starts;
    reg  [5:0]  ft_bits;
    always_ff @(posedge pixel_clk or negedge rst_pix_n) begin
        if (!rst_pix_n) begin
            fa_cs_s <= '1; fa_sck_s <= '0; fa_cnt <= '0; ft_act <= 1'b0; ft_start <= 1'b0;
            evo_rst <= 1'b0;
            ft_seen <= '0; ft_xacts <= '0; ft_starts <= '0; ft_bits <= '0;
        end else begin
            fa_cs_s  <= {fa_cs_s[1:0], ft_cs_n};
            fa_sck_s <= {fa_sck_s[1:0], ft_sck};
            ft_act   <= 1'b0;
            ft_start <= 1'b0;
            evo_rst  <= 1'b0;
            if (fa_cs_s[1]) begin
                fa_cnt <= '0;
            end else if (fa_sck_s[1] && !fa_sck_s[2] && fa_cnt != 5'd16) begin
                fa_cnt <= fa_cnt + 5'd1;
                ft_act <= fa_cnt == 5'd15;
            end
            if (fa_cs_s[1] && !fa_cs_s[2] && fa_cnt == 5'd16) begin : xact_end
                logic go;
                go = (fs_n == 6'd24 && fs_sr[23:0] == 24'd0) || (fs_n >= 6'd32 && !fs_sr[30]);
                ft_seen  <= fs_sr;
                ft_bits  <= fs_n;
                ft_xacts <= ft_xacts + 8'd1;
                if (go) ft_starts <= ft_starts + 8'd1;
                ft_start <= go;
                evo_rst  <= fs_n == 6'd24 && fs_sr[23:0] == 24'h68_0000;
            end
        end
    end

    // Ядро FT812 без Evo: M_SEL = 0 (Evo вернула монитор TS-Config, например после сброса) и
    // обмена с FT812 нет SWITCH_IDLE_MS — обратно в слот TS-Config
    localparam int IDLE_W = $clog2(SWITCH_IDLE_MS + 1);
    reg  [$clog2(PIX_KHZ + 1)-1:0] ms_div;
    reg  [IDLE_W-1:0] idle_ms;
    reg         boot_armed;
    always_ff @(posedge pixel_clk or negedge rst_pix_n) begin
        if (!rst_pix_n) begin
            boot_req <= 1'b0; boot_armed <= 1'b0; ms_div <= '0; idle_ms <= '0;
        end else begin
            if (lk_wr_stb && lk_wr_addr == 8'h3C) begin
                if (lk_wr_data == 32'h424F_4F54) boot_req <= 1'b1;
                if (lk_wr_data == 32'h4152_4D31) boot_armed <= 1'b1;
                if (lk_wr_data == 32'h4152_4D30) boot_armed <= 1'b0;
            end
            if (HAS_EVO && !HAS_FT && boot_armed && ft_start) boot_req <= 1'b1;
            if (!HAS_EVO && HAS_FT) begin
                // тишина отсчитывается от последнего обмена: делитель миллисекунд — заново
                ms_div <= (blank || (ft_act && !ft_cfg_esp) || ms_div == PIX_KHZ - 1) ? '0 : ms_div + 1'b1;
                if (blank || (ft_act && !ft_cfg_esp))     // хост — ESP: обмен Evo не в счёт
                    idle_ms <= '0;
                else if (ms_div == PIX_KHZ - 1 && idle_ms != SWITCH_IDLE_MS[IDLE_W-1:0])
                    idle_ms <= idle_ms + 1'b1;
                if (idle_ms == SWITCH_IDLE_MS[IDLE_W-1:0]) boot_req <= 1'b1;
            end
        end
    end

    // Почтовый ящик ESP → BL616 (в cp_link): окно 128 слов, страница MB_PAGE
    reg  [1:0]  mb_page;
    always_ff @(posedge pixel_clk or negedge rst_pix_n) begin
        if (!rst_pix_n) mb_page <= 2'd0;
        else if (lk_wr_stb && lk_wr_addr == 8'h0D) mb_page <= lk_wr_data[1:0];
    end
    wire        mb_we    = lk_wr_stb && lk_wr_addr[7];
    wire [8:0]  mb_waddr = {mb_page, lk_wr_addr[6:0]};

    // ------------------------------------------------------------------
    // FT812: хост-машины — Evo по SPI и ESP по LINK (окно FT_ADDR/FT_DATA)
    // ------------------------------------------------------------------
    wire        ft_tg;
    wire [1:0]  ft_kind;
    wire [7:0]  ft_h0, ft_h1, ft_b, ft_rd_byte;
    wire        ft_miso_core;
    // вывод MISO — и DIN светодиода WS2812 модуля Tang: при поднятом FT_CS — «чёрный»
    ws_dark #(.CLK_KHZ(MEM_KHZ)) ws_dark (
        .clk(clk_mem), .rst_n(rst_mem_n), .cs_n(ft_cs_n), .miso(ft_miso_core), .pin(ft_miso)
    );
    generate if (HAS_FT) begin : g_ft_spi
        // хост — ESP (FT_CFG бит 2): выбор FT812 от Evo не доходит, MISO — 0
        wire miso_spi;
        ft_spi ft_spi (
            .sck(ft_sck), .cs_n(ft_cs_n | ft_cfg_esp), .mosi(ft_mosi), .miso(miso_spi),
            .tg(ft_tg), .ev_kind(ft_kind), .ev_h0(ft_h0), .ev_h1(ft_h1), .ev_b(ft_b),
            .rd_byte(ft_rd_byte)
        );
        assign ft_miso_core = ft_cfg_esp ? 1'b0 : miso_spi;
    end else begin : g_no_ft_spi
        // FT812 нет: MISO — 0, как у ft_spi без обмена (REG_ID читается 0, программа ждёт
        // ядро FT812 — оно грузится по первому обмену)
        assign ft_miso_core = 1'b0;
        assign ft_tg = 1'b0; assign ft_kind = '0;
        assign ft_h0 = '0; assign ft_h1 = '0; assign ft_b = '0;
    end endgenerate

    // Окно LINK к памяти FT812. FT_ADDR — адрес окна и чтение слова: запрос держится в домене
    // пикселей, пока домен SDRAM не ответит. Слова записи — FT_DATA и окно FT_BURST — с 1.29 идут
    // через очередь на 2^WQ_B слов (блок BSRAM, домен пикселей → clk_mem), и ESP пишет
    // транзакциями по 16 слов, не дожидаясь каждого: до 1.29 окно держало одно слово, ESP ждал
    // его (~3 мкс на слово — DOOM выгружал кадр 54 КБ за ~40 мс). Окно занято (FT_STAT 31), пока
    // идёт чтение или записано в память FT812 меньше, чем положено в очередь. Теряются (FT_STAT
    // 14): слово при полной очереди или во время чтения, FT_ADDR при занятом окне.
    localparam int WQ_B = 9;
    function automatic logic [WQ_B:0] wq_g2b(input logic [WQ_B:0] g);
        logic [WQ_B:0] b;
        b[WQ_B] = g[WQ_B];
        for (int i = WQ_B - 1; i >= 0; i--) b[i] = b[i + 1] ^ g[i];
        return b;
    endfunction
    function automatic logic [WQ_B:0] wq_b2g(input logic [WQ_B:0] b);
        return b ^ (b >> 1);
    endfunction
    reg         ft_rq_tg;
    reg  [21:2] ft_rq_addr;
    reg  [1:0]  ft_rs_s;
    reg         ft_rs_tg;                 // домен SDRAM
    reg  [31:0] ft_rs_data;               // домен SDRAM, устойчиво после ответа
    reg  [31:0] wq_mem [0:(1 << WQ_B) - 1];
    reg  [WQ_B:0] wq_w, wq_wg;            // положено в очередь (домен пикселей): двоичный, Грей
    reg  [WQ_B:0] wq_dg1, wq_dg2, wq_d;   // записано в память FT812 (из домена SDRAM, Грей) и двоичный
    wire [WQ_B:0] wq_dgm;                 // домен SDRAM: записано, Грей
    reg  [WQ_B:0] wq_fg;                  // FT_FREE: свободных мест, код Грея
    wire [WQ_B:0] wq_lvl  = wq_w - wq_d;  // занято мест (до 2^WQ_B)
    wire          wq_full = wq_lvl[WQ_B];
    wire          ft_rd_busy = ft_rq_tg != ft_rs_s[1];
    wire          ft_busy = ft_rd_busy || wq_w != wq_d;
    wire          ft_wq_stb = (lk_wr_stb && lk_wr_addr == 8'h16) ||                  // FT_DATA
                              (lk_wr_stb_w && lk_wr_word[17:16] == 2'b01);           // FT_BURST
    wire          ft_wq_put = ft_wq_stb && !wq_full && !ft_rd_busy;
    reg           ft_lost;                // FT_STAT 14: запись окна потеряна
    always_ff @(posedge pixel_clk)
        if (ft_wq_put) wq_mem[wq_w[WQ_B-1:0]] <= lk_wr_data;
    always_ff @(posedge pixel_clk or negedge rst_pix_n) begin
        if (!rst_pix_n) begin
            ft_rq_tg <= 1'b0; ft_rq_addr <= '0; ft_rs_s <= '0; ft_lost <= 1'b0;
            wq_w <= '0; wq_wg <= '0; wq_dg1 <= '0; wq_dg2 <= '0; wq_d <= '0; wq_fg <= '0;
        end else begin
            ft_rs_s <= {ft_rs_s[0], ft_rs_tg};
            wq_dg1  <= wq_dgm;
            wq_dg2  <= wq_dg1;
            wq_d    <= wq_g2b(wq_dg2);
            wq_fg   <= wq_b2g({1'b1, {WQ_B{1'b0}}} - wq_lvl);
            if (lk_wr_stb && lk_wr_addr == 8'h15) begin
                if (ft_busy) begin
                    ft_lost <= 1'b1;
                end else begin
                    ft_lost    <= 1'b0;
                    ft_rq_addr <= lk_wr_data[21:2];
                    ft_rq_tg   <= ~ft_rq_tg;
                end
            end
            if (ft_wq_stb && !ft_wq_put)
                ft_lost <= 1'b1;
            if (ft_wq_put) begin
                wq_w  <= wq_w + 1'b1;
                wq_wg <= wq_b2g(wq_w + 1'b1);
            end
        end
    end

    // FT_HOST (0xE8): команда хоста FT812 от ESP — [7:0] команда (00 ACTIVE, 44 CLKEXT,
    // 61/62 CLKSEL, 68 CORERST, …), [15:8] параметр; в ft_core — переключателем
    reg         ft_hc_tg;
    reg  [7:0]  ft_hc_cmd, ft_hc_par;
    always_ff @(posedge pixel_clk or negedge rst_pix_n)
        if (!rst_pix_n) begin
            ft_hc_tg <= 1'b0; ft_hc_cmd <= 8'd0; ft_hc_par <= 8'd0;
        end else if (lk_wr_stb && lk_wr_addr == 8'h3A) begin
            ft_hc_cmd <= lk_wr_data[7:0]; ft_hc_par <= lk_wr_data[15:8]; ft_hc_tg <= ~ft_hc_tg;
        end

    // SD_DLY (0xEC, ядро FT812): тонкая задержка чтения DQ SDRAM — IODELAYA на всех 32 входах DQ,
    // шаг ~18 пс (GW2A-18, UG289), 0…127 (~2,3 нс); вместе с точкой выборки SD READ (CTRL [7:6], шаг
    // полтакта clk_sd — 4 нс) — окно чтения на 123,75 МГц подбирается на плате. Запись: [6:0] —
    // нужная задержка, 7 — обратить направление шага (на случай другой полярности SETN), 8 — снова
    // взять статическую (0, SDTAP = 0). Чтение: [31:24] D1, 16 — DF, 15 — идут шаги, [14:8] — текущая,
    // 7 — обращение, [6:0] — нужная. Шаг — спад VALUE (UG289), раз в 16 тактов pixel_clk; задержка
    // общая для всех 32 линий; после включения — 0, как без IODELAYA.
    reg  [6:0]  sdl_tgt, sdl_cur;
    reg         sdl_inv, sdl_sdtap, sdl_setn, sdl_value, sdl_df, sdl_up;
    reg  [3:0]  sdl_ph;
    wire        sdl_busy = !sdl_sdtap || sdl_cur != sdl_tgt || sdl_ph != 4'd0;
    always_ff @(posedge pixel_clk or negedge rst_pix_n)
        if (!rst_pix_n) begin
            sdl_tgt <= '0; sdl_cur <= '0; sdl_inv <= 1'b0; sdl_sdtap <= 1'b0; sdl_setn <= 1'b0;
            sdl_value <= 1'b0; sdl_ph <= '0; sdl_df <= 1'b0; sdl_up <= 1'b1;
        end else begin
            sdl_df <= sdram_dly_df;
            if (lk_wr_stb && lk_wr_addr == 8'h3B) begin
                sdl_tgt <= lk_wr_data[6:0];
                sdl_inv <= lk_wr_data[7];
                if (lk_wr_data[8]) begin
                    sdl_sdtap <= 1'b0; sdl_cur <= '0; sdl_ph <= '0; sdl_value <= 1'b0;
                end
            end else if (!sdl_sdtap) begin
                sdl_ph <= sdl_ph + 4'd1;                 // статическая берётся 16 тактов,
                if (sdl_ph == 4'd15) sdl_sdtap <= 1'b1;  // дальше — на ходу
            end else if (sdl_ph != 4'd0 || sdl_cur != sdl_tgt) begin
                // шаг: направление — в его начале (новая нужная посреди шага его не сбивает)
                sdl_ph <= sdl_ph + 4'd1;
                if (sdl_ph == 4'd0) begin
                    sdl_up   <= sdl_tgt > sdl_cur;
                    sdl_setn <= (sdl_tgt < sdl_cur) ^ sdl_inv;           // SETN 0 — больше
                end
                if (sdl_ph == 4'd4) sdl_value <= 1'b1;
                if (sdl_ph == 4'd10) begin
                    sdl_value <= 1'b0;                   // спад VALUE — шаг
                    sdl_cur <= sdl_up ? sdl_cur + 7'd1 : sdl_cur - 7'd1;
                end
            end
        end
    assign sdram_dly = {sdl_sdtap, sdl_setn, sdl_value};

    // FT_CFG: размер RAM_G, дизеринг (меняются редко — в домен SDRAM через два триггера);
    // M_SEL — туда же: режим FT812 кадрового буфера и растеризатора
    reg         ft_cfg_3mb, ft_cfg_nodith, ft_cfg_romwe;
    reg  [1:0]  ft_cfg_3mb_s, ft_cfg_nodith_s, ft_cfg_romwe_s, m_ft_s;
    always_ff @(posedge pixel_clk or negedge rst_pix_n)
        if (!rst_pix_n) begin
            ft_cfg_3mb <= 1'b0; ft_cfg_nodith <= 1'b0; ft_cfg_esp <= 1'b0; ft_cfg_romwe <= 1'b0;
        end else if (lk_wr_stb && lk_wr_addr == 8'h17) begin
            ft_cfg_3mb <= lk_wr_data[0]; ft_cfg_nodith <= lk_wr_data[1]; ft_cfg_esp <= lk_wr_data[2];
            ft_cfg_romwe <= lk_wr_data[3];
        end
    always_ff @(posedge clk_mem) begin
        ft_cfg_3mb_s    <= {ft_cfg_3mb_s[0], ft_cfg_3mb};
        ft_cfg_nodith_s <= {ft_cfg_nodith_s[0], ft_cfg_nodith};
        ft_cfg_romwe_s  <= {ft_cfg_romwe_s[0], ft_cfg_romwe};
        m_ft_s          <= {m_ft_s[0], SCALE ? blank : 1'b0};
    end
    // Кадр FT вместо кадра Evo: общее ядро — по M_SEL, ядро FT812 — всегда, кроме таблицы
    wire        m_ft = HAS_FT && (HAS_EVO ? m_ft_s[1] : !pattern_on);

    // RAM_G FT812 — клиент SDRAM (арбитр — у sdram_burst ниже)
    wire        ft_g_req, ft_g_we, ft_g_ack, ft_g_wr_take, ft_g_rd_valid;
    wire [20:0] ft_g_addr;
    wire [2:0]  ft_g_wr_idx, ft_g_rd_idx;
    // растеризатор
    wire [10:0] rd_dl_addr;
    wire [31:0] rd_dl_q;
    wire        rd_swap_hold, ft_disp_on;
    wire [11:0] ft_hsize, ft_vsize;
    wire [31:0] ft_g_wdata, ft_g_rd_data;
    wire [3:0]  ft_g_wmask;
    wire        ft_ramg_late, ft_ramg_drop, ft_ramg_urgent;

    // Окно LINK, домен SDRAM: чтение по FT_ADDR (оно же ставит адрес окна), слова очереди — по
    // адресу окна подряд, по одному запросу к порту слов ft_core
    reg  [1:0]    ft_rq_s;
    reg           ft_p_req, ft_p_we;
    reg  [21:2]   ft_ptr;                 // адрес окна
    reg  [31:0]   ft_p_wdata;
    wire          ft_p_ack;
    wire [31:0]   ft_p_rdata;
    wire [11:0]   ft_cmd_write;
    reg  [WQ_B:0] wq_wg1, wq_wg2, wq_wb;  // положено — из домена пикселей (Грей) и двоичный
    reg  [WQ_B:0] wq_r;                   // взято из очереди
    reg  [WQ_B:0] wq_dm, wq_dmg;          // записано в память FT812: двоичный, Грей
    reg  [31:0]   wq_q;                   // слово очереди по wq_r (BSRAM, такт задержки)
    assign wq_dgm = wq_dmg;
    always_ff @(posedge clk_mem)
        wq_q <= wq_mem[wq_r[WQ_B-1:0]];
    always_ff @(posedge clk_mem or negedge rst_mem_n) begin
        if (!rst_mem_n) begin
            ft_rq_s <= '0; ft_p_req <= 1'b0; ft_p_we <= 1'b0; ft_ptr <= '0; ft_p_wdata <= '0;
            ft_rs_tg <= 1'b0; ft_rs_data <= '0;
            wq_wg1 <= '0; wq_wg2 <= '0; wq_wb <= '0; wq_r <= '0; wq_dm <= '0; wq_dmg <= '0;
        end else begin
            ft_rq_s <= {ft_rq_s[0], ft_rq_tg};
            wq_wg1  <= wq_wg;
            wq_wg2  <= wq_wg1;
            wq_wb   <= wq_g2b(wq_wg2);
            if (ft_p_req) begin
                if (ft_p_ack) begin
                    ft_p_req <= 1'b0;
                    if (ft_p_we) begin
                        ft_ptr <= ft_ptr + 20'd1;         // слово записано — следующий адрес
                        wq_dm  <= wq_dm + 1'b1;
                        wq_dmg <= wq_b2g(wq_dm + 1'b1);
                    end else begin
                        ft_rs_data <= ft_p_rdata;
                        ft_rs_tg   <= ft_rq_s[1];
                    end
                end
            end else if (ft_rq_s[1] != ft_rs_tg) begin
                // FT_ADDR: домен пикселей принимает его только при пустой очереди
                ft_ptr   <= ft_rq_addr;
                ft_p_we  <= 1'b0;
                ft_p_req <= 1'b1;
            end else if (wq_r != wq_wb) begin
                // слово очереди: wq_q — по wq_r, тот не менялся с прошлого запроса (≥ 2 тактов)
                ft_p_wdata <= wq_q;
                ft_p_we    <= 1'b1;
                ft_p_req   <= 1'b1;
                wq_r       <= wq_r + 1'b1;
            end
        end
    end

    // Сопроцессор на BL616: приёмник SPI (домен cp_sck) и очередь к порту слов
    wire         cp_tg;
    wire [1:0]   cp_kind;
    wire [31:0]  cp_data, cp_rb_q;
    wire [127:0] cp_st;
    wire [8:0]   cp_rb_addr;
    cp_spi cp_spi (
        .sck(cp_sck), .cs_n(cp_cs_n), .mosi(cp_mosi), .miso(cp_miso),
        .tg(cp_tg), .ev_kind(cp_kind), .ev_data(cp_data), .st_vec(cp_st),
        .rb_addr(cp_rb_addr), .rb_q(cp_rb_q)
    );

    wire         cp_p_req, cp_p_we, cp_ovf;
    wire [31:0]  cp_info, p_cp_info;
    wire [21:2]  cp_p_addr;
    wire [31:0]  cp_p_wdata;
    wire [11:0]  ft_cmd_read;
    wire [12:0]  ft_cmd_dl;
    wire [1:0]   ft_dlswap;
    wire [2:0]   ft_cpureset;
    wire [7:0]   ft_cp_resets;
    // Порт слов ft_core — двум: окну LINK и сопроцессору
    wire         pa_req, pa_we, pa_cp, pa_ack, cp_p_ack;
    wire [21:2]  pa_addr;
    wire [31:0]  pa_wdata, pa_rdata;
    generate if (HAS_FT) begin : g_port_arb
        ft_port_arb port_arb (
            .clk(clk_mem), .rst_n(rst_mem_n),
            .a_req(ft_p_req), .a_we(ft_p_we), .a_addr(ft_ptr), .a_wdata(ft_p_wdata), .a_ack(ft_p_ack),
            .b_req(cp_p_req), .b_we(cp_p_we), .b_addr(cp_p_addr), .b_wdata(cp_p_wdata), .b_ack(cp_p_ack),
            .p_req(pa_req), .p_we(pa_we), .p_cp(pa_cp), .p_addr(pa_addr), .p_wdata(pa_wdata),
            .p_ack(pa_ack)
        );
    end else begin : g_no_port_arb
        // FT812 нет: окно LINK и BL616 получают ответ сразу, чтение — 0
        assign ft_p_ack = ft_p_req;
        assign cp_p_ack = cp_p_req;
        assign pa_req = 1'b0; assign pa_we = 1'b0; assign pa_cp = 1'b0;
        assign pa_addr = '0; assign pa_wdata = '0;
    end endgenerate
    assign ft_p_rdata = pa_rdata;

    // Запрос перезагрузки — в домен clk_mem, в STATUS BL616 (бит 7 флагов): BL616 перезагружает
    // ПЛИС командой JTAG Reconfig по адресу перехода multiboot (стенд 18.10.2026: свой RECONFIG_N
    // ПЛИС, выведенный как GPIO, перезагрузку не запускает)
    reg  [1:0]  boot_m_s;
    always_ff @(posedge clk_mem) boot_m_s <= {boot_m_s[0], boot_req};

    cp_link cp_link (
        .clk(clk_mem), .rst_n(rst_mem_n), .boot(boot_m_s[1]),
        .sck(cp_sck), .spi_tg(cp_tg), .spi_kind(cp_kind), .spi_data(cp_data), .st_vec(cp_st),
        .rb_addr(cp_rb_addr), .rb_q(cp_rb_q),
        .p_req(cp_p_req), .p_we(cp_p_we), .p_addr(cp_p_addr), .p_wdata(cp_p_wdata),
        .p_ack(cp_p_ack), .p_rdata(pa_rdata),
        .cmd_read(ft_cmd_read), .cmd_write(ft_cmd_write), .cmd_dl(ft_cmd_dl), .dlswap(ft_dlswap),
        .cpureset(ft_cpureset), .resets(ft_cp_resets), .irq_n(cp_irq_n), .overflow(cp_ovf),
        .info(cp_info),
        .mb_clk(pixel_clk), .mb_we(mb_we), .mb_waddr(mb_waddr), .mb_wdata(lk_wr_data)
    );
    cdc_view #(.W(32)) cp_to_pix (.clk(pixel_clk), .d(cp_info), .q(p_cp_info));

    wire        ft_dl_active, ft_copro_hold;
    wire [7:0]  ft_status;
    wire        ft_int_core;                 // INT_N эмулятора; к Evo — только если хост не ESP
    assign ft_int_n = ft_cfg_esp ? 1'b1 : ft_int_core;
    generate if (HAS_FT) begin : g_ft
        // ядро FT812 без Evo (слот 1) — сразу ACTIVE: хост начал обмен ещё в слоте TS-Config
        ft_core #(.CLK_KHZ(MEM_KHZ), .START_ACTIVE(!HAS_EVO)) ft (
            .clk(clk_mem), .rst_n(rst_mem_n),
            .spi_tg(ft_tg), .spi_kind(ft_kind), .spi_h0(ft_h0), .spi_h1(ft_h1), .spi_b(ft_b),
            .spi_cs_n(ft_cs_n | ft_cfg_esp), .spi_rd_byte(ft_rd_byte),
            .host_tg(ft_hc_tg), .host_cmd(ft_hc_cmd), .host_par(ft_hc_par),
            .p_req(pa_req), .p_we(pa_we), .p_cp(pa_cp), .p_addr(pa_addr), .p_wdata(pa_wdata),
            .p_ack(pa_ack), .p_rdata(pa_rdata),
            .int_n(ft_int_core), .cmd_kick(), .copro_hold(ft_copro_hold), .cmd_read_ptr(ft_cmd_read),
            .cmd_write_ptr(ft_cmd_write), .dl_active(ft_dl_active), .frame_tick(),
            .cmd_dl_val(ft_cmd_dl), .dlswap_val(ft_dlswap), .cpureset_val(ft_cpureset),
            .copro_resets(ft_cp_resets),
            .rd_dl_addr(rd_dl_addr), .rd_dl_q(rd_dl_q), .swap_hold(rd_swap_hold),
            .disp_hsize(ft_hsize), .disp_vsize(ft_vsize), .disp_on(ft_disp_on), .status(ft_status),
            .ramg_late(ft_ramg_late), .ramg_drop(ft_ramg_drop), .ramg_urgent(ft_ramg_urgent),
            .ramg_3mb(ft_cfg_3mb_s[1]),
            .rom_we(ft_cfg_romwe_s[1]),
            .g_req(ft_g_req), .g_we(ft_g_we), .g_addr(ft_g_addr), .g_ack(ft_g_ack),
            .g_wr_take(ft_g_wr_take), .g_wr_idx(ft_g_wr_idx), .g_wdata(ft_g_wdata),
            .g_wmask(ft_g_wmask), .g_rd_valid(ft_g_rd_valid), .g_rd_idx(ft_g_rd_idx),
            .g_rd_data(ft_g_rd_data)
        );
    end else begin : g_no_ft
        // FT812 нет: сопроцессор BL616 видит его в сбросе и ждёт, очередь команд пуста
        assign ft_rd_byte = 8'd0; assign pa_ack = 1'b0; assign pa_rdata = 32'd0;
        assign ft_int_core = 1'b1; assign ft_copro_hold = 1'b0; assign ft_dl_active = 1'b0;
        assign ft_cmd_read = '0; assign ft_cmd_write = '0; assign ft_cmd_dl = '0;
        assign ft_dlswap = '0; assign ft_cpureset = 3'b001; assign ft_cp_resets = '0;
        assign rd_dl_q = '0; assign rd_swap_hold = 1'b0;
        assign ft_hsize = '0; assign ft_vsize = '0; assign ft_disp_on = 1'b0; assign ft_status = '0;
        assign ft_ramg_late = 1'b0; assign ft_ramg_drop = 1'b0; assign ft_ramg_urgent = 1'b0;
        assign ft_g_req = 1'b0; assign ft_g_we = 1'b0; assign ft_g_addr = '0;
        assign ft_g_wdata = '0; assign ft_g_wmask = '0;
    end endgenerate

    // Записи настроек (CTRL, ADJUST, таблица поправок) — тактом позже:
    // сравнение адреса отдельной ступенью, иначе путь от адреса LINK до регистров fb_ui
    // едва укладывается в такт 74,25 МГц
    reg        ui_ctrl_we, ui_adj_we, ui_tab_we;
    reg [2:0]  ui_tab_idx;
    reg [31:0] ui_wr_data;
    always_ff @(posedge pixel_clk or negedge rst_pix_n) begin
        if (!rst_pix_n) begin
            ui_ctrl_we <= 1'b0; ui_adj_we <= 1'b0; ui_tab_we <= 1'b0;
            ui_tab_idx <= '0; ui_wr_data <= '0;
        end else begin
            ui_ctrl_we <= lk_wr_stb && lk_wr_addr == 8'h10;
            ui_adj_we  <= lk_wr_stb && lk_wr_addr == 8'h11;
            ui_tab_we  <= lk_wr_stb && lk_wr_addr[7:3] == 5'b00011;
            ui_tab_idx <= lk_wr_addr[2:0];
            ui_wr_data <= lk_wr_data;
        end
    end

    // Flash Tang: настройки и мост
    wire [31:0]  fl_status, fl_buf_rdata;
    wire         fl_load_stb;
    wire [11:0]  fl_load_cfg;
    wire [127:0] fl_load_adj, adj_tab;

    // Мост — только в ядре с Evo (TS-Config, слот 0). Ядру FT812 (слот 1, 1.31+) он не нужен:
    // из настроек у него важны лишь ASPECT и SIZE — их пишет в CTRL ESP после перехода, шрифты ПЗУ
    // ESP берёт из своей копии (читает её из flash в TS-Config). FLASH_STATUS = 0: моста нет.
    generate if (HAS_EVO) begin : g_flash
        fb_flash #(.CLK_KHZ(PIX_KHZ), .SET_ADDR(FLASH_SET_ADDR), .BOOT_US(FLASH_BOOT_US)) flash (
            .clk(pixel_clk), .rst_n(rst_pix_n),
            .f_sck(flash_sck), .f_cs_n(flash_cs_n), .f_mosi(flash_mosi), .f_miso(flash_miso),
            .buf_we(lk_wr_stb && lk_wr_addr[7:4] == 4'h2), .buf_waddr(lk_wr_addr[3:0]),
            .buf_wdata(lk_wr_data),
            .buf_raddr(lk_rd_addr[3:0]), .buf_rdata(fl_buf_rdata),
            .cmd_we(lk_wr_stb && lk_wr_addr == 8'h12), .cmd(lk_wr_data),
            .status(fl_status),
            .load_stb(fl_load_stb), .load_cfg(fl_load_cfg), .load_adj(fl_load_adj)
        );
    end else begin : g_no_flash
        assign flash_sck = 1'b0; assign flash_cs_n = 1'b1; assign flash_mosi = 1'b0;
        assign fl_status = 32'd0; assign fl_buf_rdata = 32'd0;
        assign fl_load_stb = 1'b0; assign fl_load_cfg = '0; assign fl_load_adj = '0;
    end endgenerate

    // Мост к microSD Tang (только ядро с Evo — TS-Config, слот 0)
    wire [31:0] sd_ctrl_rd, sd_ptr_rd, sd_stat, sd_rx_q;
    generate if (HAS_EVO) begin : g_tf
        fb_sdspi #(.CLK_KHZ(PIX_KHZ)) sdspi (
            .clk(pixel_clk), .rst_n(rst_pix_n),
            .sd_cs_n(sd_cs_n), .sd_sck(sd_sck), .sd_mosi(sd_mosi), .sd_miso(sd_miso),
            .ctrl_we(lk_wr_stb && lk_wr_addr == 8'h40), .ptr_we(lk_wr_stb && lk_wr_addr == 8'h41),
            .cmd_we(lk_wr_stb && lk_wr_addr == 8'h42), .wdata(lk_wr_data),
            .ctrl_rd(sd_ctrl_rd), .ptr_rd(sd_ptr_rd), .status(sd_stat),
            .tx_we(lk_wr_stb_w && lk_wr_word[17:9] == 9'd4), .tx_waddr(lk_wr_word[8:0]),
            .tx_wdata(lk_wr_data),
            .rd_clk(link_sck), .rd_addr(lk_rd_addr[8:0]), .rd_q(sd_rx_q)
        );
    end else begin : g_no_tf
        assign sd_cs_n = 1'b1; assign sd_sck = 1'b0; assign sd_mosi = 1'b1;
        assign sd_ctrl_rd = '0; assign sd_ptr_rd = '0; assign sd_stat = '0; assign sd_rx_q = '0;
    end endgenerate

    always_comb begin
        if (lk_rd_addr[11]) lk_rd_data = sd_rx_q;            // 0x2000…: окно RX моста к microSD
        else case (lk_rd_addr[7:0])
            8'h00: lk_rd_data = 32'h5644_4334;
            8'h01: lk_rd_data = {HAS_EVO ? 8'h01 : 8'h02, 8'h01, 16'h0120};
            8'h02: lk_rd_data = {pclk_khz, cal_fail, cal_done, cal_busy, blank, p_have_frame,
                                 p_mem_ready, p_pattern_on, p_bb_valid, p_aligned_v, p_aligned_h,
                                 p_vs_low, p_hs_low, p_mode, p_locked};
            8'h03: lk_rd_data = {5'd0, p_lines, 4'd0, p_line_cycles};
            8'h04: lk_rd_data = {12'd0, cal_ok, 4'd0, p_hs_cycles};
            8'h05: lk_rd_data = {5'd0, p_v_off, 5'd0, p_h_off};
            8'h06: lk_rd_data = {5'd0, p_bb_x1, 5'd0, p_bb_x0};
            8'h07: lk_rd_data = {5'd0, p_bb_y1, 5'd0, p_bb_y0};
            8'h08: lk_rd_data = {p_frames_rejected, p_frames_written};
            8'h09: lk_rd_data = {13'd0, p_avg_shown, p_overflow, p_fetch_late, chk_err};
            8'h0A: lk_rd_data = p_rd_status;
            8'h0B: lk_rd_data = {5'd0, p_rd_w, p_rd_time};
            8'h0D: lk_rd_data = {30'd0, mb_page};
            8'h0E: lk_rd_data = {{(31 - WQ_B){1'b0}}, wq_fg};
            8'h0F: lk_rd_data = lk_scratch;
            8'h10: lk_rd_data = {14'd0, cfg_avg, cfg_size, cfg_aspect, cfg_clock, cal_busy, 1'b0, cfg_rd, cfg_align, cfg_delay, cfg_phase, set_src};
            8'h11: lk_rd_data = {16'd0, cfg_v_adj, cfg_h_adj};
            8'h12, 8'h13: lk_rd_data = fl_status;
            8'h14: lk_rd_data = ft_seen;
            8'h15: lk_rd_data = {8'd0, ft_starts, ft_xacts, 2'd0, ft_bits};
            8'h16: lk_rd_data = ft_rs_data;
            8'h0C: lk_rd_data = p_cp_info;
            8'h39: lk_rd_data = {31'd0, ovl_ovf};
            8'h3B: lk_rd_data = SD_FAST ? {8'hD1, 7'd0, sdl_df, sdl_busy, sdl_cur, sdl_inv, sdl_tgt} : 32'd0;
            8'h3C: lk_rd_data = {12'hB00, boot_req, boot_armed, HAS_FT, HAS_EVO, BOOT_NEXT, BOOT_SLOT};
            8'h40: lk_rd_data = sd_ctrl_rd;
            8'h41: lk_rd_data = sd_ptr_rd;
            8'h42: lk_rd_data = sd_stat;
            8'h17: lk_rd_data = {ft_busy, ft_int_core, ft_dl_active, ft_copro_hold, ft_cfg_3mb,
                                 ft_ramg_late, ft_ramg_drop, cp_ovf, ft_status, 1'b0, ft_lost, ft_cfg_nodith, ft_cfg_esp, ft_cmd_write};
            default:
                if (lk_rd_addr[7:3] == 5'b00011)  lk_rd_data = {16'd0, adj_tab[lk_rd_addr[2:0] * 16 +: 16]};
                else if (lk_rd_addr[7:4] == 4'h2) lk_rd_data = fl_buf_rdata;
                else                              lk_rd_data = 32'd0;
        endcase
    end

    fb_ui #(
        .TICK(PIX_KHZ), .CAL_TIMEOUT_MS(CAL_TIMEOUT_MS),
        .DEF_SRC(DEF_SRC), .DEF_PHASE(DEF_PHASE), .DEF_DELAY(DEF_DELAY_CODE), .DEF_CLOCK(DEF_CLOCK),
        .DEF_RD(DEF_RD), .DEF_ALIGN(DEF_ALIGN), .DEF_ASPECT(DEF_ASPECT), .DEF_SIZE(DEF_SIZE),
        .DEF_AVG(DEF_AVG), .CALIBRATE(CALIBRATE)
    ) ui (
        .clk(pixel_clk), .rst_n(rst_pix_n),
        .chk_tick(chk_tick), .chk_err(chk_err), .pattern_on(p_pattern_on),
        .frames_written(p_frames_written),
        .ext_ctrl_we(ui_ctrl_we), .ext_ctrl(ui_wr_data[17:0]), .ext_src_auto(evo_rst),
        .ext_adj_we(ui_adj_we),
        .ext_h_adj(ui_wr_data[7:0]), .ext_v_adj(ui_wr_data[15:8]),
        .ext_tab_we(ui_tab_we), .ext_tab_idx(ui_tab_idx),
        .ext_tab_data(ui_wr_data[15:0]),
        .ext_load_stb(fl_load_stb), .ext_load_cfg(fl_load_cfg), .ext_load_adj(fl_load_adj),
        .mode(p_mode), .adj_tab(adj_tab),
        .set_src(set_src), .cfg_src(cfg_src), .cfg_phase(cfg_phase), .cfg_delay(cfg_delay),
        .cfg_clock(cfg_clock), .cfg_h_adj(cfg_h_adj), .cfg_v_adj(cfg_v_adj), .cfg_align(cfg_align),
        .cfg_aspect(cfg_aspect), .cfg_size(cfg_size), .cfg_avg(cfg_avg), .cfg_rd(cfg_rd),
        .cal_busy(cal_busy), .cal_point(cal_point),
        .cal_ok(cal_ok), .cal_done(cal_done), .cal_fail(cal_fail)
    );

    // ------------------------------------------------------------------
    // Приём и упаковка (clk_cap) — только в ядре с Evo (HAS_EVO)
    // ------------------------------------------------------------------
    wire        hs_low, vs_low;
    wire [10:0] h_off, v_off;
    wire        cap_rd, cap_empty;
    wire [32:0] cap_q;
    generate if (HAS_EVO) begin : g_evo
    wire              c_phase, c_align;
    wire [1:0]        c_delay;
    wire signed [7:0] c_h_adj, c_v_adj;
    cdc_bus #(.W(20), .INIT({DEF_PHASE, DEF_DELAY_CODE, 8'd0, 8'd0, DEF_ALIGN})) cfg_to_cap (
        .clk(clk_cap), .rst_n(rst_cap_n),
        .d({cfg_phase, cfg_delay, cfg_h_adj, cfg_v_adj, cfg_align}),
        .q({c_phase, c_delay, c_h_adj, c_v_adj, c_align})
    );

    wire       pix_valid, pix_pal, hs_active, vs_active, hs_start, vs_start;
    wire [4:0] pix_r, pix_g, pix_b;
    wire       hs_level, vs_level, frame_ok;

    ts_mux_capture capture (
        .clk_cap(clk_cap), .rst_n(rst_cap_n),
        .sample_delay(c_delay), .sel_phase(c_phase),
        .mux_d(mux_d), .hsync_in(hsync_in), .vsync_in(vsync_in), .mux_sel(mux_sel),
        .hs_active_low(hs_low), .vs_active_low(vs_low),
        .hs_level(hs_level), .vs_level(vs_level),
        .pix_valid(pix_valid), .pix_r(pix_r), .pix_g(pix_g), .pix_b(pix_b), .pix_pal(pix_pal),
        .hs_active(hs_active), .vs_active(vs_active), .hs_start(hs_start), .vs_start(vs_start)
    );

    wire        fifo_wr, fifo_full;
    wire [32:0] fifo_wdata;

    wire bb_new;
    active_bbox bbox (
        .clk_cap(clk_cap), .rst_n(rst_cap_n),
        .pix_valid(pix_valid), .pix_r(pix_r), .pix_g(pix_g), .pix_b(pix_b),
        .hs_start(hs_start), .vs_start(vs_start),
        .bb_new(bb_new), .bb_valid(bb_valid),
        .bb_x0(bb_x0), .bb_x1(bb_x1), .bb_y0(bb_y0), .bb_y1(bb_y1)
    );

    raster_detect #(
        .H_ACTIVE(H_ACTIVE), .V_ACTIVE(V_ACTIVE), .TEST_PROFILES(TEST_PROFILES)
    ) detect (
        .clk_cap(clk_cap), .rst_n(rst_cap_n),
        .hs_level(hs_level), .vs_level(vs_level), .hs_active(hs_active),
        .hs_start(hs_start), .vs_start(vs_start),
        .h_adj(c_h_adj), .v_adj(c_v_adj),
        .auto_align(c_align), .bb_new(bb_new), .bb_valid(bb_valid),
        .bb_x0(bb_x0), .bb_x1(bb_x1), .bb_y0(bb_y0), .bb_y1(bb_y1),
        .aligned_h(aligned_h), .aligned_v(aligned_v),
        .hs_active_low(hs_low), .vs_active_low(vs_low),
        .locked(raster_locked), .frame_ok(frame_ok), .mode(raster_mode),
        .h_off(h_off), .v_off(v_off),
        .line_cycles(raster_line_cycles), .frame_lines(raster_lines), .hs_cycles(raster_hs_cycles)
    );

    fb_capture #(
        .H_ACTIVE(H_ACTIVE), .V_ACTIVE(V_ACTIVE)
    ) packer (
        .clk_cap(clk_cap), .rst_n(rst_cap_n),
        .pix_valid(pix_valid), .pix_r(pix_r), .pix_g(pix_g), .pix_b(pix_b), .pix_pal(pix_pal),
        .hs_start(hs_start), .vs_start(vs_start),
        .enable(raster_locked), .h_off(h_off), .v_off(v_off), .frame_ok(frame_ok),
        .fifo_wr(fifo_wr), .fifo_data(fifo_wdata), .fifo_full(fifo_full),
        .overflow(cap_overflow)
    );

    async_fifo #(.WIDTH(33), .ADDR_BITS(8)) cap_fifo (
        .wr_clk(clk_cap), .wr_reset_n(rst_cap_n), .wr_en(fifo_wr), .wr_data(fifo_wdata),
        .wr_full(fifo_full),
        .rd_clk(clk_mem), .rd_reset_n(rst_mem_n), .rd_en(cap_rd), .rd_data(cap_q),
        .rd_empty(cap_empty)
    );
    end else begin : g_no_evo
        // Ядро FT812: захвата Evo нет — растра нет, FIFO пуст
        assign mux_sel = 1'b0;
        assign hs_low = 1'b0; assign vs_low = 1'b0; assign h_off = '0; assign v_off = '0;
        assign cap_empty = 1'b1; assign cap_q = '0; assign cap_overflow = 1'b0;
        assign bb_valid = 1'b0; assign bb_x0 = '0; assign bb_x1 = '0; assign bb_y0 = '0; assign bb_y1 = '0;
        assign aligned_h = 1'b0; assign aligned_v = 1'b0; assign raster_locked = 1'b0;
        assign raster_mode = '0; assign raster_line_cycles = '0; assign raster_lines = '0;
        assign raster_hs_cycles = '0;
    end endgenerate

    // ------------------------------------------------------------------
    // Источник кадров: Evo или испытательная таблица (clk_mem)
    // ------------------------------------------------------------------
    wire [1:0] m_src, m_rd;
    wire       m_avg;
    cdc_bus #(.W(5), .INIT({CALIBRATE ? SRC_TEST : DEF_SRC, DEF_RD, DEF_AVG})) cfg_to_mem (
        .clk(clk_mem), .rst_n(rst_mem_n), .d({cfg_src, cfg_rd, cfg_avg}), .q({m_src, m_rd, m_avg})
    );
    wire m_locked;
    cdc_view #(.W(1)) locked_to_mem (.clk(clk_mem), .d(raster_locked), .q(m_locked));

    localparam [26:0] NOSIG_LAST = NOSIG_CYCLES - 1;
    reg [26:0] nosig_cnt;
    reg        no_evo;
    always_ff @(posedge clk_mem or negedge rst_mem_n) begin
        if (!rst_mem_n) begin
            nosig_cnt <= '0; no_evo <= 1'b0; pattern_on <= 1'b0;
        end else begin
            if (m_locked) begin
                nosig_cnt <= '0; no_evo <= 1'b0;
            end else if (nosig_cnt == NOSIG_LAST) begin
                no_evo <= 1'b1;
            end else begin
                nosig_cnt <= nosig_cnt + 27'd1;
            end
            // без захвата Evo (ядро FT812) таблица — только при калибровке и SOURCE = TEST
            pattern_on <= (m_src == SRC_TEST) || (m_src == SRC_AUTO && no_evo && HAS_EVO);
        end
    end

    wire        fb_rd, pat_empty;
    wire [32:0] pat_q;
    reg         pattern_q;           // источник слова, запрошенного тактом раньше
    always_ff @(posedge clk_mem) pattern_q <= pattern_on;

    fb_pattern #(.H_ACTIVE(H_ACTIVE), .V_ACTIVE(V_ACTIVE), .PERIOD(PATTERN_PERIOD)) pattern (
        .clk(clk_mem), .rst_n(rst_mem_n), .enable(pattern_on),
        .rd(fb_rd & pattern_on), .q(pat_q), .empty(pat_empty)
    );
    // Пока пишется таблица, FIFO захвата сливается, чтобы не переполняться
    assign cap_rd = pattern_on ? !cap_empty : fb_rd;
    wire        fb_empty = pattern_on ? pat_empty : cap_empty;
    wire [32:0] fb_q     = pattern_q ? pat_q : cap_q;

    // ------------------------------------------------------------------
    // SDRAM и кадровый буфер (clk_mem)
    // ------------------------------------------------------------------
    // Карта SDRAM (адреса слов): RAM_G FT812 — 0x000000 (3 МБ), образ ПЗУ FT812 — 0x0C0000
    // (1152 КБ), оверлеи ESP — OVL_REGION (512 КБ), кадры — с FB_BASE: три кадра Evo по
    // 1,125 МБ (fb_core) до конца SDRAM; кадры FT — там же. Ядро TS-Config без FT812 (FRAME
    // BLEND, 1.32) держит четвёртый кадр Evo с адреса 0 — на месте RAM_G, которой в нём нет.
    localparam logic [20:0] OVL_REGION = 21'h108000;
    localparam logic [20:0] FB_BASE    = 21'h128000;

    // Клиенты SDRAM: кадровый буфер (fb_core — писатель и читатель), RAM_G FT812 и
    // растеризатор, оверлеи. Метка операции — {владелец[1:0], метка клиента}: 00 fb_core,
    // 01 RAM_G, 10 растеризатор, 11 оверлеи (чтение строк холста — к сроку вывода, запись —
    // от ESP): оверлеи первыми, fb_core и RAM_G — по очереди, растеризатор — когда все молчат.
    wire        mem_req, mem_we, mem_tag, mem_ack, mem_ack_tag, mem_wr_tag, mem_rd_tag;
    wire [2:0]  mem_len;
    wire        mem_wr_take, mem_rd_valid;
    wire [5:0]  mem_wr_idx, mem_rd_idx;
    wire [20:0] mem_addr;
    wire [31:0] mem_wdata, mem_rd_data;
    wire        sd_ack;
    wire [2:0]  sd_ack_tag, sd_wr_tag, sd_rd_tag;   // метки: принятой операции, слова записи, чтения
    wire        mem_wr_take_any, mem_rd_valid_any;
    wire        rd_req, rd_we, rd_sub;
    wire [20:0] rd_addr;
    wire [2:0]  rd_len;
    wire [31:0] rd_wdata;
    wire        ov_req, ov_we, ov_ack, ov_wr_take, ov_rd_valid;
    wire [20:0] ov_addr;
    wire [2:0]  ov_len;
    wire [31:0] ov_wdata;
    wire [3:0]  ov_wmask;
    reg         sd_last_ft;
    always_ff @(posedge clk_mem or negedge rst_mem_n)
        if (!rst_mem_n) sd_last_ft <= 1'b0;
        else if (sd_ack && !sd_ack_tag[2]) sd_last_ft <= sd_ack_tag[2:1] == 2'd1;
    // чтение текселей растеризатора — первым (у моста SD_FAST — своя очередь); иначе оверлеи,
    // RAM_G и вывод по очереди, растеризатор — последним. С 1.28: если у хоста (SPI Evo) в
    // очереди RAM_G 3+ запроса (ft_ramg_urgent), RAM_G — раньше текселей и вывода: иначе при
    // плотном текстурировании очередь хоста переполнялась (FT_STAT 25 на плате, ядро 1.27)
    reg  sd_urg;                       // ft_ramg_urgent тактом позже (короче путь в арбитр)
    always_ff @(posedge clk_mem) sd_urg <= ft_ramg_urgent;
    wire sd_tx     = rd_req && !rd_we && !(ft_g_req && sd_urg);
    wire sd_sel_ov = ov_req && !sd_tx;
    wire sd_sel_ft = !ov_req && !sd_tx && ft_g_req && (sd_urg || !mem_req || !sd_last_ft);
    wire sd_sel_fb = !ov_req && !sd_tx && mem_req && !sd_sel_ft;
    // владелец по метке: 0 fb_core, 1 RAM_G, 2 растеризатор, 3 оверлеи
    wire sd_wr_ft  = sd_wr_tag[2:1] == 2'd1;
    wire sd_wr_rd  = sd_wr_tag[2:1] == 2'd2;
    wire sd_wr_ov  = sd_wr_tag[2:1] == 2'd3;
    assign ov_ack      = sd_ack && sd_ack_tag[2:1] == 2'd3;
    assign ov_wr_take  = mem_wr_take_any && sd_wr_ov;
    assign ov_rd_valid = mem_rd_valid_any && sd_rd_tag[2:1] == 2'd3;
    assign mem_ack       = sd_ack && sd_ack_tag[2:1] == 2'd0;
    assign mem_ack_tag   = sd_ack_tag[0];
    assign mem_wr_tag    = sd_wr_tag[0];
    assign mem_rd_tag    = sd_rd_tag[0];
    assign ft_g_ack      = sd_ack && sd_ack_tag[2:1] == 2'd1;
    assign ft_g_wr_take  = mem_wr_take_any && sd_wr_ft;
    assign ft_g_wr_idx   = mem_wr_idx[2:0];
    assign ft_g_rd_valid = mem_rd_valid_any && sd_rd_tag[2:1] == 2'd1;
    assign ft_g_rd_idx   = mem_rd_idx[2:0];
    assign ft_g_rd_data  = mem_rd_data;
    assign mem_wr_take   = mem_wr_take_any && sd_wr_tag[2:1] == 2'd0;
    assign mem_rd_valid  = mem_rd_valid_any && sd_rd_tag[2:1] == 2'd0;

    wire        sd_req  = mem_req || ft_g_req || rd_req || ov_req;
    wire        sd_we   = sd_sel_ov ? ov_we : sd_sel_ft ? ft_g_we : sd_sel_fb ? mem_we : rd_we;
    wire [20:0] sd_addr = sd_sel_ov ? ov_addr : sd_sel_ft ? ft_g_addr : sd_sel_fb ? mem_addr : rd_addr;
    wire [2:0]  sd_len  = sd_sel_ov ? ov_len : sd_sel_ft ? 3'd0 : sd_sel_fb ? mem_len : rd_len;
    wire [2:0]  sd_tag  = sd_sel_ov ? 3'b110 : sd_sel_ft ? 3'b010 : sd_sel_fb ? {2'b00, mem_tag} : {2'b10, rd_sub};
    wire [31:0] sd_wdata = sd_wr_ov ? ov_wdata : sd_wr_ft ? ft_g_wdata : sd_wr_rd ? rd_wdata : mem_wdata;
    wire [3:0]  sd_wmask = sd_wr_ov ? ov_wmask : sd_wr_ft ? ft_g_wmask : 4'hF;

    generate if (SD_FAST) begin : g_sd_fast
        wire        c_req, c_we, c_ack, c_wr_take, c_rd_valid, c_init;
        wire [20:0] c_addr;
        wire [2:0]  c_len;
        wire [31:0] c_wdata, c_rd_data;
        wire [3:0]  c_wmask;
        wire [1:0]  c_rd_phase;
        sd_bridge #(.TAGW(3)) bridge (
            .clk(clk_mem), .rst_n(rst_mem_n), .clk_sd(clk_sd), .rst_sd_n(rst_sd_n),
            .req(sd_req), .we(sd_we), .addr(sd_addr), .req_len(sd_len), .req_tag(sd_tag),
            .ack(sd_ack), .ack_tag(sd_ack_tag),
            .wr_take(mem_wr_take_any), .wr_idx(mem_wr_idx), .wr_tag(sd_wr_tag),
            .wdata(sd_wdata), .wmask(sd_wmask),
            .rd_valid(mem_rd_valid_any), .rd_idx(mem_rd_idx), .rd_tag(sd_rd_tag), .rd_data(mem_rd_data),
            .rd_phase(m_rd), .init_done(mem_ready),
            .c_req(c_req), .c_we(c_we), .c_addr(c_addr), .c_len(c_len), .c_ack(c_ack),
            .c_wr_take(c_wr_take), .c_wdata(c_wdata), .c_wmask(c_wmask),
            .c_rd_valid(c_rd_valid), .c_rd_data(c_rd_data), .c_rd_phase(c_rd_phase), .c_init_done(c_init)
        );
        // такты из паспортных времён на 2 × MEM_KHZ (123,75 МГц: tRCD, tRP — 2, tWR — 2, tRFC — 8), CL 3
        localparam int SD_KHZ = 2 * MEM_KHZ;
        localparam int SD_RCD = (15 * SD_KHZ + 999_999) / 1_000_000;
        localparam int SD_WR  = (12 * SD_KHZ + 999_999) / 1_000_000;
        localparam int SD_RFC = (60 * SD_KHZ + 999_999) / 1_000_000;
        sdram_burst #(.TAGW(1), .CLK_KHZ(SD_KHZ), .CL(3), .T_RCD(SD_RCD < 2 ? 2 : SD_RCD), .T_RP(SD_RCD),
                      .T_WR(SD_WR), .T_RFC(SD_RFC), .INIT_US(SDRAM_INIT_US)) sdram (
            .clk(clk_sd), .rst_n(rst_sd_n), .rd_phase(c_rd_phase),
            .req(c_req), .we(c_we), .addr(c_addr), .req_len(c_len), .req_tag(1'b0),
            .ack(c_ack), .op_tag(), .wr_take(c_wr_take), .wr_idx(), .wdata(c_wdata), .wmask(c_wmask),
            .rd_valid(c_rd_valid), .rd_idx(), .rd_data(c_rd_data), .init_done(c_init),
            .sdram_cs_n(sdram_cs_n), .sdram_ras_n(sdram_ras_n), .sdram_cas_n(sdram_cas_n),
            .sdram_we_n(sdram_we_n), .sdram_a(sdram_a), .sdram_ba(sdram_ba), .sdram_dqm(sdram_dqm),
            .sdram_dq_out(sdram_dq_out), .sdram_dq_oe(sdram_dq_oe), .sdram_dq_in(sdram_dq_in)
        );
    end else begin : g_sd
        wire [2:0] op_tag;
        sdram_burst #(.TAGW(3), .CLK_KHZ(MEM_KHZ), .INIT_US(SDRAM_INIT_US)) sdram (
            .clk(clk_mem), .rst_n(rst_mem_n), .rd_phase(m_rd),
            .req(sd_req), .we(sd_we), .addr(sd_addr), .req_len(sd_len), .req_tag(sd_tag),
            .ack(sd_ack), .op_tag(op_tag),
            .wr_take(mem_wr_take_any), .wr_idx(mem_wr_idx), .wdata(sd_wdata), .wmask(sd_wmask),
            .rd_valid(mem_rd_valid_any), .rd_idx(mem_rd_idx), .rd_data(mem_rd_data),
            .init_done(mem_ready),
            .sdram_cs_n(sdram_cs_n), .sdram_ras_n(sdram_ras_n), .sdram_cas_n(sdram_cas_n),
            .sdram_we_n(sdram_we_n), .sdram_a(sdram_a), .sdram_ba(sdram_ba), .sdram_dqm(sdram_dqm),
            .sdram_dq_out(sdram_dq_out), .sdram_dq_oe(sdram_dq_oe), .sdram_dq_in(sdram_dq_in)
        );
        // одна операция за раз: метка принятой — она же у слов записи и чтения
        assign sd_ack_tag = op_tag;
        assign sd_wr_tag  = op_tag;
        assign sd_rd_tag  = op_tag;
    end endgenerate

    wire        req_toggle, req_first, req_two, req_ft;
    wire [9:0]  req_line;

    // Растеризатор FT812: кадр FT в SDRAM на месте кадров Evo (при M_SEL = 1)
    wire        ft_showing, ft_pub_v;
    wire [1:0]  ft_front, ft_pub_buf;
    wire [9:0]  ft_pub_words, ft_pub_h;
    wire [10:0] ft_pub_w;
    wire [31:0] rd_status;
    wire [15:0] rd_time;
    generate if (HAS_FT) begin : g_render
        ft_render #(.FB_BASE(FB_BASE)) render (
            .clk(clk_mem), .rst_n(rst_mem_n),
            .enable(m_ft && ft_disp_on), .hsize(ft_hsize), .vsize(ft_vsize),
            .ramg_3mb(ft_cfg_3mb_s[1]), .dither(!ft_cfg_nodith_s[1]),
            .dl_addr(rd_dl_addr), .dl_q(rd_dl_q), .swap_hold(rd_swap_hold), .swap_wait(ft_dlswap != 2'd0),
            .s_req(rd_req), .s_we(rd_we), .s_addr(rd_addr), .s_len(rd_len), .s_sub(rd_sub),
            .s_ack(sd_ack && sd_ack_tag[2:1] == 2'd2), .s_op_sub(sd_ack_tag[0]),
            .s_wr_take(mem_wr_take_any && sd_wr_tag == 3'b101), .s_wdata(rd_wdata),
            .s_rd_valid(mem_rd_valid_any && sd_rd_tag == 3'b100), .s_rd_idx(mem_rd_idx[2:0]),
            .s_rd_data(mem_rd_data),
            .fb_showing(ft_showing), .fb_front(ft_front),
            .pub_v(ft_pub_v), .pub_buf(ft_pub_buf), .pub_words(ft_pub_words), .pub_w(ft_pub_w),
            .pub_h(ft_pub_h), .status(rd_status), .frame_time(rd_time)
        );
    end else begin : g_no_render
        assign rd_req = 1'b0; assign rd_we = 1'b0; assign rd_addr = '0; assign rd_len = '0;
        assign rd_sub = 1'b0; assign rd_wdata = '0; assign rd_dl_addr = '0;
        assign ft_pub_v = 1'b0; assign ft_pub_buf = 2'd0; assign ft_pub_words = '0;
        assign ft_pub_w = '0; assign ft_pub_h = '0; assign rd_status = '0; assign rd_time = '0;
    end endgenerate
    wire        lb_we;
    wire [10:0] lb_waddr;
    wire [9:0]  lb_ra_e, lb_ra_o, lb_rb_e, lb_rb_o;
    wire [31:0] lb_wdata;
    wire [15:0] lb_qa_e, lb_qa_o, lb_qb_e, lb_qb_o;

    // FRAME BLEND — только в ядре с Evo без FT812: четвёртый кадр Evo лежит с адреса 0 (место
    // RAM_G, в этом ядре его нет); на испытательной таблице (калибровка, нет Evo) не смешивается
    wire avg_shown;
    fb_core #(.H_ACTIVE(H_ACTIVE), .V_ACTIVE(V_ACTIVE), .BASE(FB_BASE),
              .AVG(HAS_EVO && !HAS_FT), .BUF3_BASE(21'h000000)) fb (
        .clk(clk_mem), .rst_n(rst_mem_n),
        .fifo_rd(fb_rd), .fifo_q(fb_q), .fifo_empty(fb_empty),
        .req_toggle(req_toggle), .req_line(req_line), .req_first(req_first),
        .req_two(req_two), .req_ft(req_ft), .avg_on(m_avg && !pattern_on),
        .ft_mode(m_ft), .ft_pub_v(ft_pub_v), .ft_pub_buf(ft_pub_buf), .ft_words(ft_pub_words),
        .ft_front(ft_front), .ft_showing(ft_showing),
        .lb_we(lb_we), .lb_addr(lb_waddr), .lb_wdata(lb_wdata),
        .mem_req(mem_req), .mem_we(mem_we), .mem_addr(mem_addr), .mem_tag(mem_tag),
        .mem_len(mem_len), .mem_ack(mem_ack), .mem_ack_tag(mem_ack_tag), .mem_wr_tag(mem_wr_tag),
        .mem_rd_tag(mem_rd_tag),
        .mem_wr_take(mem_wr_take), .mem_wr_idx(mem_wr_idx), .mem_wdata(mem_wdata),
        .mem_rd_valid(mem_rd_valid), .mem_rd_data(mem_rd_data),
        .frames_written(frames_written), .frames_rejected(frames_rejected),
        .fetch_late(fetch_late), .have_frame(have_frame), .avg_shown(avg_shown)
    );

    fb_line_ram line_ram (
        .wr_clk(clk_mem), .we(lb_we), .waddr(lb_waddr), .wdata(lb_wdata),
        .rd_clk(pixel_clk), .ra_e(lb_ra_e), .ra_o(lb_ra_o), .rb_e(lb_rb_e), .rb_o(lb_rb_o),
        .qa_e(lb_qa_e), .qa_o(lb_qa_o), .qb_e(lb_qb_e), .qb_o(lb_qb_o)
    );

    // ------------------------------------------------------------------
    // Развёртка, меню и TMDS (pixel_clk)
    // ------------------------------------------------------------------
    wire [7:0] s_r, s_g, s_b;
    wire       s_de, s_hs, s_vs;
    // M_SEL (blank = 1 — монитор у FT812): общее ядро в 720p показывает кадр FT, без масштаба —
    // чёрное поле; ядро без FT812 — чёрное поле; ядро FT812 — кадр FT, при M_SEL = 0 — чёрное
    // поле (картинки TS-Config в нём нет), испытательная таблица — всегда. Калибровка выборки
    // SDRAM (после каждой загрузки ядра, ~0,4 с) сверяет таблицу по чтению, а на выход — чёрное
    // поле (с 1.23): с загрузкой слота 0,2 с таблица мигала при каждом старте
    wire sc_ft    = HAS_FT && SCALE && (HAS_EVO ? blank : !p_pattern_on);
    wire sc_blank = cal_busy ||
                    (!HAS_FT ? blank : HAS_EVO ? (SCALE ? 1'b0 : blank) : (!p_pattern_on && !blank));

    fb_scanout #(
        .H_ACTIVE(H_ACTIVE), .V_ACTIVE(V_ACTIVE),
        .OUT_W(OUT_W), .OUT_H(OUT_H), .IMG_X(IMG_X), .IMG_Y(IMG_Y),
        .H_FP(H_FP), .H_SYNC(H_SYNC), .H_BP(H_BP),
        .V_FP(V_FP), .V_SYNC(V_SYNC), .V_BP(V_BP),
        .HS_NEG(HS_NEG), .VS_NEG(VS_NEG), .SCALE(SCALE)
    ) scanout (
        .pixel_clk(pixel_clk), .rst_n(rst_pix_n), .blank(sc_blank),
        .aspect(cfg_aspect), .size(cfg_size),
        .ft(sc_ft), .src_w(p_rd_w), .src_h(p_rd_h),
        .req_toggle(req_toggle), .req_line(req_line), .req_first(req_first),
        .req_two(req_two), .req_ft(req_ft),
        .lb_ra_e(lb_ra_e), .lb_ra_o(lb_ra_o), .lb_rb_e(lb_rb_e), .lb_rb_o(lb_rb_o),
        .lb_qa_e(lb_qa_e), .lb_qa_o(lb_qa_o), .lb_qb_e(lb_qb_e), .lb_qb_o(lb_qb_o),
        .out_r(s_r), .out_g(s_g), .out_b(s_b),
        .out_de(s_de), .out_hs(s_hs), .out_vs(s_vs),
        .chk_err(chk_err), .chk_tick(chk_tick)
    );

    // Растеризатор — в домен пикселей (LINK и размер кадра FT для развёртки; меняется редко)
    cdc_view #(.W(69)) rd_to_pix (
        .clk(pixel_clk), .d({rd_status, rd_time, ft_pub_w, ft_pub_h}),
        .q({p_rd_status, p_rd_time, p_rd_w, p_rd_h})
    );

    // Состояние захвата и SDRAM — в домен пикселей (для меню и калибровки)
    cdc_view #(.W(37)) mem_to_pix (
        .clk(pixel_clk),
        .d({frames_written, frames_rejected, fetch_late, have_frame, mem_ready, pattern_on, avg_shown}),
        .q({p_frames_written, p_frames_rejected, p_fetch_late, p_have_frame, p_mem_ready, p_pattern_on,
            p_avg_shown})
    );
    cdc_view #(.W(111)) cap_to_pix (
        .clk(pixel_clk),
        .d({raster_locked, raster_mode, raster_line_cycles, raster_lines, raster_hs_cycles,
            hs_low, vs_low, h_off, v_off, aligned_h, aligned_v, bb_valid,
            bb_x0, bb_x1, bb_y0, bb_y1, cap_overflow}),
        .q({p_locked, p_mode, p_line_cycles, p_lines, p_hs_cycles,
            p_hs_low, p_vs_low, p_h_off, p_v_off, p_aligned_h, p_aligned_v, p_bb_valid,
            p_bb_x0, p_bb_x1, p_bb_y0, p_bb_y1, p_overflow})
    );
    generate if (HAS_EVO) begin : g_meter
        pclk_meter #(.WINDOW(PIX_KHZ)) meter (
            .pixel_clk(pixel_clk), .rst_pix_n(rst_pix_n),
            .clk_cap(clk_cap), .rst_cap_n(rst_cap_n), .khz(pclk_khz)
        );
    end else begin : g_no_meter
        assign pclk_khz = '0;
    end endgenerate

    // Оверлеи ESP поверх картинки (меню настроек, обложка, спектр) — fb_ovl; только в ядре с
    // Evo: ядру FT812 меню и оверлеи не нужны (решение 2026-10-19), их место — растеризатору
    generate if (HAS_EVO) begin : g_ovl
    fb_ovl #(.N(NOVL), .REGION(OVL_REGION), .HS_NEG(HS_NEG), .VS_NEG(VS_NEG)) ovl (
        .pclk(pixel_clk), .prst_n(rst_pix_n),
        .cfg_on(ovl_on), .cfg_x2(ovl_x2), .cfg_alpha(ovl_alpha), .cfg_x(ovl_x), .cfg_y(ovl_y),
        .cfg_w(ovl_w), .cfg_h(ovl_h), .cfg_base(ovl_base),
        .ow_stb(ov_stb), .ow_addr(lk_wr_word[16:0]), .ow_data(lk_wr_data), .ow_ovf(ovl_ovf),
        .in_r(s_r), .in_g(s_g), .in_b(s_b), .in_de(s_de), .in_hs(s_hs), .in_vs(s_vs),
        .out_r(vid_r), .out_g(vid_g), .out_b(vid_b),
        .out_de(vid_de), .out_hs(vid_hs), .out_vs(vid_vs),
        .mclk(clk_mem), .mrst_n(rst_mem_n),
        .s_req(ov_req), .s_we(ov_we), .s_addr(ov_addr), .s_len(ov_len), .s_ack(ov_ack),
        .s_wr_take(ov_wr_take), .s_wr_idx(mem_wr_idx), .s_wdata(ov_wdata), .s_wmask(ov_wmask),
        .s_rd_valid(ov_rd_valid), .s_rd_idx(mem_rd_idx), .s_rd_data(mem_rd_data)
    );
    end else begin : g_no_ovl
        assign {vid_r, vid_g, vid_b, vid_de, vid_hs, vid_vs} = {s_r, s_g, s_b, s_de, s_hs, s_vs};
        assign ov_req = 1'b0;
        assign ov_we = 1'b0;
        assign ov_addr = '0;
        assign ov_len = '0;
        assign ov_wdata = '0;
        assign ov_wmask = '0;
        assign ovl_ovf = 1'b0;
    end endgenerate

    // TMDS: DVI или HDMI с AVI InfoFrame; синхроимпульсы — в управляющих битах синего канала
    hdmi_tx #(
        .HDMI(HDMI), .VS_HIGH(!VS_NEG),
        .VIC(SCALE ? (HAS_EVO ? 8'd19 : 8'd4) : 8'd17), .PIC_AR(SCALE ? 2'b10 : 2'b01)   // 4 — 720p60 (ядро FT812)
    ) tmds (
        .pixel_clk(pixel_clk), .reset_n(rst_pix_n),
        .r(vid_r), .g(vid_g), .b(vid_b), .de(vid_de), .hs(vid_hs), .vs(vid_vs),
        .tmds_blue(tmds_blue), .tmds_green(tmds_green), .tmds_red(tmds_red)
    );
endmodule
