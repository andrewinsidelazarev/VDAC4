`timescale 1ns/1ps
//
// Растеризатор FT812 (ft_render) на кадре заставки Weather 2: дисплей-лист и образ RAM_G —
// те же, что рисует bt8xxemu (scripts/ft_golden.py --weather2). SDRAM — контроллер
// sdram_burst и модель с проверкой протокола; RAM_DL — массив с чтением через такт.
//
// Каждая готовая плитка (tile_go, 128×8 или по параметрам ft_render) берётся из буфера плитки в кадр тестбенча: точки
// RGBA ({A, R, G, B}, «не касались» — значение плитки по умолчанию), до дизеринга и RGB565;
// в конце кадр — в ft_lines.hex (строка: номер и 1024 точки). Сверка с моделью —
// scripts/ft_rtl_check.py. Кадр RGB565 из SDRAM — в ft_frame.hex.
//
//   +dl=weather2_dl.hex +ramg=weather2_ramg.hex [+lines=N]
//
// FRAMES > 1 — тройной буфер кадра FT (ft_render): FRAMES кадров подряд, читатель берёт
// объявленный раз в RD_PERIOD тактов (кадр вывода). Проверки: ни одна запись кадра не попадает
// в выводимый буфер; растеризатор не ждёт читателя (кадров за время — по времени кадра);
// лист тот же — буферы 0, 1, 2 совпадают слово в слово во всех строках (буфер 2 — двумя
// кусками: строки 0…511 и 512…767).
//
// SDF = 1 — как в ядре FT812 (ts_fb_core SD_FAST, с 1.40): мост sd_bridge (метки 100/101, выгрузка —
// по два слова за такт), контроллер sdram_train на clk_sd = 2 × clk (60,75 / 121,5 МГц, CL 3,
// подбор сдвига клока микросхемы, ранняя следующая операция), модель — на clk_sd со сдвигом
// подбора. Кадр — тот же, что при SDF = 0 (слово в слово); нарушения протокола модели после
// подбора — ошибка.
module tb_ft_render;
    parameter int N_LINES = 768;
    parameter int TW_LOG = 6, TH_LOG = 4;      // плитка ft_render
    parameter int FRAMES = 1;
    parameter int RD_PERIOD = 1_031_250;      // кадр вывода 720p60 на 61,875 МГц
    parameter bit SDF = 1'b0;
    // SDF: TCK/2 и TCK/4 — целые пикосекунды (иначе фаза clk_sd к clk плывёт; у PLL она жёсткая)
    localparam real TCK = SDF ? 16.46 : 16.16;    // 60,75 МГц (SDF) или 61,875
    reg clk = 1'b0, rst_n = 1'b0;
    always #(TCK / 2) clk = ~clk;
    // clk_sd: фронт — с каждым фронтом clk и посередине (как CLKOUT/CLKOUTD fb_pll_sd)
    reg clk_sd = 1'b1;
    initial if (SDF) forever #(TCK / 4) clk_sd = ~clk_sd;

    // RAM_DL
    reg  [31:0] dl [0:2047];
    wire [10:0] dl_addr;
    reg  [31:0] dl_q;
    always @(posedge clk) dl_q <= dl[dl_addr];

    // SDRAM
    wire        s_req, s_we, s_sub, sd_ack, wr_take, rd_valid, init_done;
    wire [20:0] s_addr;
    wire [2:0]  s_len;
    wire [31:0] s_wdata, s_wdata2, rd_data;
    wire [5:0]  wr_idx, rd_idx;
    wire [0:0]  op_tag;
    wire        r_ack, r_op_sub, r_wr_take, r_rd_valid;   // порт растеризатора
    wire [3:0]  sd_phase;
    wire        sd_cs_n, sd_ras_n, sd_cas_n, sd_we_n, sd_dq_oe;
    wire [10:0] sd_a;
    wire [1:0]  sd_ba;
    wire [3:0]  sd_dqm;
    wire [31:0] sd_dq_out, sd_dq_in;

    reg         enable = 1'b0;
    reg         showing = 1'b0;
    reg  [1:0]  front = 2'd0;
    wire        pub_v, swap_hold;
    wire [1:0]  pub_buf;
    wire [9:0]  pub_words, pub_h;
    wire [10:0] pub_w;
    wire [31:0] status;
    wire [15:0] frame_time;

    ft_render #(.TW_LOG(TW_LOG), .TH_LOG(TH_LOG), .UP2(SDF)) dut (
        .clk(clk), .rst_n(rst_n), .clk_sd(clk_sd), .enable(enable), .hsize(12'd1024), .vsize(12'(N_LINES)),
        .ramg_3mb(1'b0), .dither(1'b1),
        .dl_addr(dl_addr), .dl_q(dl_q), .swap_hold(swap_hold), .swap_wait(1'b0),
        .s_req(s_req), .s_we(s_we), .s_addr(s_addr), .s_len(s_len), .s_sub(s_sub),
        .s_ack(r_ack), .s_op_sub(r_op_sub), .s_wr_take(r_wr_take), .s_wdata(s_wdata), .s_wdata2(s_wdata2),
        .s_rd_valid(r_rd_valid), .s_rd_idx(rd_idx[2:0]), .s_rd_data(rd_data),
        .fb_showing(showing), .fb_front(front),
        .pub_v(pub_v), .pub_buf(pub_buf), .pub_words(pub_words), .pub_w(pub_w), .pub_h(pub_h),
        .status(status), .frame_time(frame_time)
    );

    generate if (SDF) begin : g_sdf
        wire [2:0]  ack_tag, wr_tag, rd_tag;
        wire        c_req, c_we, c_ack, c_wr_take, c_rd_valid, c_init, c_tag, c_rd_tag;
        wire [20:0] c_addr;
        wire [2:0]  c_len;
        wire [31:0] c_wdata, c_rd_data;
        wire [3:0]  c_wmask;
        sd_bridge #(.TAGW(3), .W2(1'b1)) bridge (
            .clk(clk), .rst_n(rst_n), .clk_sd(clk_sd), .rst_sd_n(rst_n),
            .req(s_req), .we(s_we), .addr(s_addr), .req_len(s_len), .req_tag({2'b10, s_sub}),
            .ack(sd_ack), .ack_tag(ack_tag), .wr_take(wr_take), .wr_idx(wr_idx), .wr_tag(wr_tag),
            .wdata(s_wdata), .wmask(4'hF), .wdata2(s_wdata2),
            .rd_valid(rd_valid), .rd_idx(rd_idx), .rd_tag(rd_tag), .rd_data(rd_data),
            .rd_phase(2'd1), .init_done(init_done),
            .c_req(c_req), .c_we(c_we), .c_addr(c_addr), .c_len(c_len), .c_ack(c_ack),
            .c_wr_take(c_wr_take), .c_wdata(c_wdata), .c_wmask(c_wmask),
            .c_rd_valid(c_rd_valid), .c_rd_data(c_rd_data), .c_rd_phase(), .c_init_done(c_init),
            .c_tag(c_tag), .c_rd_tag(c_rd_tag));
        sdram_train #(.TAGW(1), .CLK_KHZ(121_500), .CL(3), .T_RCD(2), .T_RP(2), .T_WR(2), .T_RFC(8),
                      .INIT_US(5)) ctrl (
            .clk(clk_sd), .rst_n(rst_n),
            .req(c_req), .we(c_we), .addr(c_addr), .req_len(c_len), .req_tag(c_tag),
            .ack(c_ack), .op_tag(), .wr_take(c_wr_take), .wr_idx(), .wdata(c_wdata), .wmask(c_wmask),
            .rd_valid(c_rd_valid), .rd_idx(), .rd_data(c_rd_data), .rd_tag(c_rd_tag), .init_done(c_init),
            .phase(sd_phase), .train_st(), .train_win(),
            .sdram_cs_n(sd_cs_n), .sdram_ras_n(sd_ras_n), .sdram_cas_n(sd_cas_n),
            .sdram_we_n(sd_we_n), .sdram_a(sd_a), .sdram_ba(sd_ba), .sdram_dqm(sd_dqm),
            .sdram_dq_out(sd_dq_out), .sdram_dq_oe(sd_dq_oe), .sdram_dq_in(sd_dq_in));
        assign op_tag     = ack_tag[0];
        assign r_ack      = sd_ack && ack_tag[2:1] == 2'd2;
        assign r_op_sub   = ack_tag[0];
        assign r_wr_take  = wr_take && wr_tag == 3'b101;
        assign r_rd_valid = rd_valid && rd_tag == 3'b100;
    end else begin : g_one
        sdram_burst #(.TAGW(1), .CLK_KHZ(61_875), .INIT_US(5)) ctrl (
            .clk(clk), .rst_n(rst_n), .rd_phase(2'd1),
            .req(s_req), .we(s_we), .addr(s_addr), .req_len(s_len), .req_tag(s_sub),
            .ack(sd_ack), .op_tag(op_tag),
            .wr_take(wr_take), .wr_idx(wr_idx), .wdata(s_wdata), .wmask(4'hF),
            .rd_valid(rd_valid), .rd_idx(rd_idx), .rd_data(rd_data), .init_done(init_done),
            .sdram_cs_n(sd_cs_n), .sdram_ras_n(sd_ras_n), .sdram_cas_n(sd_cas_n),
            .sdram_we_n(sd_we_n), .sdram_a(sd_a), .sdram_ba(sd_ba), .sdram_dqm(sd_dqm),
            .sdram_dq_out(sd_dq_out), .sdram_dq_oe(sd_dq_oe), .sdram_dq_in(sd_dq_in));
        assign sd_phase   = 4'd0;
        assign r_ack      = sd_ack;
        assign r_op_sub   = op_tag[0];
        assign r_wr_take  = wr_take && op_tag[0];
        assign r_rd_valid = rd_valid && !op_tag[0];
    end endgenerate
    // клок модели: SDF — clk_sd со сдвигом PLL, что выбрал подбор (как tb_ts_fb), иначе ~clk
    reg sd_clk_sh = 1'b0;
    always @(clk_sd) sd_clk_sh <= #(sd_phase * TCK / 32.0) clk_sd;
    integer err_train = 0;               // нарушений протокола модели к концу подбора (SDF)
    initial if (SDF) begin
        wait (rst_n && init_done);
        err_train = mem.errors;
        $display("подбор SDRAM: сдвиг %0d, нарушений протокола во время подбора: %0d", sd_phase, err_train);
    end
    sdram_model #(.TCK_NS(SDF ? TCK / 2 : TCK), .CL(SDF ? 3 : 2)) mem (
        .clk(SDF ? sd_clk_sh : ~clk), .cs_n(sd_cs_n), .ras_n(sd_ras_n), .cas_n(sd_cas_n), .we_n(sd_we_n),
        .a(sd_a), .ba(sd_ba), .dqm(sd_dqm), .dq_from_ctrl(sd_dq_out), .dq_oe(sd_dq_oe),
        .dq_to_ctrl(sd_dq_in));

    // плитки из буфера плитки — в кадр тестбенча
    integer fl, ff, i, tiles_done = 0, lines_done = 0;
    reg [32:0] e, o;
    reg [31:0] impl;
    reg [31:0] frame [0:N_LINES * 1024 - 1];
    localparam int HW = (1 << TW_LOG) / 2;       // слов в строке плитки
    always @(posedge clk) if (dut.tile_go) begin : take_tile
        int ty, tx;
        impl = dut.pix.impl_c[dut.tile_buf];
        ty   = dut.tile_y;
        tx   = (1 << TW_LOG) * dut.tile_x;
        for (i = 0; i < 512; i = i + 1)
            if (ty + i / HW < N_LINES) begin
                e = dut.tile_buf ? dut.pix.ce1[i] : dut.pix.ce0[i];
                o = dut.tile_buf ? dut.pix.co1[i] : dut.pix.co0[i];
                frame[(ty + i / HW) * 1024 + tx + 2 * (i % HW)]     = e[32] ? impl : e[31:0];
                frame[(ty + i / HW) * 1024 + tx + 2 * (i % HW) + 1] = o[32] ? impl : o[31:0];
            end
        tiles_done = tiles_done + 1;
        if (tx + 2 * HW >= 1024) begin
            lines_done = ty + 512 / HW > N_LINES ? N_LINES : ty + 512 / HW;
            if (lines_done % 64 == 0 || lines_done == N_LINES)
                $display("[%0t] строк %0d, кадр рисуется %0d мкс", $time, lines_done, $time / 1000_000);
        end
    end

`ifdef PROF
    // Профиль (-DPROF): такты кадра по видам — точки, промахи текселей и палитры, остановка
    // на корне RECTS, ожидание ft_rd_pix, прочее (подготовка, перенос, выборка заданий);
    // по заданиям — по {вид, формат, BITMAP_SOURCE} задания, чьи точки в конвейере.
    longint pr_all = 0, pr_pix = 0, pr_tex = 0, pr_pal = 0, pr_band = 0, pr_back = 0;
    longint pr_jobs = 0, pr_dem = 0, pr_sp = 0, pr_pf = 0, pr_plf = 0, pr_idle = 0;
    reg  [28:0] pj_key [0:63];
    longint     pj_cyc [0:63], pj_pix [0:63], pj_tex [0:63], pj_pal [0:63], pj_job [0:63];
    integer     pj_n = 0, pj_i;
    wire [28:0] pk = {dut.exec.j_kind, dut.exec.j_fmt, dut.exec.j_base};
    always @(posedge clk) if (enable && !pub_v) begin
        pr_all++;
        pj_i = -1;
        for (int j = 0; j < pj_n; j++) if (pj_key[j] == pk) pj_i = j;
        if (pj_i < 0 && pj_n < 64) begin
            pj_i = pj_n; pj_key[pj_n] = pk;
            pj_cyc[pj_n] = 0; pj_pix[pj_n] = 0; pj_tex[pj_n] = 0; pj_pal[pj_n] = 0; pj_job[pj_n] = 0;
            pj_n++;
        end
        if (pj_i >= 0) pj_cyc[pj_i]++;
        // перенос задания (у j_* — уже новое на следующем такте), заполнения кэша
        if (dut.exec.xs_q == dut.exec.X_XFER && !dut.exec.n_skip && !dut.exec.j_run &&
            dut.exec.front_empty && !dut.exec.slot_busy[dut.exec.slot_nx] &&
            !(dut.exec.n_kind == dut.exec.J_CLR && dut.exec.untouched)) begin
            pr_jobs++;
            if (pj_i >= 0) pj_job[pj_i]++;
        end
        if (dut.exec.f_go_dem && pr_dem < 400 && $test$plusargs("filllog"))
            $display("заполнение %0d: строка кэша %05h индекс %02h, P4 x %0d строка %0d, метка в P4 %03h, j_row %0d, плитка %0d,%0d",
                     pr_dem, dut.exec.p4_wa[20:3], dut.exec.p4_idx, dut.exec.p4_x, dut.exec.p4_r, dut.exec.p4_tag,
                     dut.exec.j_row, dut.exec.tx0, dut.exec.ty0);
        if (dut.exec.f_go_pal) pr_plf++;
        else if (dut.exec.f_go_dem) pr_dem++;
        else if (dut.exec.f_go_pf && dut.exec.sp_v) pr_sp++;
        else if (dut.exec.f_go_pf) pr_pf++;
        if (dut.exec.front_empty && !dut.exec.j_run) pr_idle++;
        if (dut.exec.f_v && dut.exec.f_ready) begin pr_pix++; if (pj_i >= 0) pj_pix[pj_i]++; end
        else if (dut.exec.q6_miss) begin pr_pal++; if (pj_i >= 0) pj_pal[pj_i]++; end
        else if (dut.exec.p4_miss) begin pr_tex++; if (pj_i >= 0) pj_tex[pj_i]++; end
        else if (dut.exec.p4_band && !dut.exec.sq_done) pr_band++;
        else if (dut.exec.p5_v && !dut.exec.f_ready) pr_back++;
    end
    task automatic print_prof();
        bit done [0:63];
        $display("ПРОФИЛЬ: тактов %0d; точки %0d, промахи текселей %0d, палитры %0d, корень %0d, ожидание пикселей %0d, прочее %0d",
                 pr_all, pr_pix, pr_tex, pr_pal, pr_band, pr_back, pr_all - pr_pix - pr_tex - pr_pal - pr_band - pr_back);
        $display("  заданий %0d; заполнений: по промаху %0d, предвыборка начала пролёта %0d, следующей строки %0d, палитры %0d; конвейер пуст %0d тактов",
                 pr_jobs, pr_dem, pr_sp, pr_pf, pr_plf, pr_idle);
        for (int j = 0; j < 64; j++) done[j] = 0;
        for (int n = 0; n < 20 && n < pj_n; n++) begin
            int best = -1;
            for (int j = 0; j < pj_n; j++) if (!done[j] && (best < 0 || pj_cyc[j] > pj_cyc[best])) best = j;
            done[best] = 1;
            $display("  вид %0d формат %0d адрес %06h: тактов %0d, заданий %0d, точек %0d, промахов текселей %0d, палитры %0d",
                     pj_key[best][28:27], pj_key[best][26:22], pj_key[best][21:0], pj_cyc[best], pj_job[best],
                     pj_pix[best], pj_tex[best], pj_pal[best]);
        end
    endtask
`endif

    // тройной буфер: база половины кадра b (как ft_render.lbase) и проверки
    function automatic logic [20:0] lb(input int b, input logic hi);
        case (b)
            0:       lb = 21'h120000 + (hi ? 21'h040000 : 21'd0);
            1:       lb = 21'h120000 + 21'h060000 + (hi ? 21'h040000 : 21'd0);
            default: lb = hi ? 21'h0C9000 : 21'h080000;
        endcase
    endfunction
    function automatic int buf_of(input logic [20:0] a);
        buf_of = -1;
        for (int b = 0; b < 3; b++) begin
            if (a >= lb(b, 1'b0) && a < lb(b, 1'b0) + 21'h040000) buf_of = b;
            if (a >= lb(b, 1'b1) && a < lb(b, 1'b1) + 21'h020000) buf_of = b;
        end
    endfunction
    reg     rd_on = 1'b0;
    integer rd_cnt = 0, tb_errors = 0, used0 = 0, used1 = 0, used2 = 0, takes = 0;
    always @(posedge clk) begin
        if (rd_on) begin
            rd_cnt <= rd_cnt + 1 >= RD_PERIOD ? 0 : rd_cnt + 1;
            if (rd_cnt == 0 && pub_v) begin showing <= 1'b1; front <= pub_buf; takes = takes + 1; end
        end
        if (dut.pix.w_req && dut.pix.w_ack) begin : wr_chk
            int b;
            b = buf_of(dut.pix.w_addr);
            if (b < 0) begin
                tb_errors = tb_errors + 1;
                if (tb_errors <= 5) $display("[%0t] запись кадра вне буферов: %06h", $time, dut.pix.w_addr);
            end else begin
                if (showing && b == front) begin
                    tb_errors = tb_errors + 1;
                    if (tb_errors <= 5) $display("[%0t] запись в выводимый буфер %0d: %06h", $time, b, dut.pix.w_addr);
                end
                if (b == 0) used0 = used0 + 1; else if (b == 1) used1 = used1 + 1; else used2 = used2 + 1;
            end
        end
    end

    // читатель: взял объявленный кадр
    reg [31:0] t0;
    initial begin
        string dlf, rgf;
        if (!$value$plusargs("dl=%s", dlf)) dlf = "weather2_dl.hex";
        if (!$value$plusargs("ramg=%s", rgf)) rgf = "weather2_ramg.hex";
        $readmemh(dlf, dl);
        $readmemh(rgf, mem.mem);
        fl = $fopen("ft_lines.hex", "w");
        #(TCK * 10) rst_n = 1'b1;
        wait (init_done);
        @(posedge clk) enable <= 1'b1;
        t0 = $time;
        wait (pub_v);
        $display("кадр объявлен: буфер %0d, %0dx%0d, слов в строке %0d, время %0d × 1024 тактов (%0d мкс)",
                 pub_buf, pub_w, pub_h, pub_words, frame_time, ($time - t0) / 1000);
        // SDF: записи плиток ещё могут быть в мосте — дождаться их в SDRAM
        if (SDF) repeat (4000) @(posedge clk);
        $display("состояние %08h, ошибок протокола SDRAM: %0d (после подбора: %0d)", status, mem.errors,
                 mem.errors - err_train);
        for (int yy = 0; yy < N_LINES; yy++) begin
            $fwrite(fl, "%0d", yy);
            for (int xx = 0; xx < 1024; xx++) $fwrite(fl, " %08h", frame[yy * 1024 + xx]);
            $fwrite(fl, "\n");
        end
`ifdef PROF
        print_prof();
`endif
        $fclose(fl);
        // кадр RGB565 из SDRAM
        ff = $fopen("ft_frame.hex", "w");
        for (i = 0; i < N_LINES * 512; i = i + 1)
            $fwrite(ff, "%08h\n", mem.mem[21'h120000 + i]);
        $fclose(ff);
        showing <= 1'b1;
        front   <= pub_buf;
        repeat (20) @(posedge clk);
        if (FRAMES > 1) begin : triple
            longint t1, n0;
            int diff;
            n0 = dut.frames;
            t1 = $time;
            rd_on = 1'b1;
            wait (dut.frames >= n0 + FRAMES - 1);
            wait (dut.drain_idle && dut.rst_q != dut.R_RUN);
            repeat (20) @(posedge clk);
            $display("тройной буфер: кадров %0d за %0d мкс (%0d мкс на кадр), читатель взял %0d; записей в буферы 0/1/2: %0d/%0d/%0d",
                     dut.frames - n0, ($time - t1) / 1000, ($time - t1) / 1000 / (dut.frames - n0), takes, used0, used1, used2);
            diff = 0;
            for (int b = 1; b < 3; b++)
                for (int y = 0; y < N_LINES; y++)
                    for (int w = 0; w < 512; w++)
                        if (mem.mem[lb(b, y >= 512) + {y[8:0], w[8:0]}] !== mem.mem[lb(0, y >= 512) + {y[8:0], w[8:0]}]) begin
                            diff = diff + 1;
                            if (diff <= 5) $display("буфер %0d строка %0d слово %0d: %08h, у буфера 0 %08h", b, y, w,
                                                    mem.mem[lb(b, y >= 512) + {y[8:0], w[8:0]}],
                                                    mem.mem[lb(0, y >= 512) + {y[8:0], w[8:0]}]);
                        end
            if (used2 == 0) begin tb_errors = tb_errors + 1; $display("буфер 2 не использовался"); end
            if (diff != 0) begin tb_errors = tb_errors + 1; $display("буферы расходятся: слов %0d", diff); end
        end
        if (mem.errors - err_train == 0 && tb_errors == 0) $display("ТЕСТ ПРОЙДЕН (кадр нарисован; сверка — ft_rtl_check.py)");
        else $display("ТЕСТ НЕ ПРОЙДЕН: протокол SDRAM %0d, проверок %0d", mem.errors - err_train, tb_errors);
        $finish;
    end

    initial begin
        #(64'd2_000_000_000 * FRAMES);
        $display("ТЕСТ НЕ ПРОЙДЕН: время вышло, строк %0d", lines_done);
        $finish;
    end
endmodule
