// SPDX-License-Identifier: Apache-2.0
//
// FT812: то, что видит хост, — регистры, RAM_CMD, RAM_DL, время кадров, INT_N.
// Поведение повторяет eve-emu (alfishe/unreal-ng, core/src/3rdparty/eve-emu, MIT): таблица
// регистров (eve-registers.cpp), посылки SPI (eve-spi.cpp, eve-memory.cpp), развёртка и
// смена дисплей-листа (eve-timing.cpp). Сопроцессора здесь нет: кольцо RAM_CMD читает
// сопроцессор на BL616 (bl616/copro) через порт слов (cp_link), RAM_G — в SDRAM.
//
// Хост (ft_spi, домен SCK) присылает события по переключению spi_tg; конец посылки —
// снятие FT_CS_N, его путь синхронизации на два триггера длиннее, поэтому последний
// байт посылки всегда обработан раньше её конца. Событие приходит не чаще раза в 8 тактов
// SCK (286 нс при 28 МГц, 17 тактов ядра), а ответ на чтение готов через ~6 тактов.
//
// RAM_G (2 МБ, с ramg_3mb — 3 МБ) и шрифты ПЗУ — в SDRAM, через ft_ramg: запросы хоста по SPI
// идут через очередь на 8 запросов (ждать SPI не умеет; до 1.28 — 4: на плате под Zuma и
// заставкой погоды переполнялась), слова порта — с подтверждением; от 3 ждущих — ramg_urgent:
// арбитр SDRAM ставит RAM_G раньше текселей растеризатора.
// Чтение RAM_G промахом мимо кэша отвечает дольше, чем за 8 тактов SCK 28 МГц: первый байт
// такой посылки может опоздать (ramg_late) — на 14 МГц успевает.
//
// Сброс ядра (RST_PULSE, CLKEXT/CLKINT, PWRDOWN) возвращает регистры к значениям сброса за 128
// тактов (2 мкс); RAM_DL и RAM_CMD не стираются — хост читает их только после своей записи
// (eve-emu их обнуляет; на чтение до записи это и отличается).
//
// Регистр (как у FT812) вступает в силу, когда записан его старший байт, когда запись
// ушла в другой регистр или когда посылка кончилась. REG_CMDB_WRITE — порт: каждые
// четыре байта — слово в кольцо RAM_CMD, адрес не растёт; неполное слово в конце
// посылки отбрасывается. Запись, начатая внутри RAM_CMD, заворачивается в её пределах.
//
// Тактовая FT812: CLKEXT — 8 МГц (генератор VDAC2), CLKINT — 12 МГц; множитель CLKSEL,
// 0 — ×5. Такты FT812 считаются от clk дробным счётчиком (до двух за такт clk).
module ft_core #(
    parameter int CLK_KHZ = 61_875,
    // 1 — после сброса ACTIVE, а не SLEEP, и внешний генератор ×8 (64 МГц): ядро FT812 в слоте 1
    // грузится, когда хост уже начал обмен (переход из TS-Config по ACTIVE — CORERST, CLKEXT,
    // CLKSEL, ACTIVE уходят в ядро без FT812), и они пропали; в SLEEP ядро не отвечало бы на чтения
    parameter bit START_ACTIVE = 1'b0
) (
    input  wire        clk,
    input  wire        rst_n,

    // ft_spi
    input  wire        spi_tg,
    input  wire [1:0]  spi_kind,
    input  wire [7:0]  spi_h0,
    input  wire [7:0]  spi_h1,
    input  wire [7:0]  spi_b,
    input  wire        spi_cs_n,          // асинхронный, прямо с вывода
    output reg  [7:0]  spi_rd_byte,

    // Команда хоста от ESP (LINK, регистр FT_HOST): переключатель host_tg из другого домена,
    // host_cmd/host_par устойчивы до него. 00 — ACTIVE, остальные — как с SPI (CLKEXT,
    // CLKSEL, CORERST, …): без SPI ESP-хосту иначе не разбудить FT812
    input  wire        host_tg,
    input  wire [7:0]  host_cmd,
    input  wire [7:0]  host_par,

    // Порт слов — вторая хост-машина: ESP по LINK (её программы, например DOOM, управляют
    // FT812 сами, как Z80) — и сопроцессор BL616 (p_cp). Правила те же, что у хоста на SPI,
    // но слово вступает в силу сразу; сопроцессору ещё можно поднимать флаги REG_INT_FLAGS
    // (запись — ИЛИ с флагами).
    input  wire        p_req,             // держится до p_ack
    input  wire        p_we,
    input  wire        p_cp,
    input  wire [21:2] p_addr,
    input  wire [31:0] p_wdata,
    output reg         p_ack,
    output reg  [31:0] p_rdata,

    output wire        int_n,
    output reg         cmd_kick,          // такт: хост записал в кольцо или в CMD_WRITE
    output wire        copro_hold,        // REG_CPURESET бит 0
    output wire [11:0] cmd_read_ptr,
    output wire [11:0] cmd_write_ptr,
    output wire        dl_active,         // банк RAM_DL, который показывается
    output reg         frame_tick,        // событие кадра FT812
    // для сопроцессора: REG_CMD_DL, REG_DLSWAP, REG_CPURESET; счётчик сбросов сопроцессора
    // (REG_CPURESET бит 0: 1 → 0, конец сброса ядра)
    output wire [12:0] cmd_dl_val,
    output wire [1:0]  dlswap_val,
    output wire [2:0]  cpureset_val,
    output reg  [7:0]  copro_resets,

    // Растеризатор (ft_render): чтение показываемого банка RAM_DL; пока он читает лист,
    // DLSWAP ждёт (swap_hold). Размер кадра и идёт ли развёртка.
    input  wire [10:0] rd_dl_addr,
    output wire [31:0] rd_dl_q,
    input  wire        swap_hold,
    output wire [11:0] disp_hsize,
    output wire [11:0] disp_vsize,
    output wire        disp_on,
    output wire [7:0]  status,            // {активен, внешний клок, 0, свап ждёт [1:0], режим [1:0], 0}
    output reg         ramg_late,         // липкий: ответ на чтение RAM_G для SPI опоздал
    output reg         ramg_drop,         // липкий: очередь RAM_G хоста переполнилась
    output wire        ramg_urgent,       // в очереди хоста 3+: RAM_G — раньше текселей

    input  wire        ramg_3mb,          // RAM_G 3 МБ: шрифты ПЗУ вне адресов FT812
    input  wire        rom_we,            // окно LINK (не сопроцессор) пишет в образ ПЗУ: его заливает ESP

    // SDRAM: клиент sdram_burst (через арбитр ts_fb_core)
    output wire        g_req,
    output wire        g_we,
    output wire [20:0] g_addr,
    input  wire        g_ack,
    input  wire        g_wr_take,
    input  wire [2:0]  g_wr_idx,
    output wire [31:0] g_wdata,
    output wire [3:0]  g_wmask,
    input  wire        g_rd_valid,
    input  wire [2:0]  g_rd_idx,
    input  wire [31:0] g_rd_data
);
    // --- карта адресов ------------------------------------------------------------------------
    localparam logic [21:0] CHIPID   = 22'h0C0000;   // ROM_CHIPID: пишется в RAM_G при сбросе ядра
    localparam logic [20:0] FONT_W   = 21'h0C0000;   // SDRAM: образ ПЗУ 0x1E0000…0x2FFFFF (1152 КБ)
    localparam logic [21:0] DL_BASE  = 22'h300000;   // 8 КБ
    localparam logic [21:0] REG_BASE = 22'h302000;   // 4 КБ
    localparam logic [21:0] CMD_BASE = 22'h308000;   // 4 КБ
    localparam logic [21:0] SPC_BASE = 22'h309000;   // REG_TRACKER, REG_MEDIAFIFO_*

    // Номера регистров: (адрес - 0x302000) / 4
    localparam int R_ID = 0, R_FRAMES = 1, R_CLOCK = 2, R_CPURESET = 8;
    localparam int R_HCYCLE = 11, R_HOFFSET = 12, R_HSIZE = 13;
    localparam int R_VCYCLE = 16, R_VOFFSET = 17, R_VSIZE = 18, R_DLSWAP = 21, R_PCLK = 28;
    localparam int R_INT_FLAGS = 42, R_INT_EN = 43, R_INT_MASK = 44;
    localparam int R_CMD_READ = 62, R_CMD_WRITE = 63, R_CMD_DL = 64;
    localparam int R_UND558 = 342, R_UND560 = 344, R_CMDB_SPACE = 349, R_CMDB_WRITE = 350;
    localparam int R_ADAPTIVE = 351;
    localparam int S_MF_READ = 5, S_MF_WRITE = 6;   // в 0x309000

    // Регистры 0..127, которые хранятся в памяти: есть ли, только чтение, значение сброса
    // (eve-registers.cpp). Регистры с триггерами (ниже) здесь тоже есть — для сброса.
    function automatic logic reg_exists(input logic [6:0] i);
        case (i)
            40, 56, 57, 58, 59, 61, 92, 94, 95: reg_exists = 1'b0;
            default: reg_exists = i <= 101;
        endcase
    endfunction

    function automatic logic reg_ro(input logic [6:0] i);
        case (i)
            0, 1, 2, 9, 31, 41, 42, 47, 60, 91, 99, 100, 101: reg_ro = 1'b1;
            default: reg_ro = i >= 71 && i <= 83;
        endcase
    endfunction

    function automatic logic [31:0] reg_reset(input logic [6:0] i);
        case (i)
            0:  reg_reset = 32'h7C;
            3:  reg_reset = 32'd60_000_000;
            10: reg_reset = 32'hFFFF_FFFF;
            11: reg_reset = 32'd548;
            12: reg_reset = 32'd43;
            13: reg_reset = 32'd480;
            15: reg_reset = 32'd41;
            16: reg_reset = 32'd292;
            17: reg_reset = 32'd12;
            18: reg_reset = 32'd272;
            20: reg_reset = 32'd10;
            24, 26: reg_reset = 32'd1;
            32, 33: reg_reset = 32'hFF;
            41: reg_reset = 32'h1;
            44: reg_reset = 32'hFF;
            48: reg_reset = 32'd8000;
            52: reg_reset = 32'd250;
            53: reg_reset = 32'd128;
            60: reg_reset = 32'h3;
            65: reg_reset = 32'd3;
            66: reg_reset = 32'd1;
            67: reg_reset = 32'h1770;
            68: reg_reset = 32'd3;
            69: reg_reset = 32'd7;
            70: reg_reset = 32'hFFFF;
            71: reg_reset = 32'hFFFF_FFFF;
            72: reg_reset = 32'h7FFF;
            73: reg_reset = 32'h8000_8000;
            84, 88: reg_reset = 32'h10000;
            90: reg_reset = 32'h8000;
            101: reg_reset = 32'h1;
            default: reg_reset = 32'd0;
        endcase
    endfunction

    function automatic logic [31:0] merge(input logic [31:0] old, input logic [31:0] wd,
                                          input logic [3:0] be);
        for (int b = 0; b < 4; b++)
            merge[8*b +: 8] = be[b] ? wd[8*b +: 8] : old[8*b +: 8];
    endfunction

    // --- синхронизация хоста ----------------------------------------------------------------
    reg [2:0] tg_s;
    reg [4:0] cs_s;
    reg [2:0] hc_s;                      // команда хоста от ESP
    wire      hc_ev = hc_s[2] != hc_s[1];
    always_ff @(posedge clk) begin
        hc_s <= {hc_s[1:0], host_tg};
        tg_s <= {tg_s[1:0], spi_tg};
        cs_s <= {cs_s[3:0], spi_cs_n};
    end
    wire ev     = tg_s[2] ^ tg_s[1];
    wire tr_end = cs_s[4:3] == 2'b01;

    // --- состояние ----------------------------------------------------------------------------
    localparam logic [1:0] PM_ACTIVE = 2'd0, PM_STANDBY = 2'd1, PM_SLEEP = 2'd2, PM_DOWN = 2'd3;
    localparam logic [2:0] H_IDLE = 3'd0, H_HOST = 3'd1, H_READ = 3'd2, H_WRITE = 3'd3, H_IGNORE = 3'd4;

    reg  [1:0]  pmode;
    reg         ext_clk;
    reg  [5:0]  mul;
    reg  [2:0]  h_phase;
    reg  [7:0]  h_cmd, h_par;
    reg         h_zero;              // заголовок 00 00 00 (ACTIVE)
    reg  [21:0] h_addr;
    reg         h_wrap;              // запись началась в RAM_CMD

    // регистры на триггерах
    reg  [2:0]  cpureset;
    reg  [11:0] hcycle, hoffset, hsize, vcycle, voffset, vsize;
    reg  [1:0]  dlswap_reg, swap_pend;
    reg         swap_due;            // DLSWAP_FRAME: строка события прошла, ждём окна растеризатора
    reg  [7:0]  pclk;
    reg  [7:0]  int_flags, int_mask;
    reg         int_en;
    reg  [11:0] cmd_read, cmd_write;
    reg  [12:0] cmd_dl;
    reg         adaptive;
    reg  [31:0] mf_read, mf_write;
    reg  [31:0] frames, clock;
    reg         active_dl;
    reg         chipid_pend;

    // отложенное вступление регистра в силу
    reg         pend_v;
    reg  [9:0]  pend_idx;
    reg  [1:0]  pend_old_dlswap;
    reg  [2:0]  pend_old_cpureset;

    reg  [1:0]  cmdb_fill;
    reg         p_busy;              // чтение порта идёт по конвейеру
    reg  [23:0] cmdb_word;

    // значения сброса в память регистров
    reg  [7:0]  clr;                 // [7] — идёт
    wire        clearing = clr[7];
    reg         clr_q, cpur_q;

    // --- память ---------------------------------------------------------------------------------
    reg         dl_we;
    reg  [3:0]  dl_be;
    reg  [10:0] dl_waddr, dl_raddr;
    reg  [31:0] dl_wdata;
    wire [31:0] dl0_q, dl1_q;
    // Хост пишет и читает тот банк, что не показывается; растеризатор читает показываемый.
    ft_ram #(.AW(11)) u_dl0 (.clk(clk), .we(dl_we && active_dl), .be(dl_be),
                             .waddr(dl_waddr), .wdata(dl_wdata),
                             .raddr(active_dl ? dl_raddr : rd_dl_addr), .rdata(dl0_q));
    ft_ram #(.AW(11)) u_dl1 (.clk(clk), .we(dl_we && !active_dl), .be(dl_be),
                             .waddr(dl_waddr), .wdata(dl_wdata),
                             .raddr(active_dl ? rd_dl_addr : dl_raddr), .rdata(dl1_q));
    assign rd_dl_q = active_dl ? dl1_q : dl0_q;

    reg         cm_we;
    reg  [3:0]  cm_be;
    reg  [9:0]  cm_waddr, cm_raddr;
    reg  [31:0] cm_wdata;
    wire [31:0] cm_q;
    ft_ram #(.AW(10)) u_cmd (.clk(clk), .we(cm_we), .be(cm_be), .waddr(cm_waddr),
                             .wdata(cm_wdata), .raddr(cm_raddr), .rdata(cm_q));

    reg         rg_we;
    reg  [3:0]  rg_be;
    reg  [6:0]  rg_waddr, rg_raddr;
    reg  [31:0] rg_wdata;
    wire [31:0] rg_q;
    ft_ram #(.AW(7)) u_reg (.clk(clk), .we(rg_we), .be(rg_be), .waddr(rg_waddr),
                            .wdata(rg_wdata), .raddr(rg_raddr), .rdata(rg_q));

    // --- время -------------------------------------------------------------------------------------
    wire [19:0] f_khz      = (ext_clk ? 20'd8000 : 20'd12000) * (mul != 0 ? mul : 6'd5);
    wire [19:0] f_lim      = f_khz > 2 * CLK_KHZ ? 20'(2 * CLK_KHZ) : f_khz;
    wire        running    = pmode == PM_ACTIVE;
    wire [19:0] line_clks  = hcycle * pclk;
    wire        scan_on    = running && line_clks != 0 && vcycle != 0;
    wire [12:0] vis_end    = voffset + vsize;
    wire [11:0] ev_line_a  = vis_end == 0 ? 12'd1 : (vis_end > vcycle ? vcycle : vis_end[11:0]);
    wire [11:0] event_line = ev_line_a - 12'd1;

    reg  [20:0] f_acc;
    reg  [19:0] line_pos;
    reg  [11:0] line;

    // Настройки времени меняются редко — через регистр: короче путь от регистров FT812
    // до развёртки (иначе множитель, сумматор и сравнения не помещаются в такт 61,875 МГц).
    reg  [19:0] f_lim_r, line_clks_r;
    reg  [11:0] event_line_r;
    reg         scan_on_r;
    reg  [1:0]  ticks;                   // такты FT812 за прошлый такт clk
    always_ff @(posedge clk) begin
        f_lim_r      <= f_lim;
        line_clks_r  <= line_clks;
        event_line_r <= event_line;
        scan_on_r    <= scan_on;
    end

    wire [20:0] f_sum  = f_acc + f_lim_r;
    wire [1:0]  f_tick = f_sum >= 21'(2 * CLK_KHZ) ? 2'd2 : (f_sum >= 21'(CLK_KHZ) ? 2'd1 : 2'd0);
    wire [20:0] pos_n  = line_pos + ticks;
    wire        eol    = scan_on_r && pos_n >= line_clks_r;

    // --- доступ к памяти: стадия 0 — запрос ----------------------------------------------------
    // Хост: чтение (rd_host), запись байта; порт: слово. За такт — одно обращение.
    reg         a_v, a_we, a_host;
    reg  [21:0] a_addr;              // байтовый адрес
    reg  [3:0]  a_be;
    reg  [31:0] a_wd;

    // стадия 1 — данные памяти готовы
    reg         b_v, b_host, b_clr_int;
    reg  [21:0] b_addr;
    reg  [31:0] b_ff;                // значение регистра с триггерами или константы
    reg  [2:0]  b_src;               // 0 — b_ff, 1 — RAM_DL, 2 — RAM_CMD, 3 — память регистров
    localparam logic [2:0] SRC_FF = 3'd0, SRC_DL = 3'd1, SRC_CMD = 3'd2, SRC_REG = 3'd3;

    wire [31:0] cmdb_space = 32'((12'hFFC - (cmd_write - cmd_read)) & 12'hFFC);

    // Значение, которое не лежит в памяти: регистр с триггерами, вычисляемый, константа.
    // [32] = 1 — слово берётся из памяти (RAM_DL, RAM_CMD, память регистров 0..127).
    function automatic logic [32:0] ff_read(input logic [21:0] a);
        logic [9:0] i;
        i = a[11:2];
        ff_read = {1'b1, 32'd0};
        if (a >= REG_BASE && a < REG_BASE + 22'h1000) begin
            case (i)
                R_ID:         ff_read = {1'b0, 32'h7C};
                R_FRAMES:     ff_read = {1'b0, frames};
                R_CLOCK:      ff_read = {1'b0, clock};
                R_CPURESET:   ff_read = {1'b0, 32'(cpureset)};
                R_HCYCLE:     ff_read = {1'b0, 32'(hcycle)};
                R_HOFFSET:    ff_read = {1'b0, 32'(hoffset)};
                R_HSIZE:      ff_read = {1'b0, 32'(hsize)};
                R_VCYCLE:     ff_read = {1'b0, 32'(vcycle)};
                R_VOFFSET:    ff_read = {1'b0, 32'(voffset)};
                R_VSIZE:      ff_read = {1'b0, 32'(vsize)};
                R_DLSWAP:     ff_read = {1'b0, 32'(dlswap_reg)};
                R_PCLK:       ff_read = {1'b0, 32'(pclk)};
                R_INT_FLAGS:  ff_read = {1'b0, 32'(int_flags)};
                R_INT_EN:     ff_read = {1'b0, 32'(int_en)};
                R_INT_MASK:   ff_read = {1'b0, 32'(int_mask)};
                R_CMD_READ:   ff_read = {1'b0, 32'(cmd_read)};
                R_CMD_WRITE:  ff_read = {1'b0, 32'(cmd_write)};
                R_CMD_DL:     ff_read = {1'b0, 32'(cmd_dl)};
                R_UND558:     ff_read = {1'b0, 32'h100};
                R_UND560:     ff_read = {1'b0, 32'h800};
                R_CMDB_SPACE: ff_read = {1'b0, cmdb_space};
                R_ADAPTIVE:   ff_read = {1'b0, 32'(adaptive)};
                // остальные 0..127 — в памяти, несуществующие и дальше 127 — нули
                default:      ff_read = {i < 10'd128 && reg_exists(i[6:0]), 32'd0};
            endcase
        end else if (a >= SPC_BASE && a < SPC_BASE + 22'h20) begin
            ff_read = {1'b0, a[4:2] == 3'(S_MF_READ) ? mf_read :
                             a[4:2] == 3'(S_MF_WRITE) ? mf_write : 32'd0};
        end else if (a < 22'h300000) begin
            ff_read = {1'b0, 32'd0};                     // RAM_G и ПЗУ отвечает ft_ramg
        end
    endfunction

    wire [32:0] a_ff = ff_read(a_addr);

    // --- RAM_G: ft_ramg -------------------------------------------------------------------------
    // Адрес FT812 → SDRAM: есть ли, только чтение (ПЗУ), адрес слова. RAM_G — с нуля SDRAM;
    // ПЗУ (0x1E0000…0x2FFFFF FT812) — образом с FONT_W, видна в режиме 2 МБ с 0x200000.
    function automatic logic gm_v(input logic [21:0] a);
        gm_v = a < 22'h300000;
    endfunction
    function automatic logic gm_ro(input logic [21:0] a);
        gm_ro = !ramg_3mb && a >= 22'h200000;
    endfunction
    function automatic logic [20:0] gm_addr(input logic [21:0] a);
        logic [21:0] r;
        r = a - 22'h1E0000;
        gm_addr = gm_ro(a) ? FONT_W + 21'(r[21:2]) : {1'b0, a[21:2]};
    endfunction

    // очередь запросов хоста (SPI): {запись, адрес слова, байт, данные} — 8 мест в распределённой
    // памяти (RAM16: запись по такту, чтение сразу; в триггерах с выбором 8:1 она стоила ~220
    // ячеек при CLS 99 % — тайминг clk_mem не сходился). Запрос сначала ложится в hs_*, тактом
    // позже — в память: место занимает hq_wa (по нему «полно» и «срочно»), читатель видит запрос
    // по hq_wr — он двигается вместе с записью в память
    reg  [31:0] hq_mem [0:7];
    reg  [3:0]  hq_wa, hq_wr, hq_rd;  // указатели с лишним разрядом: полно — разница 8
    reg         hs_v;
    reg  [2:0]  hs_idx;
    reg  [31:0] hs_w;
    wire        hq_empty = hq_wr == hq_rd;
    wire        hq_full  = hq_wa - hq_rd == 4'd8;
    assign      ramg_urgent = hq_wa - hq_rd >= 4'd3;
    wire [31:0] hq_q      = hq_mem[hq_rd[2:0]];
    wire        hq_q_we   = hq_q[31];
    wire [20:0] hq_q_addr = hq_q[30:10];
    wire [1:0]  hq_q_lane = hq_q[9:8];
    wire [7:0]  hq_q_data = hq_q[7:0];
    always_ff @(posedge clk)
        if (hs_v) hq_mem[hs_idx] <= hs_w;
    always_ff @(posedge clk or negedge rst_n)
        if (!rst_n) hq_wr <= 4'd0;
        else if (hs_v) hq_wr <= hq_wr + 4'd1;

    reg         pg_v;                 // порт ждёт ft_ramg
    reg  [20:0] pg_addr;              // его адрес SDRAM — пересчитан при приёме (короче путь)
    reg         g_rd_pend, g_rd_host; // ответ на чтение ждёт: хост или порт
    reg  [1:0]  g_rd_lane;
    reg         g_flush;

    // Следующий запрос к ft_ramg выбирается заранее в регистр rq_*: от очереди и порта до
    // сравнений в ft_ramg — граница такта (запросы приходят реже, чем раз в 17 тактов).
    localparam logic [1:0] G_NONE = 2'd0, G_CHIP = 2'd1, G_HOST = 2'd2, G_PORT = 2'd3;
    wire [1:0]  nx_src = chipid_pend ? G_CHIP : !hq_empty ? G_HOST : pg_v ? G_PORT : G_NONE;
    reg         rq_v, rq_we;
    reg  [1:0]  rq_src, rq_lane;
    reg  [20:0] rq_addr;
    reg  [3:0]  rq_be;
    reg  [31:0] rq_wd;
    // пока ждём ответа на чтение, следующее чтение не подаём (ответы без меток)
    wire        gq_valid = rq_v && !(g_rd_pend && !rq_we);
    wire        gq_ready, gr_valid;
    wire [31:0] gr_data;

    ft_ramg u_ramg (
        .clk(clk), .rst_n(rst_n),
        .q_valid(gq_valid), .q_ready(gq_ready), .q_we(rq_we), .q_waddr(rq_addr), .q_be(rq_be),
        .q_wdata(rq_wd), .q_flush(g_flush), .r_valid(gr_valid), .r_data(gr_data), .idle(),
        .g_req(g_req), .g_we(g_we), .g_addr(g_addr), .g_ack(g_ack), .g_wr_take(g_wr_take),
        .g_wr_idx(g_wr_idx), .g_wdata(g_wdata), .g_wmask(g_wmask), .g_rd_valid(g_rd_valid),
        .g_rd_idx(g_rd_idx), .g_rd_data(g_rd_data)
    );

    // Запрос хоста в очередь: запись байта или чтение (ответ — в spi_rd_byte).
    task automatic hq_push(input logic we, input logic [20:0] wa, input logic [1:0] lane,
                           input logic [7:0] d);
        if (hq_full) begin
            ramg_drop <= 1'b1;
        end else begin
            hs_v   <= 1'b1;
            hs_idx <= hq_wa[2:0];
            hs_w   <= {we, wa, lane, d};
            hq_wa  <= hq_wa + 4'd1;
        end
    endtask

    // Следующий байт чтения хоста — заранее, регистрами: адрес, попал ли он в RAM_G/ПЗУ и адрес
    // SDRAM. h_addr меняется только по событиям SPI, а они не чаще раза в 17 тактов — к
    // следующему событию значения готовы (иначе сумматор, сравнение и вычитание стоят на
    // пути к разрешению записи очереди хоста — один из худших путей clk_mem).
    reg  [21:0] hn_addr;
    reg         hn_g;
    reg  [20:0] hn_gaddr;
    always_ff @(posedge clk) begin
        hn_addr  <= h_addr + 22'd1;
        hn_g     <= h_addr + 22'd1 < 22'h300000;
        hn_gaddr <= gm_addr(h_addr + 22'd1);
    end

    task automatic host_read_next();
        if (hn_g) begin
            if (g_rd_pend && g_rd_host)
                ramg_late <= 1'b1;
            hq_push(1'b0, hn_gaddr, hn_addr[1:0], 8'd0);
        end else begin
            access(1'b0, 1'b1, hn_addr, 4'h0, 32'd0);
        end
    endtask

    // Чтение байта для SPI: RAM_G и ПЗУ — через очередь, остальное — из памяти и регистров.
    task automatic host_read(input logic [21:0] a);
        if (a < 22'h300000) begin
            if (g_rd_pend && g_rd_host)
                ramg_late <= 1'b1;                   // прошлый ответ ещё не пришёл
            hq_push(1'b0, gm_addr(a), a[1:0], 8'd0);
        end else begin
            access(1'b0, 1'b1, a, 4'h0, 32'd0);
        end
    endtask

    // --- основной автомат ------------------------------------------------------------------------
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            pmode    <= START_ACTIVE ? PM_ACTIVE : PM_SLEEP;   // SLEEP — как FT812 при включении
            // слот 1: и такты, как ставят программы VDAC2 (TSLib, ESP) — CLKEXT, CLKSEL ×8: 64 МГц
            ext_clk  <= START_ACTIVE;
            mul      <= START_ACTIVE ? 6'd8 : 6'd0;
            h_phase  <= H_IDLE;
            clr      <= 8'h80;
            f_acc    <= '0;
            ticks    <= 2'd0;
            spi_rd_byte <= 8'h00;
            p_ack    <= 1'b0;
            p_busy   <= 1'b0;
            cmd_kick <= 1'b0;
            frame_tick <= 1'b0;
            a_v      <= 1'b0;
            b_v      <= 1'b0;
            hq_wa    <= 4'd0;
            hq_rd    <= 4'd0;
            hs_v     <= 1'b0;
            pg_v     <= 1'b0;
            rq_v     <= 1'b0;
            g_rd_pend <= 1'b0;
            g_flush  <= 1'b0;
            ramg_late <= 1'b0;
            ramg_drop <= 1'b0;
            copro_resets <= 8'd0;
            clr_q    <= 1'b1;
            cpur_q   <= 1'b0;
            core_reset();
        end else begin
            clr_q  <= clearing;
            cpur_q <= cpureset[0];
            if ((clr_q && !clearing) || (cpur_q && !cpureset[0]))
                copro_resets <= copro_resets + 8'd1;
            dl_we <= 1'b0;
            cm_we <= 1'b0;
            rg_we <= 1'b0;
            p_ack <= 1'b0;
            cmd_kick <= 1'b0;
            frame_tick <= 1'b0;
            a_v <= 1'b0;
            g_flush <= 1'b0;
            hs_v <= 1'b0;

            // RAM_G: следующий запрос — в регистр rq_* (источник снимается сразу)
            if (!rq_v && nx_src != G_NONE) begin
                rq_v   <= 1'b1;
                rq_src <= nx_src;
                case (nx_src)
                    G_CHIP: begin
                        chipid_pend <= 1'b0;
                        rq_we <= 1'b1; rq_addr <= 21'(CHIPID >> 2); rq_be <= 4'hF;
                        rq_wd <= 32'h0001_1208; rq_lane <= 2'd0;
                    end
                    G_HOST: begin
                        hq_rd   <= hq_rd + 4'd1;
                        rq_we   <= hq_q_we;
                        rq_addr <= hq_q_addr;
                        rq_be   <= 4'b0001 << hq_q_lane;
                        rq_wd   <= {4{hq_q_data}};
                        rq_lane <= hq_q_lane;
                    end
                    default: begin                    // G_PORT
                        pg_v    <= 1'b0;
                        rq_we   <= p_we;
                        rq_addr <= pg_addr;
                        rq_be   <= 4'hF;
                        rq_wd   <= p_wdata;
                        rq_lane <= 2'd0;
                    end
                endcase
            end
            // запрос принят ft_ramg; ответ на чтение — владельцу
            if (gq_ready) begin
                rq_v <= 1'b0;
                if (!rq_we) begin
                    g_rd_pend <= 1'b1;
                    g_rd_host <= rq_src == G_HOST;
                    g_rd_lane <= rq_lane;
                end else if (rq_src == G_PORT) begin
                    p_ack  <= 1'b1;
                    p_busy <= 1'b0;
                end
            end
            if (gr_valid) begin
                g_rd_pend <= 1'b0;
                if (g_rd_host) begin
                    spi_rd_byte <= pick(gr_data, g_rd_lane);
                end else begin
                    p_rdata <= gr_data;
                    p_ack   <= 1'b1;
                    p_busy  <= 1'b0;
                end
            end

            // такты FT812 и развёртка
            f_acc <= f_sum - (f_tick == 2'd2 ? 21'(2 * CLK_KHZ) : (f_tick == 2'd1 ? 21'(CLK_KHZ) : 21'd0));
            ticks <= f_tick;
            if (running)
                clock <= clock + 32'(ticks);
            if (scan_on_r) begin
                if (eol) begin
                    line_pos <= pos_n[19:0] - line_clks_r;
                    if (line == event_line_r) begin
                        if (swap_pend == 2'd2)
                            swap_due <= 1'b1;        // смена «по кадру» созрела
                        frames     <= frames + 32'd1;
                        frame_tick <= 1'b1;
                    end
                    line <= line + 12'd1 >= vcycle ? 12'd0 : line + 12'd1;
                end else begin
                    line_pos <= pos_n[19:0];
                end
            end
            // Смена листа — как только растеризатор его не читает (swap_hold снят): по строке и
            // без развёртки — сразу, по кадру — после строки события (не чаще кадра FT812, как у
            // FT812). Ждать саму строку события нельзя: растеризатор читает лист почти весь кадр,
            // окно между кадрами привязано к выводу 60 Гц, строка события — к развёртке 59 Гц;
            // фаза плывёт, и смена стояла до полсекунды (DEMO3D ~9 fps рывками, R-Type).
            if (!swap_hold && (swap_pend == 2'd1 || (swap_pend == 2'd2 && (swap_due || !scan_on_r))))
                apply_swap();
            if (line_clks_r != 0 && line_pos >= line_clks_r)
                line_pos <= line_clks_r - 20'd1;
            if (vcycle != 0 && line >= vcycle)
                line <= vcycle - 12'd1;

            // значения сброса в память регистров
            if (clearing) begin
                rg_we    <= 1'b1;
                rg_be    <= 4'hF;
                rg_waddr <= clr[6:0];
                rg_wdata <= reg_reset(clr[6:0]);
                clr      <= clr[6:0] == 7'h7F ? 8'd0 : clr + 8'd1;
            end

            // события хоста
            if (ev) begin
                case (spi_kind)
                    2'd0: begin                              // заголовок
                        h_cmd  <= spi_h0;
                        h_par  <= spi_h1;
                        h_zero <= spi_h0 == 8'd0 && spi_h1 == 8'd0 && spi_b == 8'd0;
                        h_addr <= {spi_h0[5:0], spi_h1, spi_b};
                        h_wrap <= {spi_h0[5:0], spi_h1, spi_b} >= CMD_BASE &&
                                  {spi_h0[5:0], spi_h1, spi_b} <  CMD_BASE + 22'h1000;
                        spi_rd_byte <= 8'h00;
                        case (spi_h0[7:6])
                            2'b01: h_phase <= H_HOST;
                            2'b11: h_phase <= H_IGNORE;
                            2'b00: begin
                                h_phase <= running && !clearing ? H_READ : H_IGNORE;
                                if (running && !clearing)
                                    host_read({spi_h0[5:0], spi_h1, spi_b});
                            end
                            default: h_phase <= running ? H_WRITE : H_IGNORE;
                        endcase
                    end
                    2'd1: if (h_phase == H_WRITE && !clearing) begin   // байт записи
                        host_write(h_addr, spi_b);
                        h_addr <= next_waddr(h_addr);
                    end
                    2'd2: if (h_phase == H_READ) begin                 // следующий байт чтения
                        h_addr <= hn_addr;
                        host_read_next();
                    end
                    default: ;
                endcase
            end else if (p_req && !p_busy && !p_ack && !clearing) begin
                // порт слов — в такт без событий хоста
                if (p_cp && p_we && {p_addr, 2'b00} == REG_BASE + 22'(4 * R_INT_FLAGS)) begin
                    int_flags <= int_flags | p_wdata[7:0];
                    p_ack     <= 1'b1;
                end else if ({p_addr, 2'b00} < 22'h300000) begin
                    if (gm_v({p_addr, 2'b00}) && !(p_we && gm_ro({p_addr, 2'b00}) && !(rom_we && !p_cp))) begin
                        p_busy  <= 1'b1;              // RAM_G: ответит ft_ramg
                        pg_v    <= 1'b1;
                        pg_addr <= gm_addr({p_addr, 2'b00});
                    end else begin
                        p_rdata <= 32'd0;
                        p_ack   <= 1'b1;
                    end
                end else if (p_we) begin
                    port_write({p_addr, 2'b00}, p_wdata);
                    p_ack <= 1'b1;
                end else begin
                    p_busy <= 1'b1;
                    access(1'b0, 1'b0, {p_addr, 2'b00}, 4'h0, 32'd0);
                end
            end

            // конец посылки
            if (tr_end) begin
                case (h_phase)
                    H_HOST:   host_command(h_cmd, h_par);
                    H_IGNORE: if (!running && h_zero) pmode <= PM_ACTIVE;
                    H_WRITE:  begin commit(); cmdb_fill <= 2'd0; g_flush <= 1'b1; end
                    default: ;
                endcase
                h_phase <= H_IDLE;
            end

            // команда хоста от ESP (LINK, FT_HOST): 00 — ACTIVE, остальные — как с SPI
            if (hc_ev) begin
                if (host_cmd == 8'h00) begin
                    if (!running) pmode <= PM_ACTIVE;
                end else begin
                    host_command(host_cmd, host_par);
                end
            end

            // стадия 1: адрес чтения ушёл в память
            b_v <= a_v;
            if (a_v) begin
                b_host <= a_host;
                b_addr <= a_addr;
                b_clr_int <= a_host && a_addr == REG_BASE + 22'(4 * R_INT_FLAGS);
                b_ff  <= a_ff[31:0];
                b_src <= !a_ff[32] ? SRC_FF :
                         (a_addr >= DL_BASE  && a_addr < DL_BASE + 22'h2000)  ? SRC_DL :
                         (a_addr >= CMD_BASE && a_addr < CMD_BASE + 22'h1000) ? SRC_CMD :
                         (a_addr >= REG_BASE && a_addr < REG_BASE + 22'h200)  ? SRC_REG : SRC_FF;
            end
            // стадия 2: ответ
            if (b_v) begin
                if (b_host) begin
                    spi_rd_byte <= pick(read_word(), b_addr[1:0]);
                    if (b_clr_int)
                        int_flags <= 8'd0;   // REG_INT_FLAGS сбрасывается чтением
                end else begin
                    p_rdata <= read_word();
                    p_ack   <= 1'b1;
                    p_busy  <= 1'b0;
                end
            end
        end
    end

    function automatic logic [31:0] read_word();
        case (b_src)
            SRC_DL:  read_word = active_dl ? dl0_q : dl1_q;
            SRC_CMD: read_word = cm_q;
            SRC_REG: read_word = rg_q;
            default: read_word = b_ff;
        endcase
    endfunction

    function automatic logic [7:0] pick(input logic [31:0] w, input logic [1:0] lane);
        pick = w[8*lane +: 8];
    endfunction

    function automatic logic [21:0] next_waddr(input logic [21:0] a);
        if (a[21:2] == 20'((REG_BASE + 22'(4 * R_CMDB_WRITE)) >> 2))
            next_waddr = a;
        else if (h_wrap && a >= CMD_BASE && a < CMD_BASE + 22'h1000)
            next_waddr = CMD_BASE + 22'((a + 22'd1) & 22'hFFF);
        else
            next_waddr = a + 22'd1;
    endfunction

    // Чтение: адрес — в память; ответ — через два такта.
    task automatic access(input logic we, input logic host, input logic [21:0] a,
                          input logic [3:0] be, input logic [31:0] wd);
        a_v      <= 1'b1;
        a_we     <= we;
        a_host   <= host;
        a_addr   <= a;
        a_be     <= be;
        a_wd     <= wd;
        dl_raddr <= a[12:2];
        cm_raddr <= a[11:2];
        rg_raddr <= a[8:2];
    endtask

    // Байт хоста.
    task automatic host_write(input logic [21:0] a, input logic [7:0] d);
        logic [9:0] i;
        logic [3:0] be;
        i  = a[11:2];
        be = 4'b0001 << a[1:0];
        if (a < 22'h300000) begin
            if (gm_v(a) && !gm_ro(a))
                hq_push(1'b1, gm_addr(a), a[1:0], d);
        end else if (a < DL_BASE + 22'h2000) begin
            dl_we    <= 1'b1;
            dl_be    <= be;
            dl_waddr <= a[12:2];
            dl_wdata <= {4{d}};
        end else if (a >= CMD_BASE && a < CMD_BASE + 22'h1000) begin
            cm_we    <= 1'b1;
            cm_be    <= be;
            cm_waddr <= a[11:2];
            cm_wdata <= {4{d}};
        end else if (a >= REG_BASE && a < REG_BASE + 22'h1000) begin
            if (i == 10'(R_CMDB_WRITE)) begin
                if (cmdb_fill == 2'd3) begin
                    cm_we     <= 1'b1;
                    cm_be     <= 4'hF;
                    cm_waddr  <= cmd_write[11:2];
                    cm_wdata  <= {d, cmdb_word};
                    cmd_write <= cmd_write + 12'd4;
                    cmd_kick  <= 1'b1;
                end
                if (cmdb_fill != 2'd3)
                    cmdb_word[8*cmdb_fill[1:0] +: 8] <= d;
                cmdb_fill <= cmdb_fill + 2'd1;
            end else begin
                if (pend_v && pend_idx != i)
                    commit();
                reg_write(i, be, {4{d}}, !pend_v || pend_idx != i);
                if (a[1:0] == 2'd3)
                    commit();
            end
        end else if (a >= SPC_BASE && a < SPC_BASE + 22'h20) begin
            if (a[4:2] == 3'(S_MF_READ))
                mf_read <= merge(mf_read, {4{d}}, be);
            else if (a[4:2] == 3'(S_MF_WRITE)) begin
                mf_write <= merge(mf_write, {4{d}}, be);
                if (a[1:0] == 2'd3)
                    cmd_kick <= 1'b1;
            end
        end
    endtask

    // Слово порта: вступает в силу сразу.
    task automatic port_write(input logic [21:0] a, input logic [31:0] d);
        logic [9:0] i;
        i = a[11:2];
        if (a < 22'h300000) begin
            ;                                         // RAM_G — через ft_ramg (основной автомат)
        end else if (a < DL_BASE + 22'h2000) begin
            dl_we    <= 1'b1;
            dl_be    <= 4'hF;
            dl_waddr <= a[12:2];
            dl_wdata <= d;
        end else if (a >= CMD_BASE && a < CMD_BASE + 22'h1000) begin
            cm_we    <= 1'b1;
            cm_be    <= 4'hF;
            cm_waddr <= a[11:2];
            cm_wdata <= d;
        end else if (a >= REG_BASE && a < REG_BASE + 22'h1000) begin
            if (i == 10'(R_CMDB_WRITE)) begin
                cm_we     <= 1'b1;
                cm_be     <= 4'hF;
                cm_waddr  <= cmd_write[11:2];
                cm_wdata  <= d;
                cmd_write <= cmd_write + 12'd4;
                cmd_kick  <= 1'b1;
            end else begin
                if (pend_v)
                    commit();
                reg_write(i, 4'hF, d, 1'b1);
                pend_v <= 1'b0;
                effect(i, dlswap_reg, cpureset, d);
            end
        end else if (a >= SPC_BASE && a < SPC_BASE + 22'h20) begin
            if (a[4:2] == 3'(S_MF_READ))
                mf_read <= d;
            else if (a[4:2] == 3'(S_MF_WRITE)) begin
                mf_write <= d;
                cmd_kick <= 1'b1;
            end
        end
    endtask

    // Запись в регистр с маской байтов: триггеры и память регистров.
    task automatic reg_write(input logic [9:0] i, input logic [3:0] be, input logic [31:0] d,
                             input logic first);
        logic [31:0] m;
        if (first) begin
            pend_v            <= 1'b1;
            pend_idx          <= i;
            pend_old_dlswap   <= dlswap_reg;
            pend_old_cpureset <= cpureset;
        end
        if (i < 10'd128 && reg_exists(i[6:0]) && !reg_ro(i[6:0])) begin
            rg_we    <= 1'b1;
            rg_be    <= be;
            rg_waddr <= i[6:0];
            rg_wdata <= d;
        end
        case (i)
            R_CPURESET:  begin m = merge(32'(cpureset), d, be); cpureset <= m[2:0]; end
            R_HCYCLE:    begin m = merge(32'(hcycle), d, be); hcycle <= m[11:0]; end
            R_HOFFSET:   begin m = merge(32'(hoffset), d, be); hoffset <= m[11:0]; end
            R_HSIZE:     begin m = merge(32'(hsize), d, be); hsize <= m[11:0]; end
            R_VCYCLE:    begin m = merge(32'(vcycle), d, be); vcycle <= m[11:0]; end
            R_VOFFSET:   begin m = merge(32'(voffset), d, be); voffset <= m[11:0]; end
            R_VSIZE:     begin m = merge(32'(vsize), d, be); vsize <= m[11:0]; end
            R_DLSWAP:    begin m = merge(32'(dlswap_reg), d, be); dlswap_reg <= m[1:0]; end
            R_PCLK:      begin m = merge(32'(pclk), d, be); pclk <= m[7:0]; end
            R_INT_EN:    begin m = merge(32'(int_en), d, be); int_en <= m[0]; end
            R_INT_MASK:  begin m = merge(32'(int_mask), d, be); int_mask <= m[7:0]; end
            R_CMD_READ:  begin m = merge(32'(cmd_read), d, be); cmd_read <= m[11:0]; end
            R_CMD_WRITE: begin m = merge(32'(cmd_write), d, be); cmd_write <= m[11:0]; end
            R_CMD_DL:    begin m = merge(32'(cmd_dl), d, be); cmd_dl <= m[12:0]; end
            R_ADAPTIVE:  begin m = merge(32'(adaptive), d, be); adaptive <= m[0]; end
            default: ;
        endcase
    endtask

    // Регистр вступил в силу.
    task automatic commit();
        if (pend_v) begin
            pend_v <= 1'b0;
            effect(pend_idx, pend_old_dlswap, pend_old_cpureset, 32'(dlswap_reg));
        end
    endtask

    // d — новое значение DLSWAP (для порта — записанное слово).
    task automatic effect(input logic [9:0] i, input logic [1:0] old_dlswap,
                          input logic [2:0] old_cpureset, input logic [31:0] d);
        case (i)
            R_DLSWAP: case (d[1:0])
                2'd0: dlswap_reg <= old_dlswap;   // 0 не трогает ждущую смену
                2'd3: ;                            // 3 остаётся в регистре без смены
                default: swap_pend <= d[1:0];
            endcase
            R_CMD_WRITE: cmd_kick <= 1'b1;
            default: ;
        endcase
    endtask

    task automatic apply_swap();
        active_dl  <= ~active_dl;
        swap_pend  <= 2'd0;
        swap_due   <= 1'b0;
        dlswap_reg <= 2'd0;
        int_flags  <= int_flags | 8'h01;          // INT_SWAP
    endtask

    // Команда хоста (в конце посылки).
    task automatic host_command(input logic [7:0] c, input logic [7:0] p);
        case (c)
            8'h41: if (pmode == PM_ACTIVE) pmode <= PM_STANDBY;
            8'h42: if (pmode != PM_DOWN) pmode <= PM_SLEEP;
            8'h43, 8'h50: begin pmode <= PM_DOWN; core_reset(); end
            8'h44: if (!ext_clk) begin ext_clk <= 1'b1; core_reset(); end
            8'h48: if (ext_clk) begin ext_clk <= 1'b0; core_reset(); end
            8'h61, 8'h62: if (pmode == PM_SLEEP && (p[5:0] == 6'd0 || p[5:0] >= 6'd2))
                mul <= p[5:0];
            8'h68: core_reset();
            default: ;
        endcase
    endtask

    // Сброс ядра FT812: регистры к значениям сброса, RAM_DL, RAM_CMD и регистры — нули.
    task automatic core_reset();
        cpureset   <= 3'd0;
        hcycle     <= 12'd548;
        hoffset    <= 12'd43;
        hsize      <= 12'd480;
        vcycle     <= 12'd292;
        voffset    <= 12'd12;
        vsize      <= 12'd272;
        dlswap_reg <= 2'd0;
        swap_pend  <= 2'd0;
        swap_due   <= 1'b0;
        pclk       <= 8'd0;
        int_flags  <= 8'd0;
        int_mask   <= 8'hFF;
        int_en     <= 1'b0;
        cmd_read   <= 12'd0;
        cmd_write  <= 12'd0;
        cmd_dl     <= 13'd0;
        adaptive   <= 1'b0;
        mf_read    <= 32'd0;
        mf_write   <= 32'd0;
        clock      <= 32'd0;
        line_pos   <= 20'd0;
        line       <= 12'd0;
        active_dl  <= 1'b0;
        chipid_pend <= 1'b1;
        pend_v     <= 1'b0;
        cmdb_fill  <= 2'd0;
        clr        <= 8'h80;
    endtask

    assign int_n         = !(int_en && |(int_flags & int_mask));
    assign copro_hold    = cpureset[0];
    assign cmd_read_ptr  = cmd_read;
    assign cmd_write_ptr = cmd_write;
    assign dl_active     = active_dl;
    assign cmd_dl_val    = cmd_dl;
    assign dlswap_val    = dlswap_reg;
    assign cpureset_val  = cpureset;
    assign disp_hsize    = hsize;
    assign disp_vsize    = vsize;
    assign disp_on       = scan_on_r;
    assign status        = {running, ext_clk, 1'b0, swap_pend, pmode, 1'b0};
endmodule
