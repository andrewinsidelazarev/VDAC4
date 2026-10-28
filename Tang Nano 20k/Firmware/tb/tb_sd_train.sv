`timescale 1ns/1ps
//
// Стенд подбора сдвига клока SDRAM (sdram_train + sdram_burst IOB_CAP, ядро FT812 с 1.38) на модели
// с задержками корпуса. Клок микросхемы — clk_sd через сдвиг PLL (phase/16 периода) и задержку TK;
// команды, адрес, DQM и слова записи доходят до микросхемы через TC, слова чтения — обратно через
// TD плюс свой разброс у каждой линии (до SKEW). Микросхема берёт линию неверно (случайное значение),
// если та менялась ближе T_IS до её фронта или T_IH после; слово чтения у регистра ПЛИС годно
// только с T_AC + T_SU после фронта микросхемы до T_OH − T_H после следующего (вне окна на шине —
// случайное слово, как в tb/sdram_model.sv). Так неверный сдвиг портит и команды, и запись,
// и чтение — как на плате, только задержки здесь известны.
//
// Проверяется: подбор находит годный участок и встаёт на его середину; клиент до конца подбора не
// видит ни ack, ни wr_take, ни rd_valid; после подбора — случайные запросы (запись с масками и
// чтение, 1…8 пакетов) против теневой памяти без единого расхождения и без нарушений протокола
// модели (до конца подбора нарушения ожидаемы: на негодных сдвигах команды берутся неверно).
//
// Прогон: sh scripts/run_sd_train.sh (несколько наборов задержек).
module tb_sd_train #(
    parameter real T_SD = 8.2305,   // 121,5 МГц
    parameter real TK   = 0.0,      // клок: выход CLKOUTP → микросхема
    parameter real TC   = 0.0,      // команды, адрес, DQM, слова записи: регистр у вывода → микросхема
    parameter real TD   = 0.0,      // слово чтения: микросхема → регистр у вывода ПЛИС
    parameter real SKEW = 0.0,      // разброс TD по линиям DQ, до
    parameter real T_AC = 6.0,      // паспорт SDRAM (CL 3, наихудший)
    parameter real T_OH = 2.5,
    parameter real T_IS = 1.5,
    parameter real T_IH = 0.8,
    parameter real T_SU = 0.3,      // регистр у вывода ПЛИС
    parameter real T_H  = 0.3,
    parameter int  NREQ = 1500,     // случайных запросов после подбора
    parameter int  SEED = 1
);
    localparam real D = T_IH + 0.01;     // модель тактируется на D позже: решение «взята ли линия»
                                         // принимается после окна удержания
    reg clk = 1'b0;
    initial forever #(T_SD / 2) clk = ~clk;
    reg rst_n = 1'b0;
    initial #20 rst_n = 1'b1;

    // ---- клиент ----
    reg         req = 1'b0, we = 1'b0;
    reg  [20:0] addr = '0;
    reg  [2:0]  len = '0;
    wire        ack, wr_take, rd_valid, init_done;
    wire [5:0]  wr_idx, rd_idx;
    wire [31:0] rd_data;
    reg  [31:0] wbuf [0:63];
    reg  [3:0]  mbuf [0:63];
    wire [31:0] wdata = wbuf[wr_idx];
    wire [3:0]  wmask = mbuf[wr_idx];
    wire [3:0]  phase;
    wire [22:0] st, win;
    reg  [63:0] map = '0;                // карта годных пар — стенд собирает сам (в ядре её нет)

    wire        cs_n, ras_n, cas_n, we_n, dq_oe;
    wire [10:0] a;
    wire [1:0]  ba;
    wire [3:0]  dqm;
    wire [31:0] dq_out;
    reg  [31:0] dq_in;

    sdram_train #(.TAGW(1), .CLK_KHZ(121_500), .CL(3), .T_RCD(2), .T_RP(2), .T_WR(2), .T_RFC(8),
                  .INIT_US(20)) dut (
        .clk(clk), .rst_n(rst_n),
        .req(req), .we(we), .addr(addr), .req_len(len), .req_tag(1'b0),
        .ack(ack), .op_tag(), .wr_take(wr_take), .wr_idx(wr_idx), .wdata(wdata), .wmask(wmask),
        .rd_valid(rd_valid), .rd_idx(rd_idx), .rd_data(rd_data), .init_done(init_done),
        .phase(phase), .train_st(st), .train_win(win),
        .sdram_cs_n(cs_n), .sdram_ras_n(ras_n), .sdram_cas_n(cas_n), .sdram_we_n(we_n),
        .sdram_a(a), .sdram_ba(ba), .sdram_dqm(dqm), .sdram_dq_out(dq_out), .sdram_dq_oe(dq_oe),
        .sdram_dq_in(dq_in)
    );

    // ---- клок микросхемы: сдвиг PLL и задержка до неё ----
    reg sdclk = 1'b0, mclk = 1'b0;
    always @(clk) sdclk <= #(TK + phase * T_SD / 16.0) clk;
    always @(sdclk) mclk <= #(D) sdclk;

    // ---- линии к микросхеме: задержка TC, выборка с окном T_IS/T_IH ----
    localparam int NL = 53;
    wire [NL-1:0] lo = {cs_n, ras_n, cas_n, we_n, a, ba, dqm, dq_out};
    reg  [NL-1:0] lp, lm;
    reg           oe_p;
    always @(lo)    lp   <= #(TC) lo;
    always @(dq_oe) oe_p <= #(TC) dq_oe;
    real tch [0:NL-1];
    genvar gl;
    generate for (gl = 0; gl < NL; gl = gl + 1) begin : g_ln
        initial tch[gl] = -1.0e9;
        always @(lp[gl]) tch[gl] = $realtime;
    end endgenerate
    integer n_bad_in = 0;
    always @(posedge sdclk) begin : smp
        real te;
        reg [NL-1:0] v;
        integer i;
        te = $realtime;
        v  = lp;
        #(D - 0.005);
        for (i = 0; i < NL; i = i + 1)
            if (tch[i] > te - T_IS) begin v[i] = $urandom; n_bad_in = n_bad_in + 1; end
        lm <= v;
    end

    wire [31:0] m_dq;
    sdram_model #(.TCK_NS(T_SD), .CL(3), .T_AC(T_AC + T_SU - D), .T_OH(T_OH - T_H - D)) sdram (
        .clk(mclk), .cs_n(lm[52]), .ras_n(lm[51]), .cas_n(lm[50]), .we_n(lm[49]),
        .a(lm[48:38]), .ba(lm[37:36]), .dqm(lm[35:32]),
        .dq_from_ctrl(lm[31:0]), .dq_oe(oe_p), .dq_to_ctrl(m_dq)
    );

    // ---- слова чтения обратно: TD и разброс по линиям ----
    genvar gq;
    generate for (gq = 0; gq < 32; gq = gq + 1) begin : g_dq
        localparam real SK = SKEW * ((gq * 37) % 32) / 31.0;
        always @(m_dq[gq]) dq_in[gq] <= #(TD + SK) m_dq[gq];
    end endgenerate

    // ---- проверки ----
    integer errs = 0;
    task automatic bad(input string msg);
        if (errs < 20) $display("[%0t] ОШИБКА: %0s", $time, msg);
        errs = errs + 1;
    endtask
    // карта: итог каждой пары — в такт записи итога (T_REC, ещё не проверка выбранной)
    always @(posedge clk) if (rst_n && dut.tst == 6 && !dut.fin) map[dut.k] <= !dut.err;
    // до конца подбора клиент не видит контроллера
    always @(posedge clk) if (rst_n && !st[7] && (ack || wr_take || rd_valid))
        bad("клиент видит контроллер во время подбора");
    always @(posedge clk) if (rst_n && dut.s_rd_valid && $isunknown(dut.s_rd_data))
        bad("слово чтения неопределённое ('x): сверка подбора его не увидела бы");

    reg [31:0] shadow [0:(1<<21)-1];
    reg        shv    [0:(1<<21)-1];

    task automatic xfer(input bit w, input [20:0] ad, input [2:0] ln);
        integer n, got;
        n = 8 * (ln + 1);
        if (w) for (int i = 0; i < n; i++) begin
            wbuf[i] = $urandom;
            mbuf[i] = ($urandom % 4 == 0) ? 4'($urandom) : 4'hF;
        end
        @(negedge clk);
        req = 1'b1; we = w; addr = ad; len = ln;
        do @(posedge clk); while (!ack);
        @(negedge clk) req = 1'b0;
        got = 0;
        while (got < n) begin
            @(posedge clk);
            if (w && wr_take) begin
                for (int b = 0; b < 4; b++)
                    if (mbuf[wr_idx][b]) shadow[ad + wr_idx][8*b +: 8] = wbuf[wr_idx][8*b +: 8];
                if (mbuf[wr_idx] == 4'hF) shv[ad + wr_idx] = 1'b1;   // частичная — только по прописанному
                got++;
            end
            if (!w && rd_valid) begin
                if (shv[ad + rd_idx] && rd_data !== shadow[ad + rd_idx]) begin
                    bad($sformatf("чтение %06h: %08h, ожидалось %08h", ad + rd_idx, rd_data,
                                  shadow[ad + rd_idx]));
                end
                got++;
            end
        end
    endtask

    integer m_err0, seed;
    initial begin : main
        string ms;
        seed = SEED;
        void'($urandom(seed));
        // в микросхеме после включения — мусор, не 'x: иначе сверка с 'x молча проходила бы
        for (int i = 0; i < (1 << 21); i++) begin shv[i] = 1'b0; sdram.mem[i] = $urandom; end
        wait (rst_n);
        wait (init_done);
        m_err0 = sdram.errors;
        $display("подбор: сдвиг %0d, такт %0d, участок %0d пар, середина k=%0d, годных нет=%0d, сверка не сошлась=%0d",
                 st[3:0], st[5:4], st[16:10], st[22:17], st[8], st[9]);
        $display("годные: первая k=%0d, последняя k=%0d, всего %0d", win[5:0], win[13:8], win[22:16]);
        ms = "";
        for (int k = 63; k >= 0; k--) ms = {ms, map[k] ? "1" : "."};
        $display("карта (k=63…0): %s", ms);
        $display("нарушений протокола модели во время подбора: %0d, неверно взятых линий: %0d",
                 m_err0, n_bad_in);
        if (st[8]) bad("подбор не нашёл годных пар");
        if (st[9]) bad("сверка на выбранной паре не сошлась");
        begin : chk_win
            integer n, f, l;
            n = 0; f = -1; l = -1;
            for (int k = 0; k < 64; k++) if (map[k]) begin n++; if (f < 0) f = k; l = k; end
            if (n != win[22:16] || (n != 0 && (f != win[5:0] || l != win[13:8])))
                bad($sformatf("SD_WIN не совпал с картой: первая %0d, последняя %0d, всего %0d", f, l, n));
        end
        n_bad_in = 0;
        // заполнение области: строки 0…15 всех банков целиком
        for (int r = 0; r < 16; r++) for (int b = 0; b < 4; b++) for (int c = 0; c < 4; c++) begin
            for (int i = 0; i < 64; i++) begin wbuf[i] = $urandom; mbuf[i] = 4'hF; end
            xfer_full(1'b1, {11'(r), 2'(b), 8'(c * 64)});
        end
        // случайные запросы
        for (int q = 0; q < NREQ; q++) begin : rq
            bit w;
            logic [2:0] ln;
            logic [4:0] c8;
            w  = $urandom % 2;
            ln = $urandom % 8;
            c8 = $urandom % (32 - ln);
            xfer(w, {11'($urandom % 16), 2'($urandom % 4), c8, 3'b000}, ln);
        end
        repeat (64) @(posedge clk);
        if (sdram.errors != m_err0) bad($sformatf("нарушений протокола после подбора: %0d", sdram.errors - m_err0));
        if (n_bad_in != 0) bad($sformatf("линий, взятых микросхемой неверно, после подбора: %0d", n_bad_in));
        if (errs == 0) $display("ВСЁ ВЕРНО (TK %.2f, TC %.2f, TD %.2f, SKEW %.2f)", TK, TC, TD, SKEW);
        else $display("ТЕСТ НЕ ПРОЙДЕН: %0d ошибок", errs);
        $finish;
    end

    // запись строки целиком без случайных данных (wbuf уже заполнен)
    task automatic xfer_full(input bit w, input [20:0] ad);
        integer got;
        @(negedge clk);
        req = 1'b1; we = w; addr = ad; len = 3'd7;
        do @(posedge clk); while (!ack);
        @(negedge clk) req = 1'b0;
        got = 0;
        while (got < 64) begin
            @(posedge clk);
            if (wr_take) begin
                shadow[ad + wr_idx] = wbuf[wr_idx];
                shv[ad + wr_idx] = 1'b1;
                got++;
            end
        end
    endtask

    initial begin
        #(20_000_000);
        $display("ТЕСТ НЕ ПРОЙДЕН: время вышло");
        $finish;
    end
endmodule
