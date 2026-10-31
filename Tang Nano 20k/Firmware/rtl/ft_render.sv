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
    parameter logic [20:0] T_HI         = 21'h0C9000,   //            строки 512…767 (образ ПЗУ)
    parameter bit          UP2          = 1'b0          // выгрузка плиток по два слова за такт (мост)
) (
    input  wire         clk,
    input  wire         rst_n,
    input  wire         clk_sd,         // UP2: 2 × clk, тот же PLL
    input  wire         enable,         // режим FT (M_SEL) и развёртка FT812 идёт
    input  wire [11:0]  hsize,          // REG_HSIZE, REG_VSIZE
    input  wire [11:0]  vsize,
    input  wire         ramg_3mb,
    input  wire         dither,

    // RAM_DL показываемого банка (ft_core)
    input wire dl_epoch,
    input wire texture_write, // запись хоста в RAM_G дошла до моста памяти
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
    output wire [31:0]  s_wdata2,       // UP2: второе слово пары (s_wr_take — сразу два слова)
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
    reg q_full;
    wire        it_full = UP2 ? q_full : q_cnt == 4'd8;   // обходчик пишет в этот же такт — места 8
    wire [2:0]  q_rn = abort ? 3'd0 : q_rp + {2'd0, q_pop};
    always_ff @(posedge clk) begin
        if (it_v) q_mem[q_wp] <= it_data;
        q_head <= (it_v && q_wp == q_rn) ? it_data[34:0] : q_mem[q_rn][34:0];
    end
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            q_wp <= '0; q_rp <= '0; q_cnt <= '0; q_full<=1'b0;
        end else if (abort) begin
            q_wp <= '0; q_rp <= '0; q_cnt <= '0; q_full<=1'b0;
        end else begin
            if (it_v) q_wp <= q_wp + 3'd1;
            if (q_pop) q_rp <= q_rp + 3'd1;
            q_cnt <= q_cnt + {3'd0, it_v} - {3'd0, q_pop};
            q_full <= !q_pop && (q_full || (q_cnt == 4'd7 && it_v));
        end
    end

    typedef enum logic [1:0] { R_IDLE, R_RUN, R_PUB } rstate_t;
    rstate_t rst_q;

    wire [10:0] wk_dl_addr;
    wire [31:0] wk_dl_q;
    wire snap_ready, seed_we;
    wire [4:0] seed_addr;
    wire [23:0] seed_data;
    wire [2:0] seed_field;
    wire snap_overrun;
    reg dl_epoch_q;
    wire logical_swap=dl_epoch!=dl_epoch_q;
    reg image_stale;
    // Одна отмена на картинку: иначе при кадре дольше периода смен и записи в RAM_G на каждый кадр
    // кадры отменялись бы подряд и экран стоял (стенд: 1 кадр за 0,15 с вместо 6). Следующая картинка
    // дорисовывается; текстуры в ней могут быть и новее — настоящий FT812 тоже рисует из живой RAM_G.
    reg abort_used;
    wire stale_write_abort=UP2 && !abort_used && texture_write && (image_stale || logical_swap);
    always @(posedge clk or negedge rst_n)
        if(!rst_n) begin dl_epoch_q<=0;image_stale<=0;end
        else begin
            dl_epoch_q<=dl_epoch;
            if(start) image_stale<=0;
            else if(logical_swap) image_stale<=1;
        end
    generate if(UP2) begin : g_snapshot
        ft_dl_snapshot snapshot (
            .clk(clk), .rst_n(rst_n), .epoch(dl_epoch),
            .request(rst_q == R_IDLE && !abort && !wk_busy), .take(start), .ready(snap_ready),
            .live_addr(dl_addr), .live_q(dl_q), .draw_addr(wk_dl_addr), .draw_q(wk_dl_q),
            .seed_we(seed_we), .seed_addr(seed_addr), .seed_data(seed_data), .seed_field(seed_field), .overrun(snap_overrun));
    end else begin : g_live_list
        assign dl_addr=wk_dl_addr; assign wk_dl_q=dl_q; assign snap_ready=1'b1;
        assign seed_field='0; assign seed_we=1'b0; assign seed_addr='0; assign seed_data='0; assign snap_overrun=1'b0;
    end endgenerate
    ft_rd_walk #(.TW_LOG(TW_LOG), .TH_LOG(TH_LOG), .SEED_HANDLES(UP2)) walk (
        .clk(clk), .rst_n(rst_n), .abort(abort), .start(start), .lines(f_lines), .hsize(f_hsize),
        .ready(wk_ready), .dl_addr(wk_dl_addr), .dl_q(wk_dl_q),
        .seed_we(seed_we), .seed_addr(seed_addr), .seed_data(seed_data), .seed_field(seed_field),
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

    // UP2 включён только у SLOT1 (FT812 с отдельной PLL SDRAM).
    ft_rd_exec #(.TW_LOG(TW_LOG), .TH_LOG(TH_LOG), .SHORT_DECODE(UP2), .EARLY_RESTART(UP2), .PAIR_EXT(UP2)) exec (
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
    ft_rd_pix #(.TW_LOG(TW_LOG), .TH_LOG(TH_LOG), .UP2(UP2)) pix (
        .clk(clk), .rst_n(rst_n), .clk_sd(clk_sd), .fb_base(f_base), .fb_base_hi(f_base_hi), .dither(dither),
        .ready(px_ready), .clear(px_clear),
        .f_v(f_v), .f_x(f_x), .f_rgba(f_rgba), .f_raw(f_raw), .f_slot(f_slot), .f_clr(f_clr), .f_xn(f_xn), .f_adv(f_adv),
        .f_p2(f_p2), .f_w2(f_w2), .f_rgba2(f_rgba2), .f_p2n(f_p2n), .f_ready(f_ready),
        .slot_we(slot_we), .slot_sel(slot_sel), .slot_data(slot_data), .slot_busy(slot_busy),
        .impl_we(impl_we), .impl_rgba(impl_rgba), .impl_st(impl_st), .cur_buf(cur_buf),
        .be_empty(be_empty),
        .tile_go(tile_go), .tile_buf(tile_buf), .tile_x(tile_x), .tile_y(tile_y), .buf_free(buf_free),
        .drain_idle(drain_idle),
        .w_req(w_req), .w_addr(w_addr), .w_len(w_len), .w_ack(w_ack),
        .w_take(s_wr_take), .w_data(s_wdata), .w_data2(s_wdata2)
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
            frames <= '0; f_timer <= '0; frame_time <= '0; abort_used <= 1'b0;
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
                else if (!abort && !need_clr && !px_clear && (UP2 || !swap_wait) && snap_ready && wk_ready && px_ready && drain_idle && be_empty && !wk_busy && !ex_busy &&
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
            R_RUN: if (stale_write_abort || (f_redo && swap_wait && !start)) begin
                // Копия листа у растеризатора своя и смене не мешает. Отмена — только если хост
                // после смены пишет в RAM_G: старый кадр не должен выйти с чужими текстурами.
                abort     <= 1'b1;
                swap_hold <= 1'b0;
                rst_q     <= R_IDLE;
                if (stale_write_abort) abort_used <= 1'b1;
            end else begin
                if (!start && !wk_busy)
                    swap_hold <= 1'b0;                // лист прочитан: DLSWAP можно
                if (!start && !wk_busy && !ex_busy && drain_idle) begin
                    pub_v      <= 1'b1;
                    abort_used <= 1'b0;
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


// Неизменяемая копия RAM_DL для растеризатора ядра FT812 (слот 1, с 1.44): хост получает смену
// листа по событию кадра и сразу пишет в освободившийся банк, а незаконченный кадр дорисовывается
// из копии. Младшие 32 бита блоков — копия листа; четыре запасных бита на слово хранят две таблицы
// ручек 32 × 80 бит (до и после текущей смены): после смены автомат проходит лист как обходчик
// (CALL, JUMP, RETURN, SAVE/RESTORE) и собирает значения ручек, потом ими заполняет таблицу обходчика
// (seed_*). Так ручки, заданные в пропущенном кадре, сохраняются. Проход ограничен 16384 командами.
module ft_dl_snapshot (
    input wire clk, rst_n, epoch,
    input wire request, take,
    output wire ready,
    output reg overrun,
    output wire [10:0] live_addr,
    input wire [31:0] live_q,
    input wire [10:0] draw_addr,
    output reg [31:0] draw_q,
    output reg seed_we,
    output reg [4:0] seed_addr,
    output reg [2:0] seed_field,
    output reg [23:0] seed_data
);
    localparam [4:0] INIT=0, IDLE=1, PRE_RD=2, PRE_GET=3, PRE_WR=4,
        FETCH=5, EXEC=6, FIELD_RD=7, FIELD_WR=8,
        CAP_RD=9, CAP_WR=10, SEED_RD=11, SEED_GET=12;
    reg [4:0] state;
    reg epoch_q, pending, valid, have_list, rendered;
    wire changed = epoch != epoch_q;
    assign ready = valid && !changed;
    reg [10:0] index, pc;
    reg [14:0] steps;
    reg [4:0] handle;
    reg [10:0] calls [0:3];
    reg [2:0] call_n, save_n;
    reg [1:0] save_sp;
    reg [4:0] saves [0:3];
    reg [3:0] nibble;
    reg [23:0] field_value;
    reg [2:0] field_left;
    reg finish_after_field;
    wire [4:0] seed_nibble=index[4:0];
    reg [19:0] seed_shift;
    function automatic logic [66:0] rom_font(input logic [4:0] hnd);   // {source, layout, size}
        logic [4:0] fmt; logic [9:0] st; logic [8:0] fw, fh; logic [21:0] src;
        case (hnd)
            5'd16: begin fmt = 5'd1; st = 10'd1;  fw = 9'd8;  fh = 9'd8;  src = 22'h2FF7FC; end
            5'd17: begin fmt = 5'd1; st = 10'd1;  fw = 9'd8;  fh = 9'd8;  src = 22'h2FFBFC; end
            5'd18: begin fmt = 5'd1; st = 10'd1;  fw = 9'd8;  fh = 9'd16; src = 22'h2FE7FC; end
            5'd19: begin fmt = 5'd1; st = 10'd1;  fw = 9'd8;  fh = 9'd16; src = 22'h2FEFFC; end
            5'd20: begin fmt = 5'd1; st = 10'd2;  fw = 9'd10; fh = 9'd13; src = 22'h2FDAFC; end
            5'd21: begin fmt = 5'd1; st = 10'd2;  fw = 9'd13; fh = 9'd17; src = 22'h2FCD3C; end
            5'd22: begin fmt = 5'd1; st = 10'd2;  fw = 9'd14; fh = 9'd20; src = 22'h2FBD7C; end
            5'd23: begin fmt = 5'd1; st = 10'd3;  fw = 9'd17; fh = 9'd22; src = 22'h2FA17C; end
            5'd24: begin fmt = 5'd1; st = 10'd3;  fw = 9'd24; fh = 9'd29; src = 22'h2F7E3C; end
            5'd25: begin fmt = 5'd1; st = 10'd4;  fw = 9'd30; fh = 9'd38; src = 22'h2F3D1C; end
            5'd26: begin fmt = 5'd2; st = 10'd7;  fw = 9'd14; fh = 9'd16; src = 22'h2F181C; end
            5'd27: begin fmt = 5'd2; st = 10'd8;  fw = 9'd16; fh = 9'd20; src = 22'h2ED61C; end
            5'd28: begin fmt = 5'd2; st = 10'd9;  fw = 9'd18; fh = 9'd25; src = 22'h2E799C; end
            5'd29: begin fmt = 5'd2; st = 10'd11; fw = 9'd22; fh = 9'd28; src = 22'h2DFBBC; end
            5'd30: begin fmt = 5'd2; st = 10'd14; fw = 9'd28; fh = 9'd36; src = 22'h2D263C; end
            5'd31: begin fmt = 5'd2; st = 10'd18; fw = 9'd36; fh = 9'd49; src = 22'h2BAC3C; end
            default: begin fmt = '0; st = '0; fw = '0; fh = '0; src = '0; end
        endcase
        rom_font = {src, fmt, st, fh, 3'b000, fw, fh};
    endfunction



    wire [66:0] init_font=rom_font(index[9:5]);
    reg [3:0] init_nibble;
    always @* begin
        init_nibble=0;
        case(index[4:0])
            5'd0:init_nibble=init_font[48:45];
            5'd1:init_nibble=init_font[52:49];
            5'd2:init_nibble=init_font[56:53];
            5'd3:init_nibble=init_font[60:57];
            5'd4:init_nibble=init_font[64:61];
            5'd5:init_nibble={2'd0,init_font[66:65]};
            5'd6:init_nibble=init_font[24:21];
            5'd7:init_nibble=init_font[28:25];
            5'd8:init_nibble=init_font[32:29];
            5'd9:init_nibble=init_font[36:33];
            5'd10:init_nibble=init_font[40:37];
            5'd11:init_nibble=init_font[44:41];
            5'd12:init_nibble=init_font[3:0];
            5'd13:init_nibble=init_font[7:4];
            5'd14:init_nibble=init_font[11:8];
            5'd15:init_nibble=init_font[15:12];
            5'd16:init_nibble=init_font[19:16];
            5'd17:init_nibble={3'd0,init_font[20:20]};
            default:;
        endcase
    end
    wire in_pass = state >= PRE_RD && state <= FIELD_WR;
    wire [7:0] op = live_q[31:24];
    wire vertex = live_q[31:30] == 2'b01 || live_q[31:30] == 2'b10;
    wire terminal = steps == 15'h4000 || (!vertex &&
        (op == 8'h00 || (op == 8'h1D && call_n == 4) || (op == 8'h24 && call_n == 0)));
    wire field_op = !vertex && (op == 8'h01 || op == 8'h07 || op == 8'h08 || op == 8'h28 || op == 8'h29);
    wire [10:0] handle_base = {1'b0,handle,5'd0};

    reg [35:0] mem [0:2047];
    reg [35:0] mq;
    reg [10:0] ma;
    reg mw;
    reg [35:0] md;
    assign live_addr = (state == CAP_RD || state == CAP_WR) ? index : pc;
    always @* begin
        ma=index; mw=0; md=0;
        case(state)
            INIT: begin mw=1; md={init_nibble,32'd0}; end
            PRE_RD: ma={1'b1,index[9:0]};
            PRE_GET: ma=index;
            PRE_WR: begin mw=1; md={nibble,mq[31:0]}; end
            FIELD_RD: ma=index;
            FIELD_WR: begin mw=1; md={field_value[3:0],mq[31:0]}; end
            CAP_RD: ma=index;
            CAP_WR: begin mw=1; md={mq[35:32],live_q}; end
            SEED_RD,SEED_GET: ma={rendered,index[9:0]};
            default: ;
        endcase
    end
    always @(posedge clk) begin
        if(!mw) mq <= mem[ma];
        if(mw) mem[ma] <= md;
        // Чтение того же адреса при записи определено явно: запись таблиц ручек сохраняет младшее
        // слово, а копия листа пишется только при стоящем обходчике.
        draw_q <= mw && ma == draw_addr ? md[31:0] : mem[draw_addr][31:0];
    end
    always @(posedge clk or negedge rst_n) begin
        if(!rst_n) begin
            state<=INIT; index<=0; epoch_q<=0; pending<=0; valid<=0; have_list<=0;rendered<=0;
            overrun<=0; seed_we<=0;seed_field<=0; seed_addr<=0; seed_data<=0; seed_shift<=0;
            pc<=0; steps<=0; handle<=0; call_n<=0; save_n<=0; save_sp<=0;
            nibble<=0; field_value<=0; field_left<=0; finish_after_field<=0;
        end else begin
            seed_we<=0;
            epoch_q<=epoch;
            if(take) begin valid<=0;rendered<=1;end
            case(state)
                INIT: begin
                    if(index==2047) begin state<=IDLE;index<=0;end else index<=index+1'b1;
                end
                IDLE: if(pending) begin
                    pending<=0; valid<=0; index<=0; state<=PRE_RD;
                end else if(request && !valid && have_list && !take) begin
                    index<=0; state<=CAP_RD;
                end
                PRE_RD: state<=PRE_GET;
                PRE_GET: begin nibble<=mq[35:32];state<=PRE_WR;end
                PRE_WR: if(index==1023) begin
                    pc<=0;steps<=0;handle<=0;call_n<=0;save_n<=0;save_sp<=0;state<=FETCH;
                end else begin index<=index+1'b1;state<=PRE_RD;end
                FETCH: state<=EXEC;
                EXEC: if(terminal) begin state<=IDLE;have_list<=1;end else begin
                    steps<=steps+1'b1;
                    pc<=pc+1'b1;
                    state<=pc==2047 ? IDLE : FETCH;
                    if(pc==2047) have_list<=1;
                    if(!vertex) case(op)
                        8'h05: handle<=live_q[4:0];
                        8'h1D: begin
                            calls[call_n[1:0]]<=pc+1'b1;call_n<=call_n+1'b1;
                            pc<=live_q[10:0];state<=FETCH;
                        end
                        8'h1E: begin pc<=live_q[10:0];state<=FETCH;end
                        8'h24: begin pc<=calls[call_n[1:0]-2'd1];call_n<=call_n-1'b1;state<=FETCH;end
                        8'h22: begin
                            saves[save_sp]<=handle;save_sp<=save_sp+1'b1;
                            save_n<=save_n==4 ? 3'd4 : save_n+1'b1;
                        end
                        8'h23: if(save_n!=0) begin
                            handle<=saves[save_sp-2'd1];save_sp<=save_sp-1'b1;save_n<=save_n-1'b1;
                        end else handle<=0;
                        default: ;
                    endcase
                    if(field_op) begin
                        state<=FIELD_RD;finish_after_field<=pc==2047;
                        field_value<=live_q[23:0];field_left<=6;
                        case(op)
                            8'h01: begin index<={1'b1,handle,5'd0};field_value<={2'b0,live_q[21:0]};end
                            8'h07: index<={1'b1,handle,5'd6};
                            8'h08: begin index<={1'b1,handle,5'd12};field_value<={3'b0,live_q[20:0]};end
                            8'h28: begin index<={1'b1,handle,5'd18};field_left<=1;end
                            default: begin index<={1'b1,handle,5'd19};field_left<=1;end
                        endcase
                    end
                end
                FIELD_RD: state<=FIELD_WR;
                FIELD_WR: if(field_left==1) begin
                    state<=finish_after_field ? IDLE : FETCH;
                    if(finish_after_field) have_list<=1;
                end else begin
                    index<=index+1'b1;field_left<=field_left-1'b1;
                    field_value<=field_value>>4;state<=FIELD_RD;
                end
                CAP_RD: state<=CAP_WR;
                CAP_WR: if(index==2047) begin
                    index<=0;seed_addr<=0;seed_field<=0;state<=SEED_RD;
                end else begin index<=index+1'b1;state<=CAP_RD;end
                SEED_RD: state<=SEED_GET;
                SEED_GET: begin
                    seed_shift<={mq[35:32],seed_shift[19:4]};
                    index<=index+1'b1;state<=SEED_RD;
                    if(seed_nibble==5 || seed_nibble==11 || seed_nibble==17 || seed_nibble==18 || seed_nibble==19) begin
                        seed_we<=1;
                        if(seed_nibble==18 || seed_nibble==19) seed_data<={20'd0,mq[35:32]};
                        else seed_data<={mq[35:32],seed_shift};
                    end
                    if(seed_nibble==19) begin
                       
                        if(index==1011) begin valid<=1;state<=IDLE;end
                        else index<=index+11'd13;
                    end
                end
                default: state<=INIT;
            endcase
            if(seed_we) begin
                if(seed_field==4) begin seed_addr<=seed_addr+1'b1;seed_field<=0;end
                else seed_field<=seed_field+1'b1;
            end
            if(changed) begin
                pending<=1;valid<=0;rendered<=0;
                if(in_pass) overrun<=1;
                if(state!=INIT) begin state<=IDLE;seed_we<=0;end
            end
        end
    end
endmodule
