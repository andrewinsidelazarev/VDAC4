// SPDX-License-Identifier: Apache-2.0
//
// Мост клиентов SDRAM (clk, 61,875 МГц) и контроллера sdram_burst на удвоенной частоте
// (clk_sd = 2 × clk, 123,75 МГц; оба — из одного PLL, фронт clk совпадает с каждым вторым
// фронтом clk_sd). Ядро FT812 (ts_fb_core, SD_FAST): вывод 720p60 один занимал 42 % тактов
// SDRAM, выгрузка плиток — до 40 %; на удвоенной частоте каждая операция держит шину вдвое
// меньше, а клиенты по-прежнему берут и отдают слово за такт clk.
//
// Сторона клиентов — как у sdram_burst: req/we/addr/req_len/req_tag до ack; после ack записи
// подряд идут wr_take (со следующего такта, слово — в тот же такт, wdata/wmask), чтения —
// rd_valid с rd_idx. Отличие: ack — сразу, как только команда встала в очередь, а слова
// записи и чтения идут позже, своими метками (wr_tag, rd_tag): в одном такте могут быть ack
// одного клиента, wr_take другого и rd_valid третьего.
//
// Порядок операций — по приёму у каждого клиента (чтение клиента после его записи видит её);
// между клиентами — команда записи встаёт в очередь, когда собраны все её слова:
//   запись — слова клиента сначала целиком собираются в FIFO записи (по слову за такт clk),
//            и только тогда команда встаёт в очередь (место под неё отложено при приёме), потом
//            контроллер пишет их пакетами на clk_sd. Раньше команда вставала при приёме и держала
//            голову очереди, пока собирались её слова (32 такта у строки плитки), — за ней ждали
//            чтения вывода и тексели (стенд tb/vsys, ядро 1.24: очередь полна до 24 % кадра).
//            Пока собирается запись, чтение с той же меткой не принимается (RAM_G — чтение и
//            запись с одной меткой: его порядок прежний); прочие чтения могут встать раньше —
//            у них нет чтений, ждущих эту запись в эти такты (тексели растеризатора — метка 100,
//            его выгрузка — 101, области разные; кадр FT вывод читает после объявления);
//   чтение — контроллер на clk_sd читает в FIFO чтения (запрос подаётся, только если под все
//            его слова есть место), клиенту слова идут по одному за такт clk, по порядку;
//   тексели — чтение с меткой TX_TAG (пакет текселей и палитры растеризатора) идёт своей
//            очередью (2 места) мимо общей: контроллер берёт его раньше головы общей очереди, а
//            слова — в свой FIFO ответа (16 слов), который отдаёт клиенту раньше слов вывода.
//            Длинные операции общей очереди контроллер ведёт кусками по 2 пакета (16 слов) —
//            тексель встаёт между ними. Порядок с записями RAM_G не держится: чтение текселя
//            во время записи хостом той же строки RAM_G видит её или нет — как у FT812, где
//            растеризатор и хост тоже не согласованы. Раньше заявка текселя стояла за
//            чтениями вывода и выгрузками (до 64 слов каждое), а слова — за словами вывода в
//            общем FIFO: промах кэша стоил 45–59 тактов clk (стенд tb/vsys, ядро 1.24).
// Сигналы между доменами — только из регистров в регистры (пути — такт clk_sd).
module sd_bridge #(
    parameter int TAGW   = 3,
    parameter int RD_AW  = 9,         // FIFO чтения: 2^RD_AW слов (BSRAM)
    parameter logic [TAGW-1:0] TX_TAG = 3'b100   // метка чтений текселей (своя очередь)
) (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        clk_sd,
    input  wire        rst_sd_n,

    // клиенты (домен clk)
    input  wire        req,
    input  wire        we,
    input  wire [20:0] addr,
    input  wire [2:0]  req_len,
    input  wire [TAGW-1:0] req_tag,
    output reg         ack,
    output reg  [TAGW-1:0] ack_tag,
    output reg         wr_take,
    output reg  [5:0]  wr_idx,
    output reg  [TAGW-1:0] wr_tag,
    input  wire [31:0] wdata,
    input  wire [3:0]  wmask,
    output reg         rd_valid,
    output reg  [5:0]  rd_idx,
    output reg  [TAGW-1:0] rd_tag,
    output reg  [31:0] rd_data,
    input  wire [1:0]  rd_phase,
    output reg         init_done,

    // контроллер sdram_burst (домен clk_sd)
    output reg         c_req,
    output reg         c_we,
    output reg  [20:0] c_addr,
    output reg  [2:0]  c_len,
    input  wire        c_ack,
    input  wire        c_wr_take,
    output wire [31:0] c_wdata,
    output wire [3:0]  c_wmask,
    input  wire        c_rd_valid,
    input  wire [31:0] c_rd_data,
    output reg  [1:0]  c_rd_phase,
    input  wire        c_init_done
);
    localparam int CQ = 4;                       // очередь команд
    localparam int RI = 8;                       // очередь чтений (метка, слов) у стороны clk
    localparam int RD_N = 1 << RD_AW;

    // --- очередь команд: пишет clk, читает clk_sd ---------------------------------------------
    reg  [24+TAGW:0] cq_mem [0:CQ-1];            // {we, addr, len, tag}
    reg  [2:0]  cq_w;                            // домен clk (на разряд больше индекса)
    reg  [2:0]  cq_r;                            // домен clk_sd
    reg  [2:0]  cq_r_m, cq_w_s;                  // копии в чужом домене

    // --- FIFO записи 64 слова (данные и маска): пишет clk, читает clk_sd (без задержки чтения) --
    reg  [35:0] wf_mem [0:63];
    reg  [6:0]  wf_w;                            // домен clk
    reg  [6:0]  wf_r;                            // домен clk_sd
    reg  [6:0]  wf_r_m, wf_w_s;
    assign {c_wmask, c_wdata} = wf_mem[wf_r[5:0]];

    // --- очередь текселей (адреса пакетов) и их FIFO ответа: 2 заявки, 16 слов -----------------
    reg  [20:0] tq_mem [0:1];
    reg  [1:0]  tq_w;                            // домен clk
    reg  [1:0]  tq_r;                            // домен clk_sd
    reg  [1:0]  tq_r_m, tq_w_s;
    reg  [31:0] tf_mem [0:15];
    reg  [4:0]  tf_w;                            // домен clk_sd
    reg  [4:0]  tf_res;                          // clk_sd: занято с поданными чтениями
    reg  [4:0]  tf_r;                            // домен clk
    reg  [4:0]  tf_r_s, tf_w_m;
    reg         c_tx;                            // поданная операция — тексель
    // куда идут слова чтения: операция, которую контроллер принял последней (c_ack). Слова прошлой
    // операции приходят и после её c_ack, пока подаётся следующая, — c_tx для них уже не годится;
    // следующую контроллер принимает только после всех слов прошлой (S_WAIT: CL + 8 + tRP)
    reg         c_dtx;
    always_ff @(posedge clk_sd) if (c_rd_valid && c_dtx) tf_mem[tf_w[3:0]] <= c_rd_data;

    // --- FIFO чтения: пишет clk_sd, читает clk ------------------------------------------------
    reg  [31:0] rf_mem [0:RD_N-1];
    reg  [RD_AW:0] rf_w;                         // домен clk_sd
    reg  [RD_AW:0] rf_res;                       // clk_sd: занято с поданными чтениями
    reg  [RD_AW:0] rf_r;                         // домен clk
    reg  [RD_AW:0] rf_r_s, rf_w_m;
    reg  [31:0] rf_q;
    always_ff @(posedge clk_sd) if (c_rd_valid && !c_dtx) rf_mem[rf_w[RD_AW-1:0]] <= c_rd_data;
    always_ff @(posedge clk) rf_q <= rf_mem[rf_r[RD_AW-1:0]];

    // ===================================== домен clk ==========================================
    // очередь чтений: метка и число слов каждого принятого чтения, по порядку
    reg  [TAGW-1:0] ri_tag [0:RI-1];
    reg  [6:0]  ri_n   [0:RI-1];
    reg  [3:0]  ri_w, ri_r;
    reg         w_pend;                          // идут wr_take принятой записи
    reg  [6:0]  w_n, w_i;
    reg         w_resv;                          // принятая запись ждёт места в очереди
    reg  [24+TAGW:0] w_ent;                      // её команда — в очередь с последним словом
    reg         wr_last;                         // этот wr_take — последнее слово записи
    reg  [2:0]  t_i;                             // слово выдаваемого пакета текселей
    reg         t_v;                             // слово текселя снято, выдаётся следующим тактом
    reg  [31:0] t_q;
    reg  [2:0]  t_idx;
    reg  [6:0]  d_i;                             // слово выдаваемого чтения
    reg         d_v;                             // слово снято с FIFO, выдаётся следующим тактом
    reg  [TAGW-1:0] d_tag;
    reg  [5:0]  d_idx;

    wire [6:0]  n_req   = {req_len, 3'd0} + 7'd8;
    wire        cq_room = (cq_w - cq_r_m) + {2'd0, w_resv} != 3'(CQ);
    wire        ri_room = (ri_w - ri_r) != 4'(RI);
    wire        wf_room = 7'd64 - (wf_w - wf_r_m) >= n_req;
    wire        w_push  = wr_take && wr_last;    // последнее слово — команда в очередь
    wire        w_coll  = w_pend || wr_take;     // идёт сбор записи
    wire        is_tx   = !we && req_tag == TX_TAG;
    wire        tq_room = (tq_w - tq_r_m) != 2'd2;
    wire        take_tx = req && !ack && is_tx && tq_room;
    wire        take    = req && !ack && !is_tx && cq_room && !w_push &&
                          (we ? (!w_coll && !w_resv && wf_room)
                              : (ri_room && !(w_coll && req_tag == wr_tag)));
    wire        tf_have = tf_w_m != tf_r;
    wire [RD_AW:0] rf_have = rf_w_m - rf_r;
    wire        d_go    = rf_have != '0 && ri_w != ri_r && !tf_have;   // слова текселей — раньше
    wire        d_last  = d_i + 7'd1 == ri_n[ri_r[2:0]];

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            ack <= 1'b0; ack_tag <= '0; wr_take <= 1'b0; wr_idx <= '0; wr_tag <= '0;
            rd_valid <= 1'b0; rd_idx <= '0; rd_tag <= '0; rd_data <= '0;
            cq_w <= '0; cq_r_m <= '0; wf_w <= '0; wf_r_m <= '0; rf_r <= '0; rf_w_m <= '0;
            tq_w <= '0; tq_r_m <= '0; tf_r <= '0; tf_w_m <= '0;
            t_i <= '0; t_v <= 1'b0; t_q <= '0; t_idx <= '0;
            ri_w <= '0; ri_r <= '0; w_pend <= 1'b0; w_n <= '0; w_i <= '0;
            w_resv <= 1'b0; w_ent <= '0; wr_last <= 1'b0;
            d_i <= '0; d_v <= 1'b0; d_tag <= '0; d_idx <= '0; init_done <= 1'b0;
        end else begin
            cq_r_m    <= cq_r;
            wf_r_m    <= wf_r;
            rf_w_m    <= rf_w;
            tq_r_m    <= tq_r;
            tf_w_m    <= tf_w;
            init_done <= c_init_done;
            ack       <= 1'b0;
            wr_take   <= 1'b0;

            // приём чтения текселей — в свою очередь
            if (take_tx) begin
                ack     <= 1'b1;
                ack_tag <= req_tag;
                tq_mem[tq_w[0]] <= addr;
                tq_w    <= tq_w + 2'd1;
            end
            // приём команды
            if (take) begin
                ack     <= 1'b1;
                ack_tag <= req_tag;
                if (we) begin
                    // команда — в очередь с последним словом (w_push), место отложено
                    w_ent  <= {we, addr, req_len, req_tag};
                    w_resv <= 1'b1;
                    w_pend <= 1'b1;
                    w_n    <= n_req;
                    w_i    <= '0;
                    wr_tag <= req_tag;
                end else begin
                    cq_mem[cq_w[1:0]] <= {we, addr, req_len, req_tag};
                    cq_w    <= cq_w + 3'd1;
                    ri_tag[ri_w[2:0]] <= req_tag;
                    ri_n[ri_w[2:0]]   <= n_req;
                    ri_w <= ri_w + 4'd1;
                end
            end

            // слова записи: wr_take со следующего после ack такта, слово — в конце такта wr_take
            if (wr_take) begin
                wf_mem[wf_w[5:0]] <= {wmask, wdata};
                wf_w <= wf_w + 7'd1;
            end
            // слова собраны: команда записи — в очередь (в том же такте, что и последнее слово в
            // FIFO, — сторона clk_sd видит их вместе); чтение в этот такт не принимается (take)
            if (w_push) begin
                cq_mem[cq_w[1:0]] <= w_ent;
                cq_w   <= cq_w + 3'd1;
                w_resv <= 1'b0;
            end
            wr_last <= 1'b0;
            if (w_pend) begin
                wr_take <= 1'b1;
                wr_idx  <= w_i[5:0];
                wr_last <= w_i + 7'd1 == w_n;
                w_i     <= w_i + 7'd1;
                if (w_i + 7'd1 == w_n) w_pend <= 1'b0;
            end

            // слова чтения: снять с FIFO (rf_q — слово этого адреса к следующему такту) и выдать
            d_v <= 1'b0;
            t_v <= 1'b0;
            if (tf_have) begin
                tf_r  <= tf_r + 5'd1;
                t_v   <= 1'b1;
                t_q   <= tf_mem[tf_r[3:0]];
                t_idx <= t_i;
                t_i   <= t_i + 3'd1;
            end
            if (d_go) begin
                rf_r  <= rf_r + 1'b1;
                d_v   <= 1'b1;
                d_tag <= ri_tag[ri_r[2:0]];
                d_idx <= d_i[5:0];
                if (d_last) begin
                    d_i  <= '0;
                    ri_r <= ri_r + 4'd1;
                end else begin
                    d_i <= d_i + 7'd1;
                end
            end
            rd_valid <= d_v || t_v;
            if (t_v) begin
                rd_data <= t_q;
                rd_tag  <= TX_TAG;
                rd_idx  <= {3'd0, t_idx};
            end else if (d_v) begin
                rd_data <= rf_q;
                rd_tag  <= d_tag;
                rd_idx  <= d_idx;
            end
        end
    end

    // ==================================== домен clk_sd ========================================
    wire [24+TAGW:0] head = cq_mem[cq_r[1:0]];
    wire        h_we  = head[24+TAGW];
    wire [6:0]  h_n   = {head[TAGW+2:TAGW], 3'd0} + 7'd8;
    // кусок головы: 2 пакета (или остаток); первый кусок записи — когда собраны все её слова
    reg         ch_on;                       // голова идёт кусками, следующий — с ch_addr
    reg  [20:0] ch_addr;
    reg  [2:0]  ch_left;                     // пакетов головы осталось − 1
    wire [2:0]  h_len = ch_on ? ch_left : head[TAGW+2:TAGW];
    wire [20:0] h_addr = ch_on ? ch_addr : head[TAGW+23:TAGW+3];
    wire [2:0]  k_len = h_len > 3'd1 ? 3'd1 : h_len;          // пакетов куска − 1
    wire [6:0]  k_n   = {k_len, 3'd0} + 7'd8;
    // Место под кусок — регистрами, по значениям следующего такта (короткий путь в c_req):
    // слов записи в FIFO (wf_r — с учётом снятого сейчас слова) и места в FIFO чтения под кусок
    // в 8 и 16 слов (rf_res — с учётом принятого сейчас чтения). wf_w_s и rf_r_s за такт только
    // растут — оценка с запасом.
    reg  [6:0]  wf_av;
    reg         rd_ok8, rd_ok16;
    wire [RD_AW:0]   rf_res_nx = rf_res + ((c_req && c_ack && !c_tx && !c_we) ?
                                           (RD_AW+1)'({c_len, 3'd0} + 7'd8) : (RD_AW+1)'(0));
    wire [RD_AW+1:0] rf_use_nx = {1'b0, rf_res_nx - rf_r_s};
    wire        h_ok  = h_we ? (ch_on || wf_av >= h_n) : (k_len[0] ? rd_ok16 : rd_ok8);
    wire        t_ok  = tq_w_s != tq_r && (5'd16 - (tf_res - tf_r_s)) >= 5'd8;
    always_ff @(posedge clk_sd or negedge rst_sd_n) begin
        if (!rst_sd_n) begin
            cq_r <= '0; cq_w_s <= '0; wf_r <= '0; wf_w_s <= '0;
            rf_w <= '0; rf_res <= '0; rf_r_s <= '0;
            tq_r <= '0; tq_w_s <= '0; tf_w <= '0; tf_res <= '0; tf_r_s <= '0; c_tx <= 1'b0; c_dtx <= 1'b0;
            ch_on <= 1'b0; ch_addr <= '0; ch_left <= '0;
            wf_av <= '0; rd_ok8 <= 1'b0; rd_ok16 <= 1'b0;
            c_req <= 1'b0; c_we <= 1'b0; c_addr <= '0; c_len <= '0; c_rd_phase <= 2'd1;
        end else begin
            cq_w_s     <= cq_w;
            wf_w_s     <= wf_w;
            rf_r_s     <= rf_r;
            tq_w_s     <= tq_w;
            tf_r_s     <= tf_r;
            c_rd_phase <= rd_phase;
            wf_av      <= wf_w_s - (wf_r + {6'd0, c_wr_take});
            rd_ok8     <= rf_use_nx + (RD_AW+2)'(8)  <= (RD_AW+2)'(RD_N);
            rd_ok16    <= rf_use_nx + (RD_AW+2)'(16) <= (RD_AW+2)'(RD_N);
            if (c_wr_take)  wf_r <= wf_r + 7'd1;
            if (c_rd_valid && !c_dtx) rf_w <= rf_w + 1'b1;
            if (c_rd_valid && c_dtx)  tf_w <= tf_w + 5'd1;
            if (c_req) begin
                if (c_ack) begin
                    c_req <= 1'b0;
                    c_dtx <= c_tx;
                    if (c_tx) begin
                        tq_r   <= tq_r + 2'd1;
                        tf_res <= tf_res + 5'd8;
                    end else begin
                        if (!c_we) rf_res <= rf_res + (RD_AW+1)'({c_len, 3'd0} + 7'd8);
                        // кусок ушёл: голова — дальше кусками или следующая команда
                        if (h_len > k_len) begin
                            ch_on   <= 1'b1;
                            ch_addr <= h_addr + 21'({k_len, 3'd0} + 4'd8);
                            ch_left <= h_len - k_len - 3'd1;
                        end else begin
                            ch_on <= 1'b0;
                            cq_r  <= cq_r + 3'd1;
                        end
                    end
                end
            end else if (t_ok) begin
                // тексель — раньше головы общей очереди (и между её кусками)
                c_req  <= 1'b1;
                c_we   <= 1'b0;
                c_tx   <= 1'b1;
                c_addr <= tq_mem[tq_r[0]];
                c_len  <= 3'd0;
            end else if (cq_w_s != cq_r && h_ok) begin
                c_req  <= 1'b1;
                c_we   <= h_we;
                c_tx   <= 1'b0;
                c_addr <= h_addr;
                c_len  <= k_len;
            end
        end
    end
endmodule
