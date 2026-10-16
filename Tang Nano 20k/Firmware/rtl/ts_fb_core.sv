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
// Наладка — кнопками Tang через экранное меню (fb_ui, fb_osd): фаза и задержка
// мультиплексора, поправка окна, точка выборки SDRAM, источник. После сброса
// калибруется точка выборки чтения SDRAM (CALIBRATE). Те же настройки и всё состояние
// доступны ESP по LINK (fb_link) — карта регистров ниже, у мультиплексора чтения.
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
    // Меню и кнопки (такты пиксельного клока на 1 мс и пр. — см. fb_ui)
    parameter bit OSD            = 1'b1,
    parameter int PIX_KHZ        = 27_000,
    parameter int DEBOUNCE_MS    = 10,
    parameter int CAL_TIMEOUT_MS = 3_000,
    // Значения по умолчанию (то же, что после сброса показывает меню)
    parameter logic [1:0] DEF_SRC   = 2'd0,  // 0 AUTO, 1 EVO, 2 TEST
    parameter bit         DEF_PHASE = 1'b1,  // фаза мультиплексора в начале строки
    parameter int         DEF_DELAY = 1,     // задержка выборки мультиплексора, такты 56 МГц (1…4)
    parameter logic [3:0] DEF_CLOCK = 4'd0,  // фаза клока приёма, 1/16 периода 56 МГц
    parameter logic [1:0] DEF_RD    = 2'd1,  // точка выборки чтения SDRAM (sdram_burst)
    parameter bit         DEF_ALIGN = 1'b1,
    parameter bit         DEF_ASPECT = 1'b0, // 0 — 4:3, 1 — 16:9 (только SCALE)
    parameter logic [1:0] DEF_SIZE  = 2'd0,  // высота картинки: 0 — 100 %, 1 — 95 %, 2 — 90 %
    parameter bit         CALIBRATE = 1'b1,
    // Картинка на всю высоту кадра 1280×720 (fb_scanout); 0 — точка в точку с (IMG_X, IMG_Y)
    parameter bit         SCALE = 1'b0,
    // Выход HDMI с пакетом AVI InfoFrame (hdmi_tx); 0 — DVI
    parameter bit         HDMI = 1'b0,
    // Flash Tang: сектор настроек и ожидание перед их чтением после сброса
    parameter logic [23:0] FLASH_SET_ADDR = 24'h7F_F000,
    parameter int          FLASH_BOOT_US  = 1_000
) (
    input  wire        clk_cap,
    input  wire        rst_cap_n,
    input  wire        clk_mem,
    input  wire        rst_mem_n,
    input  wire        pixel_clk,
    input  wire        rst_pix_n,
    input  wire        blank,          // домен pixel_clk: 1 — чёрное поле (режим FT812)
    input  wire [1:0]  btn,            // кнопки S1, S2 как есть (любой активный уровень)
    output wire [3:0]  cap_clk_phase,  // домен пикселей: фаза PLL захвата (MUX CLOCK)

    // LINK к ESP (QSPI, ESP — ведущий)
    input  wire        link_sck,
    input  wire        link_cs_n,
    input  wire [3:0]  link_io_in,
    output wire [3:0]  link_io_out,
    output wire        link_io_oe,

    // SPI-flash Tang (выводы MSPI)
    output wire        flash_sck,
    output wire        flash_cs_n,
    output wire        flash_mosi,
    input  wire        flash_miso,

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
    // Меню и кнопки (pixel_clk)
    // ------------------------------------------------------------------
    wire [1:0]        set_src, cfg_src, cfg_delay, cfg_rd, cal_point;
    wire              cfg_phase, cfg_align, cfg_aspect, osd_on, cal_busy, cal_done, cal_fail;
    wire signed [7:0] cfg_h_adj, cfg_v_adj;
    wire [3:0]        cfg_clock;
    wire [1:0]        cfg_size;
    wire [3:0]        cursor;
    assign cap_clk_phase = cfg_clock;
    wire [3:0]        cal_ok;

    // Состояние в домене пикселей
    wire [15:0] p_frames_written, p_frames_rejected;
    wire        p_fetch_late, p_have_frame, p_mem_ready, p_pattern_on;
    wire        p_locked, p_hs_low, p_vs_low, p_aligned_h, p_aligned_v, p_bb_valid, p_overflow;
    wire [2:0]  p_mode;
    wire [11:0] p_line_cycles, p_hs_cycles;
    wire [10:0] p_lines, p_h_off, p_v_off, p_bb_x0, p_bb_x1, p_bb_y0, p_bb_y1;
    wire [15:0] pclk_khz;

    // ------------------------------------------------------------------
    // LINK: регистры для ESP (домен пикселей). Адрес — байтовый, регистр 32 бита.
    //   0x00 ID       R   0x56444334 ("VDC4")
    //   0x04 VERSION  R   {личность 8'h01 (VDAC2, кадровый буфер), протокол LINK 8'h01, версия 16'h0103}
    //                     (1.1 — flash Tang и поправки по растрам; 1.2 — ASPECT и SIZE; 1.3 — UI_KEY)
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
    //   0x24 CHECK    R   [15:0] расхождений таблицы за кадр; 16 строка не успела; 17 FIFO переполнялся
    //   0x3C SCRATCH  RW  проверка канала: что записано, то и читается
    //   0x40 CTRL     RW  [1:0] источник (0 AUTO, 1 EVO, 2 TEST); 2 MUX PHASE; [4:3] MUX DELAY − 1;
    //                     5 AUTO ALIGN; [7:6] SD READ; 8 меню на экране; 9 — запуск калибровки
    //                     (при записи; читается 1, пока она идёт); [13:10] MUX CLOCK;
    //                     14 ASPECT (0 — 4:3, 1 — 16:9); [16:15] SIZE (0 — 100 %, 1 — 95 %, 2 — 90 %)
    //   0x44 ADJUST   RW  [7:0] H ADJUST, [15:8] V ADJUST (со знаком) — текущего растра
    //   0x48 FLASH_CMD  W  мост к flash: [6:0] передать байтов буфера, [14:8] принять в буфер,
    //                     16 — оставить CS активным (fb_flash); читается как FLASH_STATUS
    //   0x4C FLASH_STATUS R 0 занят, 1 настройки прочитаны, 2 найдены, 3 CS удерживается;
    //                     [15:8] ячейка загруженных настроек; [23:16] первая чистая ячейка
    //   0x50 UI_KEY    W  нажатие для экранного меню (как кнопки Tang): 1 — следующий пункт,
    //                     2 — предыдущий, 3 — значение +1, 4 — −1, 5 — показать меню, 6 — скрыть
    //   0x60…0x7C ADJ_TAB RW поправки растров 0…7: [7:0] H, [15:8] V (0x60 + 4 × режим)
    //   0x80…0xBC FLASH_BUF RW буфер моста, 64 байта (байт 0 — старший байт 0x80)
    // ------------------------------------------------------------------
    wire        lk_wr_stb;
    wire [7:0]  lk_wr_addr, lk_rd_addr;
    wire [31:0] lk_wr_data;
    reg  [31:0] lk_rd_data;
    reg  [31:0] lk_scratch;

    fb_link link (
        .sck(link_sck), .cs_n(link_cs_n),
        .io_in(link_io_in), .io_out(link_io_out), .io_oe(link_io_oe),
        .clk(pixel_clk), .rst_n(rst_pix_n),
        .wr_stb(lk_wr_stb), .wr_addr(lk_wr_addr), .wr_data(lk_wr_data),
        .rd_addr(lk_rd_addr), .rd_data(lk_rd_data)
    );

    always_ff @(posedge pixel_clk or negedge rst_pix_n) begin
        if (!rst_pix_n) lk_scratch <= '0;
        else if (lk_wr_stb && lk_wr_addr == 8'h0F) lk_scratch <= lk_wr_data;
    end

    // Записи настроек меню (CTRL, ADJUST, таблица поправок, UI_KEY) — тактом позже:
    // сравнение адреса отдельной ступенью, иначе путь от адреса LINK до регистров fb_ui
    // едва укладывается в такт 74,25 МГц
    reg        ui_ctrl_we, ui_adj_we, ui_tab_we, ui_key_we;
    reg [2:0]  ui_tab_idx;
    reg [31:0] ui_wr_data;
    always_ff @(posedge pixel_clk or negedge rst_pix_n) begin
        if (!rst_pix_n) begin
            ui_ctrl_we <= 1'b0; ui_adj_we <= 1'b0; ui_tab_we <= 1'b0; ui_key_we <= 1'b0;
            ui_tab_idx <= '0; ui_wr_data <= '0;
        end else begin
            ui_ctrl_we <= lk_wr_stb && lk_wr_addr == 8'h10;
            ui_adj_we  <= lk_wr_stb && lk_wr_addr == 8'h11;
            ui_tab_we  <= lk_wr_stb && lk_wr_addr[7:3] == 5'b00011;
            ui_key_we  <= lk_wr_stb && lk_wr_addr == 8'h14;
            ui_tab_idx <= lk_wr_addr[2:0];
            ui_wr_data <= lk_wr_data;
        end
    end

    // Flash Tang: настройки и мост
    wire [31:0]  fl_status, fl_buf_rdata;
    wire         fl_load_stb;
    wire [10:0]  fl_load_cfg;
    wire [127:0] fl_load_adj, adj_tab;

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

    always_comb begin
        case (lk_rd_addr)
            8'h00: lk_rd_data = 32'h5644_4334;
            8'h01: lk_rd_data = {8'h01, 8'h01, 16'h0103};
            8'h02: lk_rd_data = {pclk_khz, cal_fail, cal_done, cal_busy, blank, p_have_frame,
                                 p_mem_ready, p_pattern_on, p_bb_valid, p_aligned_v, p_aligned_h,
                                 p_vs_low, p_hs_low, p_mode, p_locked};
            8'h03: lk_rd_data = {5'd0, p_lines, 4'd0, p_line_cycles};
            8'h04: lk_rd_data = {12'd0, cal_ok, 4'd0, p_hs_cycles};
            8'h05: lk_rd_data = {5'd0, p_v_off, 5'd0, p_h_off};
            8'h06: lk_rd_data = {5'd0, p_bb_x1, 5'd0, p_bb_x0};
            8'h07: lk_rd_data = {5'd0, p_bb_y1, 5'd0, p_bb_y0};
            8'h08: lk_rd_data = {p_frames_rejected, p_frames_written};
            8'h09: lk_rd_data = {14'd0, p_overflow, p_fetch_late, chk_err};
            8'h0F: lk_rd_data = lk_scratch;
            8'h10: lk_rd_data = {15'd0, cfg_size, cfg_aspect, cfg_clock, cal_busy, osd_on, cfg_rd, cfg_align, cfg_delay, cfg_phase, set_src};
            8'h11: lk_rd_data = {16'd0, cfg_v_adj, cfg_h_adj};
            8'h12, 8'h13: lk_rd_data = fl_status;
            default:
                if (lk_rd_addr[7:3] == 5'b00011)  lk_rd_data = {16'd0, adj_tab[lk_rd_addr[2:0] * 16 +: 16]};
                else if (lk_rd_addr[7:4] == 4'h2) lk_rd_data = fl_buf_rdata;
                else                              lk_rd_data = 32'd0;
        endcase
    end

    fb_ui #(
        .TICK(PIX_KHZ), .DEBOUNCE_MS(DEBOUNCE_MS), .CAL_TIMEOUT_MS(CAL_TIMEOUT_MS),
        .DEF_SRC(DEF_SRC), .DEF_PHASE(DEF_PHASE), .DEF_DELAY(DEF_DELAY_CODE), .DEF_CLOCK(DEF_CLOCK),
        .DEF_RD(DEF_RD), .DEF_ALIGN(DEF_ALIGN), .DEF_ASPECT(DEF_ASPECT), .DEF_SIZE(DEF_SIZE),
        .CALIBRATE(CALIBRATE),
        .OSD_START(OSD)
    ) ui (
        .clk(pixel_clk), .rst_n(rst_pix_n), .btn(btn),
        .chk_tick(chk_tick), .chk_err(chk_err), .pattern_on(p_pattern_on),
        .frames_written(p_frames_written),
        .picture_ok(!p_pattern_on && p_locked && !blank),
        .ext_ctrl_we(ui_ctrl_we), .ext_ctrl(ui_wr_data[16:0]),
        .ext_adj_we(ui_adj_we),
        .ext_h_adj(ui_wr_data[7:0]), .ext_v_adj(ui_wr_data[15:8]),
        .ext_tab_we(ui_tab_we), .ext_tab_idx(ui_tab_idx),
        .ext_tab_data(ui_wr_data[15:0]),
        .ext_key_we(ui_key_we), .ext_key(ui_wr_data[2:0]),
        .ext_load_stb(fl_load_stb), .ext_load_cfg(fl_load_cfg), .ext_load_adj(fl_load_adj),
        .mode(p_mode), .adj_tab(adj_tab),
        .set_src(set_src), .cfg_src(cfg_src), .cfg_phase(cfg_phase), .cfg_delay(cfg_delay),
        .cfg_clock(cfg_clock), .cfg_h_adj(cfg_h_adj), .cfg_v_adj(cfg_v_adj), .cfg_align(cfg_align),
        .cfg_aspect(cfg_aspect), .cfg_size(cfg_size), .cfg_rd(cfg_rd),
        .cursor(cursor), .osd_on(osd_on), .cal_busy(cal_busy), .cal_point(cal_point),
        .cal_ok(cal_ok), .cal_done(cal_done), .cal_fail(cal_fail)
    );

    // ------------------------------------------------------------------
    // Приём и упаковка (clk_cap)
    // ------------------------------------------------------------------
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
    wire       hs_level, vs_level, hs_low, vs_low, frame_ok;
    wire [10:0] h_off, v_off;

    ts_mux_capture capture (
        .clk_cap(clk_cap), .rst_n(rst_cap_n),
        .sample_delay(c_delay), .sel_phase(c_phase),
        .mux_d(mux_d), .hsync_in(hsync_in), .vsync_in(vsync_in), .mux_sel(mux_sel),
        .hs_active_low(hs_low), .vs_active_low(vs_low),
        .hs_level(hs_level), .vs_level(vs_level),
        .pix_valid(pix_valid), .pix_r(pix_r), .pix_g(pix_g), .pix_b(pix_b), .pix_pal(pix_pal),
        .hs_active(hs_active), .vs_active(vs_active), .hs_start(hs_start), .vs_start(vs_start)
    );

    wire        fifo_wr, fifo_full, cap_rd, cap_empty;
    wire [32:0] fifo_wdata, cap_q;

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

    // ------------------------------------------------------------------
    // Источник кадров: Evo или испытательная таблица (clk_mem)
    // ------------------------------------------------------------------
    wire [1:0] m_src, m_rd;
    cdc_bus #(.W(4), .INIT({CALIBRATE ? SRC_TEST : DEF_SRC, DEF_RD})) cfg_to_mem (
        .clk(clk_mem), .rst_n(rst_mem_n), .d({cfg_src, cfg_rd}), .q({m_src, m_rd})
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
            pattern_on <= (m_src == SRC_TEST) || (m_src == SRC_AUTO && no_evo);
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
    wire        mem_req, mem_we, mem_tag, mem_ack, mem_op_tag;
    wire        mem_wr_take, mem_rd_valid;
    wire [2:0]  mem_wr_idx, mem_rd_idx;
    wire [20:0] mem_addr;
    wire [31:0] mem_wdata, mem_rd_data;

    sdram_burst #(.CLK_KHZ(MEM_KHZ), .INIT_US(SDRAM_INIT_US)) sdram (
        .clk(clk_mem), .rst_n(rst_mem_n), .rd_phase(m_rd),
        .req(mem_req), .we(mem_we), .addr(mem_addr), .req_tag(mem_tag),
        .ack(mem_ack), .op_tag(mem_op_tag),
        .wr_take(mem_wr_take), .wr_idx(mem_wr_idx), .wdata(mem_wdata),
        .rd_valid(mem_rd_valid), .rd_idx(mem_rd_idx), .rd_data(mem_rd_data),
        .init_done(mem_ready),
        .sdram_cs_n(sdram_cs_n), .sdram_ras_n(sdram_ras_n), .sdram_cas_n(sdram_cas_n),
        .sdram_we_n(sdram_we_n), .sdram_a(sdram_a), .sdram_ba(sdram_ba), .sdram_dqm(sdram_dqm),
        .sdram_dq_out(sdram_dq_out), .sdram_dq_oe(sdram_dq_oe), .sdram_dq_in(sdram_dq_in)
    );

    wire        req_toggle, req_first;
    wire [9:0]  req_line;
    wire        lb_we;
    wire [10:0] lb_waddr;
    wire [9:0]  lb_ra_e, lb_ra_o, lb_rb_e, lb_rb_o;
    wire [31:0] lb_wdata;
    wire [15:0] lb_qa_e, lb_qa_o, lb_qb_e, lb_qb_o;

    fb_core #(.H_ACTIVE(H_ACTIVE), .V_ACTIVE(V_ACTIVE)) fb (
        .clk(clk_mem), .rst_n(rst_mem_n),
        .fifo_rd(fb_rd), .fifo_q(fb_q), .fifo_empty(fb_empty),
        .req_toggle(req_toggle), .req_line(req_line), .req_first(req_first),
        .lb_we(lb_we), .lb_addr(lb_waddr), .lb_wdata(lb_wdata),
        .mem_req(mem_req), .mem_we(mem_we), .mem_addr(mem_addr), .mem_tag(mem_tag),
        .mem_ack(mem_ack), .mem_op_tag(mem_op_tag),
        .mem_wr_take(mem_wr_take), .mem_wr_idx(mem_wr_idx), .mem_wdata(mem_wdata),
        .mem_rd_valid(mem_rd_valid), .mem_rd_data(mem_rd_data),
        .frames_written(frames_written), .frames_rejected(frames_rejected),
        .fetch_late(fetch_late), .have_frame(have_frame)
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

    fb_scanout #(
        .H_ACTIVE(H_ACTIVE), .V_ACTIVE(V_ACTIVE),
        .OUT_W(OUT_W), .OUT_H(OUT_H), .IMG_X(IMG_X), .IMG_Y(IMG_Y),
        .H_FP(H_FP), .H_SYNC(H_SYNC), .H_BP(H_BP),
        .V_FP(V_FP), .V_SYNC(V_SYNC), .V_BP(V_BP),
        .HS_NEG(HS_NEG), .VS_NEG(VS_NEG), .SCALE(SCALE)
    ) scanout (
        .pixel_clk(pixel_clk), .rst_n(rst_pix_n), .blank(blank), .aspect(cfg_aspect), .size(cfg_size),
        .req_toggle(req_toggle), .req_line(req_line), .req_first(req_first),
        .lb_ra_e(lb_ra_e), .lb_ra_o(lb_ra_o), .lb_rb_e(lb_rb_e), .lb_rb_o(lb_rb_o),
        .lb_qa_e(lb_qa_e), .lb_qa_o(lb_qa_o), .lb_qb_e(lb_qb_e), .lb_qb_o(lb_qb_o),
        .out_r(s_r), .out_g(s_g), .out_b(s_b),
        .out_de(s_de), .out_hs(s_hs), .out_vs(s_vs),
        .chk_err(chk_err), .chk_tick(chk_tick)
    );

    // Состояние захвата и SDRAM — в домен пикселей (для меню и калибровки)
    cdc_view #(.W(36)) mem_to_pix (
        .clk(pixel_clk),
        .d({frames_written, frames_rejected, fetch_late, have_frame, mem_ready, pattern_on}),
        .q({p_frames_written, p_frames_rejected, p_fetch_late, p_have_frame, p_mem_ready, p_pattern_on})
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
    pclk_meter #(.WINDOW(PIX_KHZ)) meter (
        .pixel_clk(pixel_clk), .rst_pix_n(rst_pix_n),
        .clk_cap(clk_cap), .rst_cap_n(rst_cap_n), .khz(pclk_khz)
    );

    generate if (OSD) begin : g_osd
        fb_osd #(.OX(IMG_X + 40), .OY(IMG_Y + 24), .VS_NEG(VS_NEG)) osd (
            .clk(pixel_clk), .rst_n(rst_pix_n), .show(osd_on),
            .in_r(s_r), .in_g(s_g), .in_b(s_b), .in_de(s_de), .in_hs(s_hs), .in_vs(s_vs),
            .out_r(vid_r), .out_g(vid_g), .out_b(vid_b),
            .out_de(vid_de), .out_hs(vid_hs), .out_vs(vid_vs),
            .pclk_khz(pclk_khz), .locked(p_locked), .mode(p_mode),
            .line_cycles(p_line_cycles), .frame_lines(p_lines), .hs_cycles(p_hs_cycles),
            .hs_low(p_hs_low), .vs_low(p_vs_low), .h_off(p_h_off), .v_off(p_v_off),
            .aligned_h(p_aligned_h), .aligned_v(p_aligned_v), .bb_valid(p_bb_valid),
            .bb_x0(p_bb_x0), .bb_x1(p_bb_x1), .bb_y0(p_bb_y0), .bb_y1(p_bb_y1),
            .cap_overflow(p_overflow), .m_sel(blank),
            .frames_written(p_frames_written), .frames_rejected(p_frames_rejected),
            .fetch_late(p_fetch_late), .mem_ready(p_mem_ready), .pattern_on(p_pattern_on),
            .chk_err(chk_err),
            .set_src(set_src), .cfg_phase(cfg_phase), .cfg_delay(cfg_delay), .cfg_clock(cfg_clock),
            .cfg_h_adj(cfg_h_adj), .cfg_v_adj(cfg_v_adj), .cfg_align(cfg_align),
            .cfg_aspect(cfg_aspect), .cfg_size(cfg_size), .cfg_rd(cfg_rd), .cursor(cursor), .cal_busy(cal_busy), .cal_point(cal_point), .cal_ok(cal_ok),
            .cal_done(cal_done), .cal_fail(cal_fail)
        );
    end else begin : g_no_osd
        assign {vid_r, vid_g, vid_b, vid_de, vid_hs, vid_vs} = {s_r, s_g, s_b, s_de, s_hs, s_vs};
    end endgenerate

    // TMDS: DVI или HDMI с AVI InfoFrame; синхроимпульсы — в управляющих битах синего канала
    hdmi_tx #(
        .HDMI(HDMI), .VS_HIGH(!VS_NEG),
        .VIC(SCALE ? 8'd19 : 8'd17), .PIC_AR(SCALE ? 2'b10 : 2'b01)
    ) tmds (
        .pixel_clk(pixel_clk), .reset_n(rst_pix_n),
        .r(vid_r), .g(vid_g), .b(vid_b), .de(vid_de), .hs(vid_hs), .vs(vid_vs),
        .tmds_blue(tmds_blue), .tmds_green(tmds_green), .tmds_red(tmds_red)
    );
endmodule
