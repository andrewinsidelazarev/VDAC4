// SPDX-License-Identifier: Apache-2.0
//
// Растеризатор FT812, точки плитки (ft_render): фрагменты исполнителя (ft_rd_exec) →
// буфер плитки 128×8 (TW_LOG, TH_LOG; 1024 точки) → SDRAM.
//
// Буферов плитки два: в один рисуется плитка, другой в это время уходит в кадр SDRAM.
// Точка буфера — {строка плитки, x[5:0]} (1024 точки, как прежде строка). Буфер — цвет
// RGBA по 8 бит (чётные и нечётные точки — в разных блоках BSRAM, чтобы выгружать по две
// точки за такт) и трафарет. У каждого слова цвета есть флаг «не касались»: такая точка
// берёт значение плитки по умолчанию (impl_*) — его задаёт первый CLEAR плитки без записи
// в буфер (ft_rd_exec), а выгрузка снова ставит флаг всем точкам. Плитка, брошенная посреди
// (кадр брошен: повтор листа при смене, выключение), не выгружается — её точки остались бы в
// буфере для первой плитки следующего кадра (левый верхний угол); clear размечает оба буфера
// заново, как после сброса (ready снят, 512 тактов), — растеризатор даёт его после броска.
//
// Фрагмент (как shade() модели scripts/ft_render_model.py):
//   A  — данные буфера пришли (точка, которой не касались, — значение строки по умолчанию);
//   B1 — проверка альфы, трафарета, новое значение трафарета, множители смешения;
//   B2 — произведения канал × множитель;
//   B3 — ⌊·/255⌋, сумма с насыщением, маска каналов;
//   B4 — запись.
// Точка, которая ещё в A…B4, не принимается снова, пока не записана (f_ready = 0).
// f_ready — из регистров: сравнение считается на такт раньше для обоих случаев — исполнитель
// продвинулся (следующая точка — f_xn) или стоит (точка та же, f_x).
//
// Выгрузка плитки — по строке плитки запросом в ширину/16 пакетов sdram_burst (у 128×8 — 8
// пакетов, 64 слова, как прежде строка кадра), всегда все строки во всю ширину (плитки у
// правого и нижнего края — тоже: лишнее ложится в тот же
// кадр за его край, читатель его не берёт): слово SDRAM — две точки RGB565 ([15:0] —
// чётная). Каналы 8 бит сжимаются до 5/6 с упорядоченным дизерингом 4×4 по координатам
// экрана (как REG_DITHER FT812, выход которого — 6 бит на канал).
module ft_rd_pix #(
    parameter int TW_LOG = 7,               // плитка: 2^TW_LOG × 2^TH_LOG точек (1024)
    parameter int TH_LOG = 3
) (
    input  wire         clk,
    input  wire         rst_n,
    input  wire [20:0]  fb_base,        // адрес слова строки 0 кадра в SDRAM, на кадр
    input  wire [20:0]  fb_base_hi,     // адрес слова строки 512 (буфер FT 2 — в другом месте)
    input  wire         dither,
    output wire         ready,          // буферы размечены после сброса или clear
    input  wire         clear,          // разметить заново (конвейер точек пуст, выгрузки нет)

    // фрагменты
    input  wire         f_v,
    input  wire [9:0]   f_x,
    input  wire [63:0]  f_rgba,         // f_raw: {тексель, COLOR} по 32 разряда (× и ⌊/255⌋ — здесь)
    input  wire         f_raw,
    input  wire         f_slot,
    input  wire         f_clr,
    input  wire [9:0]   f_xn,           // точка следующего такта, если исполнитель продвинется
    input  wire         f_adv,          // исполнитель продвигается в этот такт
    input  wire         f_p2,           // пара: точка f_x + 1 без смешения (запись — f_w2)
    input  wire         f_w2,
    input  wire [31:0]  f_rgba2,
    input  wire         f_p2n,          // у точки f_xn может быть пара
    output wire         f_ready,
    input  wire         slot_we,
    input  wire         slot_sel,
    input  wire [63:0]  slot_data,
    output wire [1:0]   slot_busy,
    input  wire         impl_we,
    input  wire [31:0]  impl_rgba,
    input  wire [7:0]   impl_st,
    input  wire         cur_buf,
    output wire         be_empty,

    // плитки
    input  wire         tile_go,
    input  wire         tile_buf,
    input  wire [3:0]   tile_x,         // левая точка / ширину плитки
    input  wire [9:0]   tile_y,         // верхняя строка
    output wire [1:0]   buf_free,
    output wire         drain_idle,

    // SDRAM: запись плитки
    output reg          w_req,
    output reg  [20:0]  w_addr,
    output reg  [2:0]   w_len,
    input  wire         w_ack,
    input  wire         w_take,         // слово уходит: w_data в этот такт
    output wire [31:0]  w_data
);
    // --- память буферов -----------------------------------------------------------------------
    reg  [32:0] ce0 [0:511], co0 [0:511], ce1 [0:511], co1 [0:511];   // {не касались, RGBA}
    reg  [7:0]  st0 [0:1023], st1 [0:1023];
    reg  [32:0] qe0, qo0, qe1, qo1;
    reg  [7:0]  qs0, qs1;
    reg  [8:0]  ra0, ra1;               // адрес чтения цвета буфера
    reg  [9:0]  rs;                     // адрес чтения трафарета (буфер cur_buf)
    reg         we_e0, we_o0, we_e1, we_o1, we_s0, we_s1;
    reg  [8:0]  wa0, wa1;
    reg  [32:0] wd0, wd1, wdo0, wdo1;   // данные чётного и нечётного банка
    reg  [9:0]  ws;
    reg  [7:0]  wds;

    always_ff @(posedge clk) begin
        qe0 <= ce0[ra0]; qo0 <= co0[ra0];
        qe1 <= ce1[ra1]; qo1 <= co1[ra1];
        qs0 <= st0[rs];  qs1 <= st1[rs];
        if (we_e0) ce0[wa0] <= wd0;
        if (we_o0) co0[wa0] <= wdo0;
        if (we_e1) ce1[wa1] <= wd1;
        if (we_o1) co1[wa1] <= wdo1;
        if (we_s0) st0[ws] <= wds;
        if (we_s1) st1[ws] <= wds;
    end

    // --- места контекста и значения по умолчанию -------------------------------------------------
    reg  [63:0] slot [0:1];
    reg  [31:0] impl_c [0:1];
    reg  [7:0]  impl_s [0:1];
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            slot[0] <= '0; slot[1] <= '0;
            impl_c[0] <= '0; impl_c[1] <= '0; impl_s[0] <= '0; impl_s[1] <= '0;
        end else begin
            if (slot_we) slot[slot_sel] <= slot_data;
            if (impl_we) begin impl_c[cur_buf] <= impl_rgba; impl_s[cur_buf] <= impl_st; end
        end
    end

    // ⌊p/255⌋ = (p + 1 + p/256)/256 для p ≤ 255·255 (цвет битмапа × COLOR — из ft_rd_exec)
    function automatic logic [7:0] d255(input logic [15:0] p);
        d255 = 8'((p + 17'd1 + {9'd0, p[15:8]}) >> 8);
    endfunction

    // --- конвейер фрагмента ------------------------------------------------------------------
    reg         a_v, a_slot, a_clr;     // A: фрагмент принят, данные буфера читаются
    reg  [9:0]  a_x;
    reg  [63:0] a_src;
    reg         a_raw;
    reg  [31:0] b1_dcr;                 // B1: что лежало в буфере (с подстановкой «не касались»)
    reg  [7:0]  b1_dsr;
    reg         b1_v, b2_v, b3_v, b4_v;
    reg  [9:0]  b1_x, b2_x, b3_x, b4_x;
    reg         b1_slot, b2_slot, b3_slot, b4_slot;
    reg         b1_clr;
    reg  [31:0] b1_src;
    reg  [31:0] b2_src, b2_dst, b3_dst;
    reg  [7:0]  b2_fs, b2_fd;
    reg         b2_pass, b3_pass;       // цвет пишется (альфа и трафарет прошли)
    reg         b2_clr, b3_clr;
    reg  [31:0] b2_clrc;                // CLEAR: новый цвет
    reg  [7:0]  b3_st, b4_st;
    // Трафарет — на двух стадиях (1.31, худший путь clk_mem был b1_slot → b2_st): на B1 —
    // сравнения и выбор операции (b2_sop с её значением b2_sr), на B2 — сама операция и маска
    // записи. Контекст места — из регистра b1_slr (взят на стадии A).
    reg  [63:0] b1_slr;
    reg  [2:0]  b2_sop;
    reg  [7:0]  b2_sr, b2_swm, b2_ds;
    reg  [3:0]  b2_cm, b3_cm;
    // произведения канал × множитель — MULT9X9 с регистрами входов и выхода (ft_mul9, REG):
    // множители из B1 входят в DSP на границе B1/B2, произведение готово в B3;
    // ⌊p/255⌋ = (p + 1 + p/256)/256
    wire [17:0] b3_ps [0:3], b3_pd [0:3];
    wire [7:0]  b1_fs, b1_fd;
    function automatic logic [8:0] div255(input logic [15:0] p);
        div255 = 9'(({1'b0, p} + 17'd1 + {9'd0, p[15:8]}) >> 8);
    endfunction
    reg  [31:0] b4_rgba;
    // вторая точка пары: идёт вместе с первой (та же строка, x + 1), без смешения
    reg         a_p2, b1_p2, b2_p2, b3_p2, b4_p2;
    reg         a_w2, b1_w2, b2_w2, b3_w2, b4_w2;
    reg  [31:0] a_s2, b1_s2, b2_s2, b3_s2, b4_s2;

    // точка x занята в следующем такте: A получит f_x (если принята), B1…B4 — A…B3
    reg  hz_hold, hz_adv, adv_q;
    wire hz  = adv_q ? hz_adv : hz_hold;
    wire acc = f_v && !hz;
    // точка x (p — у неё пара) против точки стадии sx (sp — пара): у пары — по слову
    function automatic logic hz_one(input logic sv, input logic [9:0] sx, input logic sp,
                                    input logic [9:0] x, input logic p);
        hz_one = sv && (sx == x || ((sp || p) && sx[9:1] == x[9:1]));
    endfunction
    function automatic logic hz_next(input logic [9:0] x, input logic p);
        hz_next = hz_one(acc, f_x, f_p2, x, p) || hz_one(a_v, a_x, a_p2, x, p) ||
                  hz_one(b1_v, b1_x, b1_p2, x, p) || hz_one(b2_v, b2_x, b2_p2, x, p) ||
                  hz_one(b3_v, b3_x, b3_p2, x, p);
    endfunction
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            hz_hold <= 1'b0; hz_adv <= 1'b0; adv_q <= 1'b0;
        end else begin
            hz_hold <= hz_next(f_x, f_p2);
            hz_adv  <= hz_next(f_xn, f_p2n);
            adv_q   <= f_adv;
        end
    end
    assign f_ready   = !hz;
    assign be_empty  = !a_v && !b1_v && !b2_v && !b3_v && !b4_v;
    assign slot_busy = {(a_v && a_slot) || (b1_v && b1_slot) || (b2_v && b2_slot) || (b3_v && b3_slot) ||
                        (b4_v && b4_slot),
                        (a_v && !a_slot) || (b1_v && !b1_slot) || (b2_v && !b2_slot) || (b3_v && !b3_slot) ||
                        (b4_v && !b4_slot)};

    // сравнение ALPHA_FUNC/STENCIL_FUNC из двух: меньше и равно
    function automatic logic cmp(input logic [2:0] f, input logic [7:0] v, input logic [7:0] r);
        logic lt, eq;
        lt = v < r;
        eq = v == r;
        case (f)
            3'd0: cmp = 1'b0;      3'd1: cmp = lt;     3'd2: cmp = lt || eq; 3'd3: cmp = !(lt || eq);
            3'd4: cmp = !lt;       3'd5: cmp = eq;     3'd6: cmp = !eq;      default: cmp = 1'b1;
        endcase
    endfunction
    function automatic logic [7:0] st_op(input logic [2:0] op, input logic [7:0] v, input logic [7:0] r);
        case (op)
            3'd0: st_op = 8'd0;
            3'd2: st_op = r;
            3'd3: st_op = v == 8'd255 ? v : v + 8'd1;
            3'd4: st_op = v == 8'd0 ? v : v - 8'd1;
            3'd5: st_op = ~v;
            default: st_op = v;
        endcase
    endfunction
    function automatic logic [7:0] factor(input logic [2:0] f, input logic [7:0] sa, input logic [7:0] da);
        case (f)
            3'd0: factor = 8'd0;   3'd1: factor = 8'd255; 3'd2: factor = sa;
            3'd3: factor = da;     3'd4: factor = 8'd255 - sa; 3'd5: factor = 8'd255 - da;
            default: factor = 8'd0;
        endcase
    endfunction

    // B1: что лежит в буфере (или значение строки по умолчанию)
    // A: данные буфера пришли — «не касались» заменяется значением строки по умолчанию
    wire [32:0] a_q  = cur_buf ? (a_x[0] ? qo1 : qe1) : (a_x[0] ? qo0 : qe0);
    wire [31:0] a_dc = a_q[32] ? impl_c[cur_buf] : a_q[31:0];
    wire [7:0]  a_ds = a_q[32] ? impl_s[cur_buf] : (cur_buf ? qs1 : qs0);
    wire [31:0] b1_dc = b1_dcr;
    wire [7:0]  b1_ds = b1_dsr;
    // место пишется задолго до первой точки задания и не переписывается, пока его точки в
    // конвейере (slot_busy): его можно взять в регистр уже на стадии A
    wire [63:0] b1_sl = b1_slr;
    wire [2:0]  s_af  = b1_sl[2:0];
    wire [7:0]  s_aref = b1_sl[10:3];
    wire [2:0]  s_sf  = b1_sl[13:11];
    wire [7:0]  s_sref = b1_sl[21:14], s_smask = b1_sl[29:22], s_swm = b1_sl[37:30];
    wire [2:0]  s_sfail = b1_sl[40:38], s_spass = b1_sl[43:41];
    wire [2:0]  s_bsrc = b1_sl[46:44], s_bdst = b1_sl[49:47];
    wire [3:0]  s_cm = b1_sl[53:50];
    wire [7:0]  s_cst = b1_sl[61:54];
    wire        s_cs = b1_sl[62], s_cc = b1_sl[63];
    assign b1_fs = factor(s_bsrc, b1_src[31:24], b1_dc[31:24]);
    assign b1_fd = factor(s_bdst, b1_src[31:24], b1_dc[31:24]);
    wire        b1_at = cmp(s_af, b1_src[31:24], s_aref);
    wire        b1_sok = cmp(s_sf, b1_ds & s_smask, s_sref & s_smask);
    // операция трафарета: CLEAR — замена на CLEAR_STENCIL (s_cs) или ничего, точка — по альфе и
    // сравнению трафарета (не прошла альфу — трафарет не трогается)
    wire [2:0]  b1_sop = b1_clr ? (s_cs ? 3'd2 : 3'd1) : (b1_at ? (b1_sok ? s_spass : s_sfail) : 3'd1);
    wire [7:0]  b2_nst = (b2_ds & ~b2_swm) | (st_op(b2_sop, b2_ds, b2_sr) & b2_swm);
    // CLEAR: {A, R, G, B}, маска COLOR_MASK {R, G, B, A}
    wire [31:0] b1_cc = {s_cc && s_cm[0] ? b1_src[31:24] : b1_dc[31:24],
                         s_cc && s_cm[3] ? b1_src[23:16] : b1_dc[23:16],
                         s_cc && s_cm[2] ? b1_src[15:8]  : b1_dc[15:8],
                         s_cc && s_cm[1] ? b1_src[7:0]   : b1_dc[7:0]};

    // битмап: тексель × COLOR — от регистра стадии A (a_src = {тексель, COLOR}) к b1_src (1.31:
    // раньше умножал ft_rd_exec сразу за умножением BILINEAR — оба стояли на худшем пути clk_mem)
    wire [17:0] a_tc [0:3];
    genvar gt;
    for (gt = 0; gt < 4; gt = gt + 1) begin : g_tex
        ft_mul9 u_tc (.clk(clk), .a({1'b0, a_src[32 + 8 * gt +: 8]}), .b({1'b0, a_src[8 * gt +: 8]}),
                      .p(a_tc[gt]));
    end

    genvar gc;
    for (gc = 0; gc < 4; gc = gc + 1) begin : g_mul
        ft_mul9 #(.REG(1'b1)) u_ps (.clk(clk), .a({1'b0, b1_src[8 * gc +: 8]}), .b({1'b0, b1_fs}),
                                    .p(b3_ps[gc]));
        ft_mul9 #(.REG(1'b1)) u_pd (.clk(clk), .a({1'b0, b1_dc[8 * gc +: 8]}), .b({1'b0, b1_fd}),
                                    .p(b3_pd[gc]));
    end

    // B3: смешение
    reg [31:0] b3_mix;
    always_comb begin
        for (int c = 0; c < 4; c++) begin : ch
            logic [8:0] s;
            s = div255(b3_ps[c][15:0]) + div255(b3_pd[c][15:0]);
            b3_mix[8 * c +: 8] = s > 9'd255 ? 8'd255 : s[7:0];
        end
    end
    // COLOR_MASK {R, G, B, A} → байты {A, R, G, B}
    wire [3:0]  b3_bm = {b3_cm[0], b3_cm[3], b3_cm[2], b3_cm[1]};
    reg  [31:0] b3_new;
    always_comb begin
        for (int c = 0; c < 4; c++)
            b3_new[8 * c +: 8] = (b3_pass && b3_bm[c]) ? b3_mix[8 * c +: 8] : b3_dst[8 * c +: 8];
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            a_v <= 1'b0; a_slot <= 1'b0; a_clr <= 1'b0; a_x <= '0; a_src <= '0; a_raw <= 1'b0;
            a_p2 <= 1'b0; b1_p2 <= 1'b0; b2_p2 <= 1'b0; b3_p2 <= 1'b0; b4_p2 <= 1'b0;
            a_w2 <= 1'b0; b1_w2 <= 1'b0; b2_w2 <= 1'b0; b3_w2 <= 1'b0; b4_w2 <= 1'b0;
            a_s2 <= '0; b1_s2 <= '0; b2_s2 <= '0; b3_s2 <= '0; b4_s2 <= '0;
            b1_dcr <= '0; b1_dsr <= '0;
            b1_v <= 1'b0; b2_v <= 1'b0; b3_v <= 1'b0; b4_v <= 1'b0;
            b1_x <= '0; b2_x <= '0; b3_x <= '0; b4_x <= '0;
            b1_slot <= 1'b0; b2_slot <= 1'b0; b3_slot <= 1'b0; b4_slot <= 1'b0;
            b1_clr <= 1'b0; b1_src <= '0; b2_src <= '0; b2_dst <= '0; b3_dst <= '0;
            b2_fs <= '0; b2_fd <= '0; b2_pass <= 1'b0; b3_pass <= 1'b0;
            b2_clr <= 1'b0; b3_clr <= 1'b0; b2_clrc <= '0;
            b3_st <= '0; b4_st <= '0; b2_cm <= '0; b3_cm <= '0; b4_rgba <= '0;
            b1_slr <= '0; b2_sop <= 3'd1; b2_sr <= '0; b2_swm <= '0; b2_ds <= '0;
        end else begin
            // B1
            // A
            a_v     <= acc;
            a_p2    <= acc && f_p2;
            a_w2    <= f_w2;
            a_s2    <= f_rgba2;
            a_x     <= f_x;
            a_slot  <= f_slot;
            a_clr   <= f_clr;
            a_src   <= f_rgba;
            a_raw   <= f_raw;
            // B1
            b1_v    <= a_v;
            b1_slr  <= slot[a_slot];
            b1_p2   <= a_p2;  b1_w2 <= a_w2;  b1_s2 <= a_s2;
            b1_x    <= a_x;
            b1_slot <= a_slot;
            b1_clr  <= a_clr;
            b1_src  <= a_raw ? {d255(a_tc[3][15:0]), d255(a_tc[2][15:0]), d255(a_tc[1][15:0]), d255(a_tc[0][15:0])}
                             : a_src[31:0];
            b1_dcr  <= a_dc;
            b1_dsr  <= a_ds;
            // B2
            b2_v    <= b1_v;
            b2_p2   <= b1_p2; b2_w2 <= b1_w2; b2_s2 <= b1_s2;
            b2_x    <= b1_x;
            b2_slot <= b1_slot;
            b2_clr  <= b1_clr;
            b2_src  <= b1_src;
            b2_dst  <= b1_dc;
            b2_clrc <= b1_cc;
            b2_cm   <= s_cm;

            b2_pass <= !b1_clr && b1_at && b1_sok;
            b2_sop  <= b1_sop;
            b2_sr   <= b1_clr ? s_cst : s_sref;
            b2_swm  <= s_swm;
            b2_ds   <= b1_ds;
            // B3
            b3_v    <= b2_v;
            b3_p2   <= b2_p2; b3_w2 <= b2_w2; b3_s2 <= b2_s2;
            b3_x    <= b2_x;
            b3_slot <= b2_slot;
            b3_clr  <= b2_clr;
            b3_dst  <= b2_clr ? b2_clrc : b2_dst;
            b3_pass <= b2_pass;
            b3_cm   <= b2_cm;
            b3_st   <= b2_nst;
            // B4
            b4_v    <= b3_v;
            b4_p2   <= b3_p2; b4_w2 <= b3_w2; b4_s2 <= b3_s2;
            b4_x    <= b3_x;
            b4_slot <= b3_slot;
            b4_rgba <= b3_new;
            b4_st   <= b3_st;
        end
    end

    // --- разметка после сброса, выгрузка ------------------------------------------------------
    reg  [9:0]  init_cnt;               // [9] — разметка идёт
    assign ready = !init_cnt[9];

    // очередь плиток: не больше двух (по буферу)
    reg         lq_v [0:1];
    reg         lq_b [0:1];
    reg  [9:0]  lq_y [0:1];
    reg  [3:0]  lq_x [0:1];
    reg         lq_rd, lq_wr;
    reg  [1:0]  full;                   // буфер ждёт выгрузки или выгружается

    typedef enum logic [2:0] { D_IDLE, D_P1, D_P2, D_RUN, D_END } dstate_t;
    dstate_t dst_q;
    reg         d_buf;
    reg  [9:0]  d_y;                    // верхняя строка плитки
    reg  [3:0]  d_tx;                   // левая точка / ширину плитки
    reg  [8:0]  d_ra;                   // слово, которое держит q (адрес чтения): {строка, слово}
    reg  [8:0]  d_qi;                   // какое слово сейчас в q
    reg  [8:0]  d_k;                    // слово, уходящее при следующем w_take
    reg  [6:0]  d_iss;                  // пакетов запрошено: {строка, пакет в строке}
    reg  [9:0]  d_taken;                // слов ушло
    reg  [31:0] d_w;                    // слово для следующего w_take
    localparam logic [6:0] D_BURSTS = 7'd64;   // плитка — 512 слов: строки × (ширина / 16) пакетов
    localparam int PB = TW_LOG - 4;            // разрядов пакета в строке плитки
    localparam int KB = TW_LOG - 1;            // разрядов слова в строке плитки

    assign buf_free   = ~full & {2{ready}};
    assign drain_idle = dst_q == D_IDLE && !lq_v[0] && !lq_v[1] && !tile_go;
    assign w_data     = d_w;

    // RGB565 с дизерингом
    function automatic logic [3:0] bayer(input logic [1:0] x, input logic [1:0] y);
        case ({y, x})
            4'h0: bayer = 4'd0;  4'h1: bayer = 4'd8;  4'h2: bayer = 4'd2;  4'h3: bayer = 4'd10;
            4'h4: bayer = 4'd12; 4'h5: bayer = 4'd4;  4'h6: bayer = 4'd14; 4'h7: bayer = 4'd6;
            4'h8: bayer = 4'd3;  4'h9: bayer = 4'd11; 4'hA: bayer = 4'd1;  4'hB: bayer = 4'd9;
            4'hC: bayer = 4'd15; 4'hD: bayer = 4'd7;  4'hE: bayer = 4'd13; default: bayer = 4'd5;
        endcase
    endfunction
    function automatic logic [15:0] to565(input logic [31:0] c, input logic [3:0] t, input logic d);
        logic [8:0] r, g, b;
        r = {1'b0, c[23:16]} + (d ? {6'd0, t[3:1]} : 9'd0);
        g = {1'b0, c[15:8]}  + (d ? {7'd0, t[3:2]} : 9'd0);
        b = {1'b0, c[7:0]}   + (d ? {6'd0, t[3:1]} : 9'd0);
        to565 = {r[8] ? 5'h1F : r[7:3], g[8] ? 6'h3F : g[7:2], b[8] ? 5'h1F : b[7:3]};
    endfunction
    wire [32:0] dq_e = d_buf ? qe1 : qe0;
    wire [32:0] dq_o = d_buf ? qo1 : qo0;
    wire [31:0] dc_e = dq_e[32] ? impl_c[d_buf] : dq_e[31:0];
    wire [31:0] dc_o = dq_o[32] ? impl_c[d_buf] : dq_o[31:0];
    // точка экрана: x = TW·d_tx + 2·слово + {0, 1}, y = d_y + строка (d_y, TW·d_tx кратны 4)
    wire [31:0] d_conv = {to565(dc_o, bayer({d_qi[0], 1'b1}, d_qi[KB+1:KB]), dither),
                          to565(dc_e, bayer({d_qi[0], 1'b0}, d_qi[KB+1:KB]), dither)};

    // порты памяти: буфер cur_buf — фрагменты, выгружаемый — выгрузка, после сброса — разметка.
    // Выгрузка: q держит слово d_ra; слово ушло (w_take) — читается следующее, так что к
    // следующему w_take в d_w уже его цвет.
    wire        d_on = dst_q != D_IDLE;
    wire [8:0]  d_rnext = d_ra + {8'd0, dst_q == D_RUN && w_take};
    always_comb begin
        ra0 = (d_on && !d_buf) ? d_rnext : f_x[9:1];
        ra1 = (d_on && d_buf)  ? d_rnext : f_x[9:1];
        rs  = f_x;
        we_e0 = 1'b0; we_o0 = 1'b0; we_e1 = 1'b0; we_o1 = 1'b0; we_s0 = 1'b0; we_s1 = 1'b0;
        wa0 = b4_x[9:1]; wa1 = b4_x[9:1];
        wd0 = {1'b0, b4_rgba}; wd1 = {1'b0, b4_rgba};
        wdo0 = b4_p2 ? {1'b0, b4_s2} : {1'b0, b4_rgba};
        wdo1 = wdo0;
        ws  = b4_x; wds = b4_st;
        if (init_cnt[9]) begin
            we_e0 = 1'b1; we_o0 = 1'b1; we_e1 = 1'b1; we_o1 = 1'b1;
            wa0 = init_cnt[8:0]; wa1 = init_cnt[8:0];
            wd0 = {1'b1, 32'd0}; wd1 = {1'b1, 32'd0}; wdo0 = {1'b1, 32'd0}; wdo1 = {1'b1, 32'd0};
        end else begin
            // фрагмент
            if (b4_v && !cur_buf) begin we_e0 = !b4_x[0]; we_o0 = b4_x[0] || (b4_p2 && b4_w2); we_s0 = 1'b1; end
            if (b4_v && cur_buf)  begin we_e1 = !b4_x[0]; we_o1 = b4_x[0] || (b4_p2 && b4_w2); we_s1 = 1'b1; end
            // выгрузка: слово ушло — точки снова «не касались»
            if (dst_q == D_RUN && w_take) begin
                if (!d_buf) begin we_e0 = 1'b1; we_o0 = 1'b1; wa0 = d_k; wd0 = {1'b1, 32'd0}; wdo0 = {1'b1, 32'd0}; end
                else        begin we_e1 = 1'b1; we_o1 = 1'b1; wa1 = d_k; wd1 = {1'b1, 32'd0}; wdo1 = {1'b1, 32'd0}; end
            end
        end
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            init_cnt <= 10'h200;
            lq_v[0] <= 1'b0; lq_v[1] <= 1'b0; lq_b[0] <= 1'b0; lq_b[1] <= 1'b0;
            lq_y[0] <= '0; lq_y[1] <= '0; lq_x[0] <= '0; lq_x[1] <= '0;
            lq_rd <= 1'b0; lq_wr <= 1'b0; full <= '0;
            dst_q <= D_IDLE; d_buf <= 1'b0; d_y <= '0; d_tx <= '0; d_ra <= '0; d_qi <= '0; d_k <= '0;
            d_iss <= '0; d_taken <= '0; d_w <= '0;
            w_req <= 1'b0; w_addr <= '0; w_len <= '0;
        end else begin
            if (init_cnt[9])
                init_cnt <= init_cnt[8:0] == 9'h1FF ? 10'd0 : init_cnt + 10'd1;
            if (clear)
                init_cnt <= 10'h200;

            if (tile_go) begin
                lq_v[lq_wr] <= 1'b1;
                lq_b[lq_wr] <= tile_buf;
                lq_y[lq_wr] <= tile_y;
                lq_x[lq_wr] <= tile_x;
                lq_wr <= !lq_wr;
                full[tile_buf] <= 1'b1;
            end

            d_qi <= d_rnext;
            case (dst_q)
            D_IDLE: if (lq_v[lq_rd]) begin
                lq_v[lq_rd] <= 1'b0;
                lq_rd   <= !lq_rd;
                d_buf   <= lq_b[lq_rd];
                d_y     <= lq_y[lq_rd];
                d_tx    <= lq_x[lq_rd];
                d_ra    <= 9'd0;
                d_k     <= 9'd0;
                d_iss   <= '0;
                d_taken <= '0;
                dst_q   <= D_P1;
            end
            D_P1: begin                       // читается слово 0
                d_ra  <= 9'd1;
                dst_q <= D_P2;
            end
            D_P2: begin                       // q = слово 0, читается слово 1
                d_w   <= d_conv;
                dst_q <= D_RUN;
            end
            D_RUN: begin
                // запросы: строка плитки — ширина/16 пакетов, следующий — после ack предыдущего
                if (!w_req && d_iss != D_BURSTS) begin
                    w_req  <= 1'b1;
                    begin : row_addr
                        logic [9:0] y;
                        y = d_y + 10'(d_iss[5:PB]);
                        w_addr <= (y[9] ? fb_base_hi : fb_base) +
                                  {3'b000, y[8:0], d_tx[9-TW_LOG:0], d_iss[PB-1:0], 3'b000};
                    end
                    w_len  <= 3'((1 << PB) - 1);
                end
                if (w_req && w_ack) begin
                    w_req <= 1'b0;
                    d_iss <= d_iss + 7'(w_len) + 7'd1;
                end
                if (w_take) begin
                    // q держит слово d_k + 1
                    d_w     <= d_conv;
                    d_ra    <= d_ra + 9'd1;
                    d_k     <= d_k + 9'd1;
                    d_taken <= d_taken + 10'd1;
                    if (d_taken + 10'd1 == {D_BURSTS, 3'b000})
                        dst_q <= D_END;
                end
            end
            D_END: begin
                full[d_buf] <= 1'b0;
                dst_q <= D_IDLE;
            end
            default: dst_q <= D_IDLE;
            endcase
        end
    end
endmodule
