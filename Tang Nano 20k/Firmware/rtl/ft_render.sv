// SPDX-License-Identifier: Apache-2.0
//
// Растеризатор FT812 (шаг 4, docs/ft812-emulation.md): дисплей-лист показываемого банка
// RAM_DL → кадр FT812 в SDRAM (RGB565, строка — 512 слов), который fb_core выводит через
// масштаб 720p. Домен clk_mem.
//
// Как у FT812 на каждой строке, лист выполняется заново на каждой плитке 64×16 (ядро 1.19 и
// с 1.22; 1.20–1.21 — 128×8: кадр Zuma на 15 % дольше, стенд tb/vsys; до 1.19 — строками): не в
// такт развёртке, а в задний кадр: ft_rd_walk (обход листа)
// → очередь → ft_rd_exec (задания → точки) → ft_rd_pix (буфер плитки → SDRAM). Порядок
// примитивов в каждой точке — по листу, как у чипа; плитками тексели повёрнутых и
// масштабированных битмапов лежат кучно и держатся в кэше. Кадров FT три (при RAM_G 2 МБ):
// готовый кадр объявляется читателю (pub_*), а следующий сразу пишется в третий буфер — не
// тот, что выводится (fb_front), и не объявленный; читатель берёт объявленный в начале своего
// кадра, кадр, который он не успел взять, заменяется новым. Так растеризатор не ждёт вывод
// (с двумя буферами он стоял до начала кадра вывода — частота кадров FT шла ступенями 60, 30,
// 20…). Третий буфер — строки 0…511 в верхнем мегабайте RAM_G (при 2 МБ хост видит там ПЗУ),
// 512…767 — в свободной середине образа ПЗУ (шрифты — в его начале и конце). При RAM_G 3 МБ
// буферов два: следующий кадр — когда читатель взял объявленный. Лист перечитывается кадр
// за кадром, даже если он тот же: RAM_G (картинки) может меняться и без DLSWAP.
//
// Пока кадр читает лист, смена листа (DLSWAP) ждёт (swap_hold): хост пишет в другой банк
// и не испортит читаемый. Сложный лист снижает частоту кадров, но кадр не рвётся. Кадр-повтор
// (тот же лист, смены после начала прошлого кадра не было) смену не держит: хост её попросил —
// повтор бросается (с 1.22). Иначе игра, которой на кадр нужно больше, чем растеризатору, ждала
// весь повтор, и шагов игры было вдвое меньше (Zuma: 11 в секунду вместо 17, стенд tb/vsys).
module ft_render #(
    parameter int TW_LOG = 6,                           // плитка 64×16 (1024 точки)
    parameter int TH_LOG = 4,
    parameter logic [20:0] FB_BASE      = 21'h120000,   // адрес слова кадра FT 0
    parameter logic [20:0] FT_BUF_WORDS = 21'h060000,   // кадр: 768 строк × 512 слов
    parameter logic [20:0] T_LO         = 21'h080000,   // кадр FT 2: строки 0…511 (RAM_G 2…3 МБ)
    parameter logic [20:0] T_HI         = 21'h0C9000    //            строки 512…767 (образ ПЗУ)
) (
    input  wire         clk,
    input  wire         rst_n,
    input  wire         enable,         // режим FT (M_SEL) и развёртка FT812 идёт
    input  wire [11:0]  hsize,          // REG_HSIZE, REG_VSIZE
    input  wire [11:0]  vsize,
    input  wire         ramg_3mb,
    input  wire         dither,

    // RAM_DL показываемого банка (ft_core)
    output wire [10:0]  dl_addr,
    input  wire [31:0]  dl_q,
    output reg          swap_hold,
    input  wire         swap_wait,      // смена листа запрошена и ещё не прошла (REG_DLSWAP ≠ 0)

    // SDRAM: клиент sdram_burst (метка — снаружи: sub 0 — текстуры, 1 — запись строки)
    output wire         s_req,
    output wire         s_we,
    output wire [20:0]  s_addr,
    output wire [2:0]   s_len,
    output wire         s_sub,
    input  wire         s_ack,          // уже только для этого клиента
    input  wire         s_op_sub,       // sub операции, которой s_ack
    input  wire         s_wr_take,      // слово записи плитки (sub 1) — снаружи уже отобрано

    output wire [31:0]  s_wdata,
    input  wire         s_rd_valid,     // слово чтения текселей (sub 0) — тоже
    input  wire [2:0]   s_rd_idx,
    input  wire [31:0]  s_rd_data,

    // кадры для читателя (fb_core)
    input  wire         fb_showing,
    input  wire [1:0]   fb_front,
    output reg          pub_v,
    output reg  [1:0]   pub_buf,
    output reg  [9:0]   pub_words,      // слов в строке
    output reg  [10:0]  pub_w,          // размер кадра
    output reg  [9:0]   pub_h,

    output wire [31:0]  status,
    output reg  [15:0]  frame_time      // такты кадра / 1024
);
    // --- обходчик и очередь -------------------------------------------------------------------
    reg         abort;
    reg         start;
    reg  [9:0]  f_lines;
    reg  [10:0] f_hsize;
    reg  [1:0]  w_buf;                  // буфер кадра, который пишется
    reg         f_tri;                  // кадр начат с тремя буферами (RAM_G 2 МБ)
    // база половины кадра b: строки 0…511 (hi = 0) или 512…767 (hi = 1, база строки 512)
    function automatic logic [20:0] lbase(input logic [1:0] b, input logic hi);
        case (b)
            2'd0:    lbase = FB_BASE + (hi ? 21'h040000 : 21'd0);
            2'd1:    lbase = FB_BASE + FT_BUF_WORDS + (hi ? 21'h040000 : 21'd0);
            default: lbase = hi ? T_HI : T_LO;
        endcase
    endfunction
    // буфер не из {a, b} (a ≠ b)
    function automatic logic [1:0] third(input logic [1:0] a, input logic [1:0] b);
        third = (a != 2'd0 && b != 2'd0) ? 2'd0 : ((a != 2'd1 && b != 2'd1) ? 2'd1 : 2'd2);
    endfunction
    wire        wk_ready, wk_busy;
    wire [3:0]  wk_unsup;
    wire        it_v;
    wire [135:0] it_data;

    // вид и слово головы очереди — регистр (исполнитель разбирает их в том же такте, что и
    // берёт задание): по адресу чтения следующего такта, с обходом, если оно пишется сейчас;
    // остальные поля задания — из памяти очереди
    reg  [135:0] q_mem [0:7];
    reg  [34:0] q_head;
    reg  [2:0]  q_wp, q_rp;
    reg  [3:0]  q_cnt;
    wire        q_pop;
    wire        it_full = q_cnt == 4'd8;   // обходчик пишет в этот же такт — места 8
    wire [2:0]  q_rn = abort ? 3'd0 : q_rp + {2'd0, q_pop};
    always_ff @(posedge clk) begin
        if (it_v) q_mem[q_wp] <= it_data;
        q_head <= (it_v && q_wp == q_rn) ? it_data[34:0] : q_mem[q_rn][34:0];
    end
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            q_wp <= '0; q_rp <= '0; q_cnt <= '0;
        end else if (abort) begin
            q_wp <= '0; q_rp <= '0; q_cnt <= '0;
        end else begin
            if (it_v) q_wp <= q_wp + 3'd1;
            if (q_pop) q_rp <= q_rp + 3'd1;
            q_cnt <= q_cnt + {3'd0, it_v} - {3'd0, q_pop};
        end
    end

    ft_rd_walk #(.TW_LOG(TW_LOG), .TH_LOG(TH_LOG)) walk (
        .clk(clk), .rst_n(rst_n), .abort(abort), .start(start), .lines(f_lines), .hsize(f_hsize),
        .ready(wk_ready), .dl_addr(dl_addr), .dl_q(dl_q),
        .it_v(it_v), .it_data(it_data), .it_full(it_full), .busy(wk_busy), .unsup(wk_unsup)
    );

    // --- исполнитель и точки -------------------------------------------------------------------
    wire        tx_req, tx_ack;
    wire [20:0] tx_addr;
    wire        f_v, f_slot, f_clr, f_ready, f_adv, f_p2, f_w2, f_p2n;
    wire [31:0] f_rgba2;
    wire [9:0]  f_x, f_xn;
    wire [63:0] f_rgba;
    wire        f_raw;
    wire        slot_we, slot_sel, impl_we, cur_buf, tile_go, tile_buf, be_empty;
    wire [63:0] slot_data;
    wire [1:0]  slot_busy, buf_free;
    wire [31:0] impl_rgba;
    wire [7:0]  impl_st;
    wire [3:0]  tile_x;
    wire [9:0]  tile_y;
    wire        ex_busy;
    wire [3:0]  ex_unsup;

    ft_rd_exec #(.TW_LOG(TW_LOG), .TH_LOG(TH_LOG)) exec (
        .clk(clk), .rst_n(rst_n), .abort(abort), .start(start), .lines(f_lines),
        .hsize(f_hsize), .ramg_3mb(ramg_3mb),
        .q_v(q_cnt != 4'd0), .q_data(q_mem[q_rp]), .q_head(q_head), .q_pop(q_pop),
        .tx_req(tx_req), .tx_addr(tx_addr), .tx_ack(tx_ack),
        .tx_rd_valid(s_rd_valid), .tx_rd_idx(s_rd_idx), .tx_rd_data(s_rd_data),
        .f_v(f_v), .f_x(f_x), .f_rgba(f_rgba), .f_raw(f_raw), .f_slot(f_slot), .f_clr(f_clr), .f_xn(f_xn), .f_adv(f_adv),
        .f_p2(f_p2), .f_w2(f_w2), .f_rgba2(f_rgba2), .f_p2n(f_p2n), .f_ready(f_ready),
        .slot_we(slot_we), .slot_sel(slot_sel), .slot_data(slot_data), .slot_busy(slot_busy),
        .impl_we(impl_we), .impl_rgba(impl_rgba), .impl_st(impl_st), .cur_buf(cur_buf),
        .tile_go(tile_go), .tile_buf(tile_buf), .tile_x(tile_x), .tile_y(tile_y), .buf_free(buf_free),
        .be_empty(be_empty), .busy(ex_busy), .unsup(ex_unsup)
    );

    wire        px_ready, drain_idle;
    // кадр брошен (повтор, выключение): в буфере плитки — точки недорисованной плитки; до
    // следующего кадра буферы размечаются заново (ft_rd_pix clear)
    reg         need_clr, px_clear;
    wire        w_req, w_ack;
    wire [20:0] w_addr;
    wire [2:0]  w_len;
    reg  [20:0] f_base, f_base_hi;
    ft_rd_pix #(.TW_LOG(TW_LOG), .TH_LOG(TH_LOG)) pix (
        .clk(clk), .rst_n(rst_n), .fb_base(f_base), .fb_base_hi(f_base_hi), .dither(dither),
        .ready(px_ready), .clear(px_clear),
        .f_v(f_v), .f_x(f_x), .f_rgba(f_rgba), .f_raw(f_raw), .f_slot(f_slot), .f_clr(f_clr), .f_xn(f_xn), .f_adv(f_adv),
        .f_p2(f_p2), .f_w2(f_w2), .f_rgba2(f_rgba2), .f_p2n(f_p2n), .f_ready(f_ready),
        .slot_we(slot_we), .slot_sel(slot_sel), .slot_data(slot_data), .slot_busy(slot_busy),
        .impl_we(impl_we), .impl_rgba(impl_rgba), .impl_st(impl_st), .cur_buf(cur_buf),
        .be_empty(be_empty),
        .tile_go(tile_go), .tile_buf(tile_buf), .tile_x(tile_x), .tile_y(tile_y), .buf_free(buf_free),
        .drain_idle(drain_idle),
        .w_req(w_req), .w_addr(w_addr), .w_len(w_len), .w_ack(w_ack),
        .w_take(s_wr_take), .w_data(s_wdata)
    );

    // --- SDRAM: текстуры важнее записи плитки (у записи — двойной буфер) ------------------------
    assign s_req  = tx_req || w_req;
    assign s_sub  = !tx_req;
    assign s_we   = !tx_req;
    assign s_addr = tx_req ? tx_addr : w_addr;
    assign s_len  = tx_req ? 3'd0 : w_len;
    assign tx_ack = s_ack && !s_op_sub;
    assign w_ack  = s_ack && s_op_sub;

    // --- кадр ------------------------------------------------------------------------------------
    typedef enum logic [1:0] { R_IDLE, R_RUN, R_PUB } rstate_t;
    rstate_t rst_q;
    reg  [15:0] frames;
    reg  [25:0] f_timer;
    // повтор листа: кадр начат без смены листа после начала прошлого (RAM_G могла измениться —
    // поэтому кадры идут и без смен); смена прошла — REG_DLSWAP стал 0 (swap_wait снят)
    reg         fresh, f_redo, swap_wait_q;
    wire        swap_done = swap_wait_q && !swap_wait;
    wire [10:0] hs_c = hsize > 12'd1024 ? 11'd1024 : hsize[10:0];
    wire [9:0]  vs_c = vsize > 12'd768 ? 10'd768 : vsize[9:0];

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rst_q <= R_IDLE; abort <= 1'b1; start <= 1'b0; f_lines <= 10'd1; f_hsize <= 11'd1;
            w_buf <= 2'd0; f_tri <= 1'b0; f_base <= FB_BASE; f_base_hi <= FB_BASE + 21'h040000;
            swap_hold <= 1'b0;
            pub_v <= 1'b0; pub_buf <= 2'd0; pub_words <= 10'd512; pub_w <= 11'd1024; pub_h <= 10'd768;
            frames <= '0; f_timer <= '0; frame_time <= '0;
            fresh <= 1'b1; f_redo <= 1'b0; swap_wait_q <= 1'b0; need_clr <= 1'b0; px_clear <= 1'b0;
        end else begin
            start <= 1'b0;
            abort <= !enable;
            swap_wait_q <= swap_wait;
            px_clear <= 1'b0;
            if (abort) need_clr <= 1'b1;
            if (swap_done) fresh <= 1'b1;
            if (!f_timer[25]) f_timer <= f_timer + 26'd1;
            if (!enable) begin
                rst_q     <= R_IDLE;
                swap_hold <= 1'b0;
                pub_v     <= 1'b0;
            end else case (rst_q)
            R_IDLE: begin
                // три буфера: в такт объявления читатель мог взять прежний объявленный — и он
                // совпал с выбранным; тогда буфер — снова не выводимый и не объявленный
                if (!ramg_3mb && pub_v && fb_showing && fb_front == w_buf)
                    w_buf <= third(fb_front, pub_buf);
                // RAM_G 3 МБ: буфер 2 — это RAM_G, кадр — в 0 или 1 (не выводимый)
                else if (ramg_3mb && w_buf == 2'd2)
                    w_buf <= fb_showing && fb_front == 2'd0 ? 2'd1 : 2'd0;
                // буфер не выводится, прошлый кадр выгружен, обе части после сброса готовы; смена
                // листа не ждёт — иначе сначала она (на строке события 59 Гц или сразу, если та
                // прошла): кадр начатый со старым листом держал бы её до своего конца, а с тремя
                // буферами растеризатор начинает кадр сразу
                // после броска: точки дошли, выгрузки нет — разметить буферы плитки заново
                else if (need_clr && !abort && px_ready && drain_idle && be_empty && !ex_busy && !px_clear) begin
                    px_clear <= 1'b1;
                    need_clr <= 1'b0;
                end
                else if (!abort && !need_clr && !px_clear && !swap_wait && wk_ready && px_ready && drain_idle && be_empty && !wk_busy && !ex_busy &&
                    !(fb_showing && fb_front == w_buf) && hs_c != 11'd0 && vs_c != 10'd0) begin
                    f_hsize   <= hs_c;
                    f_lines   <= vs_c;
                    f_base    <= lbase(w_buf, 1'b0);
                    f_base_hi <= lbase(w_buf, 1'b1);
                    f_tri     <= !ramg_3mb;
                    start     <= 1'b1;
                    swap_hold <= 1'b1;
                    f_redo    <= !(fresh || swap_done);
                    fresh     <= 1'b0;
                    f_timer   <= '0;
                    rst_q     <= R_RUN;
                end
            end
            // повтор, а хост просит смену листа: кадр не нужен — его буфер не выводится и не
            // объявлен; бросить (как при выключении), смена проходит, следующий кадр — с новым листом
            R_RUN: if (f_redo && swap_wait && !start) begin
                abort     <= 1'b1;
                swap_hold <= 1'b0;
                rst_q     <= R_IDLE;
            end else begin
                if (!start && !wk_busy)
                    swap_hold <= 1'b0;                // лист прочитан: DLSWAP можно
                if (!start && !wk_busy && !ex_busy && drain_idle) begin
                    pub_v      <= 1'b1;
                    pub_buf    <= w_buf;
                    pub_words  <= 10'((f_hsize + 11'd1) >> 1);
                    pub_w      <= f_hsize;
                    pub_h      <= f_lines;
                    frames     <= frames + 16'd1;
                    frame_time <= f_timer[25:10];
                    if (f_tri && !ramg_3mb) begin
                        // три буфера: следующий — сразу, в буфер не выводимый и не объявленный
                        w_buf <= third(fb_showing ? fb_front : (w_buf == 2'd0 ? 2'd1 : 2'd0), w_buf);
                        rst_q <= R_IDLE;
                    end else begin
                        rst_q <= R_PUB;
                    end
                end
            end
            R_PUB: if (fb_showing && fb_front == w_buf) begin
                // два буфера: читатель взял кадр — следующий в другой (буфер 2 — только с тремя)
                w_buf <= w_buf == 2'd0 ? 2'd1 : 2'd0;
                rst_q <= R_IDLE;
            end
            default: rst_q <= R_IDLE;
            endcase
        end
    end

    assign status = {1'b0, w_buf[0], pub_v, enable, f_tri, w_buf[1], rst_q, ex_unsup, wk_unsup, frames};
endmodule
