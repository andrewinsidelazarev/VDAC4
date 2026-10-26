// SPDX-License-Identifier: Apache-2.0
//
// Оверлеи ESP поверх выходного видео: N прямоугольников с картинкой, которую рисует ESP
// (меню настроек Tang, обложка в плеере, спектр). Стоит между fb_scanout и кодерами TMDS:
// берёт готовый поток (цвет, DE, HS, VS) и отдаёт его на LAT тактов позже вместе с
// синхроимпульсами — развёртка не меняется. Оверлей k + 1 лежит поверх оверлея k.
//
// ХОЛСТЫ — в SDRAM, в регионе с адреса слова REGION (512 КБ): точка — RGB565, в слове две
// точки ([15:0] — левая). Строка холста занимает ⌈w/16⌉·8 слов (ширина, округлённая до
// 16 точек: строки выровнены на пакет SDRAM); строка r оверлея — с base + r·шаг слов
// региона. ESP пишет холсты окном LINK (fb_link: адреса 0x80000…0xFFFFF — слова региона
// подряд), запись идёт через очередь в домен SDRAM пакетами по 8 слов.
//
// НАСТРОЙКИ оверлея k (регистры LINK, ts_fb_core) берутся в начале кадра: вкл., ×2 (точка
// холста — 2×2 точки вывода), прозрачность a (0…15: вывод = картинка + ⌊(оверлей −
// картинка)·(a + 1)/16⌋; 15 — непрозрачный), положение (x, y) на выводе, размер холста
// (w ≤ 1024 — шире не показывается, h), начало холста в регионе. Вне прямоугольника —
// картинка как есть.
//
// СТРОКИ ХОЛСТА читаются заранее: в конце строки вывода y заказывается строка холста для
// строки y + 2 (в начале кадра — для строк 0 и 1, по первым двум HS гашения). У оверлея два
// буфера строки (чётная и нечётная строка холста): заказанная строка пишется в свой, пока
// показывается другая. Не успела — видна прежняя. Чтение SDRAM на строку вывода: оба
// оверлея вместе разумно держать в ~320 словах (640 точек холста ×1).
//
// Конвейер (такт = точка): 0 — координаты, адрес буфера; 1 — слово буфера; 2 — точка,
// каналы 8 бит; 3 — оверлей 0; 4 — оверлей 1; выход — регистр. Видео задержано на LAT.
module fb_ovl #(
    parameter int          N      = 2,
    parameter logic [20:0] REGION = 21'h108000,
    parameter bit          HS_NEG = 1'b0,
    parameter bit          VS_NEG = 1'b1
) (
    // --- домен пикселей ---------------------------------------------------------------------
    input  wire              pclk,
    input  wire              prst_n,
    input  wire [N-1:0]      cfg_on,
    input  wire [N-1:0]      cfg_x2,
    input  wire [N*4-1:0]    cfg_alpha,
    input  wire [N*11-1:0]   cfg_x,
    input  wire [N*10-1:0]   cfg_y,
    input  wire [N*11-1:0]   cfg_w,
    input  wire [N*10-1:0]   cfg_h,
    input  wire [N*17-1:0]   cfg_base,
    // окно записи LINK: слово ow_addr региона
    input  wire              ow_stb,
    input  wire [16:0]       ow_addr,
    input  wire [31:0]       ow_data,
    output reg               ow_ovf,         // очередь записи переполнялась (до сброса)
    // видео
    input  wire [7:0]        in_r,
    input  wire [7:0]        in_g,
    input  wire [7:0]        in_b,
    input  wire              in_de,
    input  wire              in_hs,
    input  wire              in_vs,
    output reg  [7:0]        out_r,
    output reg  [7:0]        out_g,
    output reg  [7:0]        out_b,
    output reg               out_de,
    output reg               out_hs,
    output reg               out_vs,

    // --- домен SDRAM --------------------------------------------------------------------------
    input  wire              mclk,
    input  wire              mrst_n,
    output reg               s_req,
    output reg               s_we,
    output reg  [20:0]       s_addr,
    output reg  [2:0]        s_len,          // пакетов − 1
    input  wire              s_ack,
    input  wire              s_wr_take,
    input  wire [5:0]        s_wr_idx,
    output wire [31:0]       s_wdata,
    output wire [3:0]        s_wmask,
    input  wire              s_rd_valid,
    input  wire [5:0]        s_rd_idx,
    input  wire [31:0]       s_rd_data
);
    // =========================================================================================
    // Домен пикселей: координаты, настройки кадра, заявки строк
    // =========================================================================================
    wire       vs_act = in_vs ^ VS_NEG;
    wire       hs_act = in_hs ^ HS_NEG;
    reg [10:0] x, y;
    reg        de_q, vs_q, hs_q;
    always_ff @(posedge pclk or negedge prst_n) begin
        if (!prst_n) begin
            x <= '0; y <= '0; de_q <= 1'b0; vs_q <= 1'b0; hs_q <= 1'b0;
        end else begin
            de_q <= in_de;
            vs_q <= vs_act;
            hs_q <= hs_act;
            x    <= in_de ? x + 11'd1 : 11'd0;
            if (vs_act) y <= '0;
            else if (de_q && !in_de) y <= y + 11'd1;
        end
    end
    wire frame_start = vs_act && !vs_q;
    wire hs_edge     = hs_act && !hs_q;
    wire line_end    = de_q && !in_de;

    // настройки кадра
    reg          f_on   [0:N-1];
    reg          f_x2   [0:N-1];
    reg  [4:0]   f_a1   [0:N-1];             // a + 1
    reg  [10:0]  f_x    [0:N-1];
    reg  [10:0]  f_y    [0:N-1];
    reg  [11:0]  f_sw   [0:N-1];             // ширина на выводе
    reg  [11:0]  f_ye   [0:N-1];             // строка вывода под оверлеем (y + высота)
    reg  [16:0]  f_base [0:N-1];
    reg  [9:0]   f_words[0:N-1];             // шаг строки, слов
    // заявки: {переключатель, строка}; строка держится до следующей заявки (≥ строки вывода)
    reg  [N-1:0] rq_tog;
    reg  [9:0]   rq_row [0:N-1];
    reg  [10:0]  rq_last[0:N-1];             // последняя заказанная строка холста (+1: нет)
    reg  [1:0]   vb_cnt;                     // HS после начала кадра: заявки строк 0 и 1
    // Строка вывода в оверлее и чётность её строки холста (буфер строки) — из заявок: _b —
    // заявленная строка (выводится через одну), _a — текущая. По заявке _a ← _b, _b ← новая:
    // две заявки в гашении — строки 0 и 1, затем в конце строки y — строка y + 2.
    reg  [N-1:0] vy_a, vy_b, cy_a, cy_b;

    // строка холста оверлея k для строки вывода ny
    function automatic logic [10:0] row_of(input int k, input logic [11:0] ny);
        logic [11:0] d;
        d = ny - {1'b0, f_y[k]};
        row_of = f_x2[k] ? {1'b0, d[10:1]} : d[10:0];
    endfunction

    always_ff @(posedge pclk or negedge prst_n) begin
        if (!prst_n) begin
            rq_tog <= '0; vb_cnt <= '0; vy_a <= '0; vy_b <= '0; cy_a <= '0; cy_b <= '0;
            for (int k = 0; k < N; k++) begin
                f_on[k] <= 1'b0; f_x2[k] <= 1'b0; f_a1[k] <= 5'd16; f_x[k] <= '0; f_y[k] <= '0;
                f_sw[k] <= '0; f_ye[k] <= '0; f_base[k] <= '0; f_words[k] <= '0;
                rq_row[k] <= '0; rq_last[k] <= 11'h7FF;
            end
        end else if (frame_start) begin
            vb_cnt <= 2'd2;
            vy_a <= '0; vy_b <= '0;
            for (int k = 0; k < N; k++) begin
                // шире 1024 точек буфер строки не вмещает — такой оверлей не показывается
                f_on[k]    <= cfg_on[k] && cfg_w[k*11 +: 11] != 11'd0 && cfg_w[k*11 +: 11] <= 11'd1024
                              && cfg_h[k*10 +: 10] != 10'd0;
                f_x2[k]    <= cfg_x2[k];
                f_a1[k]    <= {1'b0, cfg_alpha[k*4 +: 4]} + 5'd1;
                f_x[k]     <= cfg_x[k*11 +: 11];
                f_y[k]     <= {1'b0, cfg_y[k*10 +: 10]};
                f_sw[k]    <= cfg_x2[k] ? {cfg_w[k*11 +: 11], 1'b0} : {1'b0, cfg_w[k*11 +: 11]};
                f_ye[k]    <= {2'b0, cfg_y[k*10 +: 10]} +
                              (cfg_x2[k] ? {1'b0, cfg_h[k*10 +: 10], 1'b0} : {2'b0, cfg_h[k*10 +: 10]});
                f_base[k]  <= cfg_base[k*17 +: 17];
                f_words[k] <= 10'((cfg_w[k*11 +: 11] + 11'd15) >> 4) << 3;
                rq_last[k] <= 11'h7FF;
            end
        end else begin : req
            // заявка: в гашении — строки 0 и 1 по первым двум HS (между ними — строка),
            // дальше — строка y + 2 в конце строки y
            logic        go;
            logic [11:0] ny;
            go = vb_cnt != 2'd0 ? hs_edge : line_end;
            ny = vb_cnt != 2'd0 ? {11'd0, vb_cnt == 2'd1} : {1'b0, y} + 12'd2;
            if (go) begin
                if (vb_cnt != 2'd0) vb_cnt <= vb_cnt - 2'd1;
                for (int k = 0; k < N; k++) begin
                    logic [10:0] r;
                    logic        v;
                    r = row_of(k, ny);
                    v = f_on[k] && ny >= {1'b0, f_y[k]} && ny < f_ye[k];
                    vy_a[k] <= vy_b[k]; vy_b[k] <= v;
                    cy_a[k] <= cy_b[k]; cy_b[k] <= r[0];
                    if (v && r != rq_last[k]) begin
                        rq_row[k]  <= r[9:0];
                        rq_last[k] <= r;
                        rq_tog[k]  <= ~rq_tog[k];
                    end
                end
            end
        end
    end

    // =========================================================================================
    // Буферы строк: две строки холста на оверлей (запись — домен SDRAM, чтение — пикселей)
    // =========================================================================================
    reg  [9:0]  lb_wa;                       // {строка[0], слово[8:0]}: w ≤ 1024 — 512 слов
    reg  [N-1:0] lb_we;
    reg  [31:0] lb_wd;
    wire [31:0] lb_q [0:N-1];
    reg  [9:0]  lb_ra [0:N-1];
    genvar gk;
    generate for (gk = 0; gk < N; gk++) begin : g_lb
        reg [31:0] mem [0:1023];
        reg [31:0] q;
        always_ff @(posedge mclk) if (lb_we[gk]) mem[lb_wa] <= lb_wd;
        always_ff @(posedge pclk) q <= mem[lb_ra[gk]];
        assign lb_q[gk] = q;
    end endgenerate

    // =========================================================================================
    // Домен пикселей: смешение
    // =========================================================================================
    // такт 0: в оверлее ли точка, адрес слова буфера
    reg  [N-1:0] s1_in, s2_in;
    reg  [N-1:0] s1_hi;                      // точка — старшая половина слова
    reg  [7:0]   s1_r, s1_g, s1_b, s2_r, s2_g, s2_b;
    reg  [2:0]   s1_sync, s2_sync, s3_sync, s4_sync;   // {DE, HS, VS}
    reg  [15:0]  s2_c [0:N-1];               // точка оверлея k, RGB565
    // dx — точка от левого края оверлея; dx < 0 (разряд 11) — левее
    reg  [11:0]  dx [0:N-1];
    always_comb begin
        for (int k = 0; k < N; k++) begin
            logic [10:0] cx;
            dx[k] = {1'b0, x} - {1'b0, f_x[k]};
            cx = f_x2[k] ? {1'b0, dx[k][10:1]} : dx[k][10:0];
            lb_ra[k] = {cy_a[k], cx[9:1]};
        end
    end
    always_ff @(posedge pclk or negedge prst_n) begin
        if (!prst_n) begin
            s1_in <= '0; s2_in <= '0; s1_hi <= '0;
            s1_r <= '0; s1_g <= '0; s1_b <= '0; s2_r <= '0; s2_g <= '0; s2_b <= '0;
            s1_sync <= '0; s2_sync <= '0;
            for (int k = 0; k < N; k++) s2_c[k] <= '0;
        end else begin
            for (int k = 0; k < N; k++) begin
                s1_in[k] <= vy_a[k] && in_de && !dx[k][11] && dx[k] < f_sw[k];
                s1_hi[k] <= f_x2[k] ? dx[k][1] : dx[k][0];
                s2_c[k]  <= s1_hi[k] ? lb_q[k][31:16] : lb_q[k][15:0];
            end
            s1_r <= in_r; s1_g <= in_g; s1_b <= in_b; s1_sync <= {in_de, in_hs, in_vs};
            s2_r <= s1_r; s2_g <= s1_g; s2_b <= s1_b; s2_sync <= s1_sync; s2_in <= s1_in;
        end
    end

    // каналы точки оверлея — 8 бит (повтором старших разрядов)
    function automatic logic [7:0] ex5(input logic [4:0] v); ex5 = {v, v[4:2]}; endfunction
    function automatic logic [7:0] ex6(input logic [5:0] v); ex6 = {v, v[5:4]}; endfunction

    // оверлей k: v + ⌊(c − v)·(a + 1)/16⌋ — (c − v) × (a + 1) со знаком в MULT9X9. Сумма всегда
    // в 0…255 (при c < v она ≥ c, при c ≥ v — ≤ c), поэтому хватает 8 разрядов.
    reg  [7:0]   bv_r [0:N], bv_g [0:N], bv_b [0:N];   // вход ступени k (bv[N] — выход)
    reg  [N-1:0] bin_q [0:N];                          // в оверлее (ступени после k)
    reg  [15:0]  bc_q  [0:N-1][0:N-1];                 // точка оверлея j на ступени k
    always_comb begin
        bv_r[0] = s2_r; bv_g[0] = s2_g; bv_b[0] = s2_b;
    end
    generate for (gk = 0; gk < N; gk++) begin : g_bl
        wire [15:0] c   = gk == 0 ? s2_c[0] : bc_q[gk][gk];
        wire        on  = gk == 0 ? s2_in[0] : bin_q[gk][gk];
        wire [7:0]  cr  = ex5(c[15:11]), cg = ex6(c[10:5]), cb = ex5(c[4:0]);
        wire [7:0]  vr  = bv_r[gk], vg = bv_g[gk], vb = bv_b[gk];
        wire [8:0]  dr  = {1'b0, cr} - {1'b0, vr};       // c − v со знаком
        wire [8:0]  dg  = {1'b0, cg} - {1'b0, vg};
        wire [8:0]  db  = {1'b0, cb} - {1'b0, vb};
        wire [17:0] pr, pg, pb;
        ft_mul9 #(.SIGNED(1'b1)) mr (.clk(pclk), .a(dr), .b({4'd0, f_a1[gk]}), .p(pr));
        ft_mul9 #(.SIGNED(1'b1)) mg (.clk(pclk), .a(dg), .b({4'd0, f_a1[gk]}), .p(pg));
        ft_mul9 #(.SIGNED(1'b1)) mb (.clk(pclk), .a(db), .b({4'd0, f_a1[gk]}), .p(pb));
        always_ff @(posedge pclk or negedge prst_n) begin
            if (!prst_n) begin
                bv_r[gk + 1] <= '0; bv_g[gk + 1] <= '0; bv_b[gk + 1] <= '0;
            end else begin
                // ⌊p/16⌋ — p[17:4]; сумма по модулю 256 верна, раз результат в 0…255
                bv_r[gk + 1] <= on ? vr + pr[11:4] : vr;
                bv_g[gk + 1] <= on ? vg + pg[11:4] : vg;
                bv_b[gk + 1] <= on ? vb + pb[11:4] : vb;
            end
        end
    end endgenerate
    // точки и признаки оверлеев выше — вдоль ступеней
    always_ff @(posedge pclk or negedge prst_n) begin
        if (!prst_n) begin
            for (int k = 0; k <= N; k++) bin_q[k] <= '0;
            for (int k = 0; k < N; k++) for (int j = 0; j < N; j++) bc_q[k][j] <= '0;
        end else begin
            bin_q[1] <= s2_in;
            for (int j = 0; j < N; j++) bc_q[1 < N ? 1 : 0][j] <= s2_c[j];
            for (int k = 2; k <= N; k++) bin_q[k] <= bin_q[k - 1];
            for (int k = 2; k < N; k++) for (int j = 0; j < N; j++) bc_q[k][j] <= bc_q[k - 1][j];
        end
    end
    // синхроимпульсы — на те же N ступеней, выход — регистр
    reg [2:0] sync_d [0:N];
    always_ff @(posedge pclk or negedge prst_n) begin
        if (!prst_n) begin
            for (int k = 0; k <= N; k++) sync_d[k] <= '0;
            out_r <= '0; out_g <= '0; out_b <= '0; out_de <= 1'b0; out_hs <= 1'b0; out_vs <= 1'b0;
        end else begin
            sync_d[0] <= s2_sync;
            for (int k = 1; k <= N; k++) sync_d[k] <= sync_d[k - 1];
            out_r <= bv_r[N]; out_g <= bv_g[N]; out_b <= bv_b[N];
            {out_de, out_hs, out_vs} <= sync_d[N - 1];
        end
    end

    // =========================================================================================
    // Запись холстов: LINK → очередь → пакеты SDRAM по 8 слов
    // =========================================================================================
    wire        wq_full, wq_empty;
    wire [48:0] wq_q;
    reg         wq_rd;
    async_fifo #(.WIDTH(49), .ADDR_BITS(5)) wq (
        .wr_clk(pclk), .wr_reset_n(prst_n), .wr_en(ow_stb), .wr_data({ow_addr, ow_data}),
        .wr_full(wq_full),
        .rd_clk(mclk), .rd_reset_n(mrst_n), .rd_en(wq_rd), .rd_data(wq_q), .rd_empty(wq_empty)
    );
    always_ff @(posedge pclk or negedge prst_n)
        if (!prst_n) ow_ovf <= 1'b0;
        else if (ow_stb && wq_full) ow_ovf <= 1'b1;

    // группа: 8 слов одного пакета (адрес слова [16:3]), маска пришедших
    reg  [31:0] g_data [0:7];
    reg  [7:0]  g_mask;
    reg  [13:0] g_blk;
    reg  [6:0]  g_idle;                      // тактов без новых слов при неполной группе
    // Слово очереди: wq_rd (такт t) → на wq_q после t + 1 → в h_q на t + 2 — всегда, и пока
    // пакет уходит в SDRAM. Прежде слово ждало группы прямо на wq_q, а чтение, начатое в такт
    // отправки пакета, затирало его: терялось первое слово каждого следующего пакета (меню
    // на плате — вертикальные полосы через 16 точек, 18.10.2026; tb/tb_fb_ovl.sv).
    reg         wq_rd_q;                     // слово на wq_q с этого такта
    reg         h_v;                         // h_q ждёт своей группы
    reg  [48:0] h_q;
    wire [13:0] h_blk = h_q[48:35];
    wire [2:0]  h_wi  = h_q[34:32];
    assign s_wdata = g_data[s_wr_idx[2:0]];
    assign s_wmask = {4{g_mask[s_wr_idx[2:0]]}};

    // =========================================================================================
    // Домен SDRAM: заявки строк и автомат запросов
    // =========================================================================================
    reg  [2:0]  rq_s [0:N-1];                // синхронизация переключателей
    reg  [N-1:0] pend;
    reg  [9:0]  pend_row [0:N-1];
    always_ff @(posedge mclk or negedge mrst_n) begin
        if (!mrst_n) begin
            for (int k = 0; k < N; k++) begin rq_s[k] <= '0; pend_row[k] <= '0; end
        end else begin
            for (int k = 0; k < N; k++) begin
                rq_s[k] <= {rq_s[k][1:0], rq_tog[k]};
                if (rq_s[k][2] ^ rq_s[k][1]) pend_row[k] <= rq_row[k];   // устойчива: заявки редки
            end
        end
    end

    typedef enum logic [2:0] { M_IDLE, M_MUL, M_RREQ, M_RDATA, M_WREQ, M_WDATA } mstate_t;
    mstate_t mst;
    reg  [$clog2(N > 1 ? N : 2)-1:0] r_k;    // оверлей, чья строка читается
    reg         r_half;
    reg  [20:0] r_addr;                      // слово SDRAM следующего куска
    reg  [9:0]  r_left;                      // слов строки осталось
    reg  [9:0]  r_done;                      // слов строки прочитано
    reg  [9:0]  r_chunk;                     // слов в текущем куске
    reg  [20:0] mul_acc;
    reg  [9:0]  mul_a;
    reg  [20:0] mul_b;
    reg  [3:0]  mul_i;

    wire        any_pend = |pend;
    integer     pk;
    always_comb begin
        pk = 0;
        for (int k = N - 1; k >= 0; k--) if (pend[k]) pk = k;
    end
    // кусок: до 64 слов, в одной строке SDRAM (256 слов), кратно пакету
    function automatic logic [9:0] chunk_of(input logic [20:0] a, input logic [9:0] left);
        logic [8:0] to_row;
        to_row = 9'd256 - {1'b0, a[7:0]};
        chunk_of = left;
        if (chunk_of > 10'd64) chunk_of = 10'd64;
        if (chunk_of > {1'b0, to_row}) chunk_of = {1'b0, to_row};
    endfunction

    always_ff @(posedge mclk or negedge mrst_n) begin
        if (!mrst_n) begin
            mst <= M_IDLE; pend <= '0; r_k <= '0; r_half <= 1'b0; r_addr <= '0; r_left <= '0;
            r_done <= '0; r_chunk <= '0; mul_acc <= '0; mul_a <= '0; mul_b <= '0; mul_i <= '0;
            s_req <= 1'b0; s_we <= 1'b0; s_addr <= '0; s_len <= '0;
            lb_we <= '0; lb_wa <= '0; lb_wd <= '0;
            g_mask <= '0; g_blk <= '0; g_idle <= '0; wq_rd <= 1'b0; wq_rd_q <= 1'b0;
            h_v <= 1'b0; h_q <= '0;
        end else begin
            lb_we <= '0;
            wq_rd <= 1'b0;
            for (int k = 0; k < N; k++)
                if (rq_s[k][2] ^ rq_s[k][1]) pend[k] <= 1'b1;

            // слово очереди — в h_q (см. выше); следующее чтение — когда h_q ушло в группу
            wq_rd_q <= wq_rd;
            if (wq_rd_q) begin
                h_q <= wq_q;
                h_v <= 1'b1;
            end else if (!h_v && !wq_rd && !wq_empty) begin
                wq_rd <= 1'b1;
            end
            // сборка группы записи (пока группа не уходит в SDRAM)
            if (mst != M_WREQ && mst != M_WDATA) begin
                if (h_v) begin
                    // слово — в группу, если она пуста или того же пакета
                    if (g_mask == 8'd0 || h_blk == g_blk) begin
                        g_data[h_wi] <= h_q[31:0];
                        g_mask[h_wi] <= 1'b1;
                        g_blk        <= h_blk;
                        h_v          <= 1'b0;
                        g_idle       <= '0;
                    end
                end else if (g_mask != 8'd0 && g_idle != 7'h7F) begin
                    g_idle <= g_idle + 7'd1;
                end
            end

            case (mst)
            M_IDLE: begin
                if (any_pend) begin
                    // строка холста: начало = REGION + base + строка · шаг (умножение по разряду)
                    r_k     <= pk[$bits(r_k)-1:0];
                    pend[pk] <= 1'b0;
                    r_half  <= pend_row[pk][0];
                    r_left  <= f_words[pk];
                    r_done  <= '0;
                    mul_a   <= pend_row[pk];
                    mul_b   <= {11'd0, f_words[pk]};
                    mul_acc <= REGION + {4'd0, f_base[pk]};
                    mul_i   <= 4'd10;
                    mst     <= M_MUL;
                end else if (g_mask == 8'hFF || (g_mask != 8'd0 && (g_idle == 7'h7F ||
                             (h_v && h_blk != g_blk)))) begin
                    s_req  <= 1'b1;
                    s_we   <= 1'b1;
                    s_addr <= REGION + {4'd0, g_blk, 3'd0};
                    s_len  <= 3'd0;
                    mst    <= M_WREQ;
                end
            end
            M_MUL: begin
                if (mul_a[0]) mul_acc <= mul_acc + mul_b;
                mul_a <= mul_a >> 1;
                mul_b <= mul_b << 1;
                mul_i <= mul_i - 4'd1;
                if (mul_i == 4'd0) begin
                    r_addr <= mul_acc;
                    if (r_left == 10'd0) mst <= M_IDLE;
                    else                 mst <= M_RREQ;
                end
            end
            M_RREQ: begin
                if (!s_req) begin : rreq
                    logic [9:0] c;
                    c = chunk_of(r_addr, r_left);
                    r_chunk <= c;
                    s_req   <= 1'b1;
                    s_we    <= 1'b0;
                    s_addr  <= r_addr;
                    s_len   <= 3'((c >> 3) - 10'd1);
                end else if (s_ack) begin
                    s_req <= 1'b0;
                    mst   <= M_RDATA;
                end
            end
            M_RDATA: if (s_rd_valid) begin : rdata
                logic [9:0] wi;
                wi = r_done + {4'd0, s_rd_idx};
                lb_we[r_k] <= 1'b1;
                lb_wa      <= {r_half, wi[8:0]};
                lb_wd      <= s_rd_data;
                if ({4'd0, s_rd_idx} == r_chunk - 10'd1) begin
                    r_done <= r_done + r_chunk;
                    r_left <= r_left - r_chunk;
                    r_addr <= r_addr + {11'd0, r_chunk};
                    if (r_left == r_chunk) mst <= M_IDLE;
                    else                   mst <= M_RREQ;
                end
            end
            M_WREQ: if (s_ack) begin
                s_req <= 1'b0;
                mst   <= M_WDATA;
            end
            M_WDATA: if (s_wr_take && s_wr_idx[2:0] == 3'd7) begin
                g_mask <= '0;
                g_idle <= '0;
                mst    <= M_IDLE;
            end
            default: mst <= M_IDLE;
            endcase
        end
    end
endmodule
