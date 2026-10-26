// SPDX-License-Identifier: Apache-2.0
//
// FT812: RAM_G (и шрифты ПЗУ) в SDRAM — клиент sdram_burst со своей меткой.
//
// SDRAM пишет и читает пакетами по 8 слов (32 байта), а хост-машины FT812 — байтами
// (SPI Evo) и словами (LINK ESP, сопроцессор). Поэтому:
//   • запись собирается в один из двух буферов на пакет (32 байта и маска байтов);
//     буфер уходит в SDRAM, когда запись ушла в другой пакет, когда q_flush (конец посылки
//     SPI) или через IDLE_FLUSH тактов тишины; пока один буфер пишется, второй собирает;
//   • чтение идёт через кэш на один пакет: подряд идущие байты SPI берутся из него. Промах
//     сначала дописывает в SDRAM всё собранное (чтение видит прежние записи), затем
//     читает пакет. Ответ — по слову, как только оно пришло.
// Запись в пакет, лежащий в кэше, правит и кэш.
//
// Запросы — по одному: q_ready = 1 — принят в этот такт. Ответ на чтение — r_valid.
module ft_ramg #(
    parameter int IDLE_FLUSH = 64
) (
    input  wire        clk,
    input  wire        rst_n,

    input  wire        q_valid,
    output wire        q_ready,
    input  wire        q_we,
    input  wire [20:0] q_waddr,       // адрес слова SDRAM
    input  wire [3:0]  q_be,
    input  wire [31:0] q_wdata,
    input  wire        q_flush,       // такт: дописать собранное
    output reg         r_valid,
    output reg  [31:0] r_data,
    output wire        idle,          // в буферах ничего не ждёт

    // клиент sdram_burst (метка — снаружи, в арбитре)
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
    // --- буферы записи ----------------------------------------------------------------------
    localparam logic [1:0] B_FREE = 2'd0, B_FILL = 2'd1, B_FLUSH = 2'd2, B_BUSY = 2'd3;
    reg  [1:0]  b_st   [0:1];
    reg  [17:0] b_blk  [0:1];         // адрес пакета: слово >> 3
    reg  [3:0]  b_mask [0:1][0:7];
    // данные буферов и кэша — в распределённой памяти (запись — слово за такт, чтение — одно),
    // по байтовым полосам: маска байтов пишется в свою полосу
    reg  [7:0]  bd0 [0:15], bd1 [0:15], bd2 [0:15], bd3 [0:15];   // полоса байта, [{буфер, слово}]
    reg  [7:0]  cd0 [0:7], cd1 [0:7], cd2 [0:7], cd3 [0:7];       // полоса байта, [слово]
    reg         cur;                  // буфер, в который собирается запись
    reg  [6:0]  quiet;                // тактов без записи

    // --- кэш чтения -------------------------------------------------------------------------
    reg         c_on;                 // в кэше пакет c_blk (возможно, ещё приходит)
    reg  [17:0] c_blk;
    reg  [7:0]  c_have;               // какие слова пришли
    reg         c_kill;               // пока пакет читался, в него писали: после — сбросить

    // --- ожидающее чтение -------------------------------------------------------------------
    reg         rd_wait;              // чтение принято, ответа ещё нет
    reg  [20:0] rd_addr;
    reg         rd_issued;            // пакет для него запрошен

    // --- операция SDRAM ------------------------------------------------------------------------
    reg         op_on;                // запрос выставлен или операция идёт
    reg         op_we;
    reg         op_buf;               // какой буфер пишется
    reg  [17:0] op_blk;
    reg         op_acked;

    wire [17:0] q_blk  = q_waddr[20:3];
    wire [2:0]  q_word = q_waddr[2:0];

    // Собирается всегда буфер cur. Новый пакет — в cur, если он свободен, иначе во второй.
    wire wr_hit  = b_st[cur] == B_FILL && b_blk[cur] == q_blk;
    wire wr_new  = b_st[cur] == B_FREE || b_st[!cur] == B_FREE;
    wire c_hit   = c_on && !c_kill && c_blk == q_blk && c_have[q_word];
    wire bufs_free = b_st[0] == B_FREE && b_st[1] == B_FREE;
    // кэш уже держит (или получает) пакет, которого ждёт чтение: собранное не трогаем
    wire c_hit_wait = c_on && !c_kill && c_blk == rd_addr[20:3];

    // Запись принимается, если есть куда; чтение — если другое чтение не ждёт.
    assign q_ready = q_valid && (q_we ? (wr_hit || wr_new) : !rd_wait);
    assign idle    = bufs_free && !op_on;

    // Какой буфер пишется в SDRAM следующим: старший по очереди — тот, что не cur.
    wire want0 = b_st[0] == B_FLUSH;
    wire want1 = b_st[1] == B_FLUSH;
    wire next_buf = (want0 && want1) ? !cur : want1;

    assign g_req   = op_on && !op_acked;
    assign g_we    = op_we;
    assign g_addr  = {op_blk, 3'b000};
    wire [3:0]  b_ra   = {op_buf, g_wr_idx};
    assign g_wdata = {bd3[b_ra], bd2[b_ra], bd1[b_ra], bd0[b_ra]};

    // запись данных: буфер сборки (put/start) и кэш (пакет из SDRAM или запись хоста в него)
    wire        b_we   = q_ready && q_we;
    wire [3:0]  b_wa   = {wr_hit || b_st[cur] == B_FREE ? cur : !cur, q_word};
    wire        cw_rd  = op_on && !op_we && g_rd_valid;            // слово пакета из SDRAM
    wire        cw_q   = b_we && c_on && c_blk == q_blk && c_have == 8'hFF;
    wire [2:0]  c_wa   = cw_rd ? g_rd_idx : q_word;
    wire [31:0] c_wd   = cw_rd ? g_rd_data : q_wdata;
    wire [3:0]  c_wbe  = cw_rd ? 4'hF : (cw_q ? q_be : 4'h0);
    // чтение кэша: по адресу запроса или ждущего чтения (одно за такт)
    wire [2:0]  c_ra   = rd_wait ? rd_addr[2:0] : q_word;
    wire [31:0] c_q    = {cd3[c_ra], cd2[c_ra], cd1[c_ra], cd0[c_ra]};
    always_ff @(posedge clk) begin
        if (b_we && q_be[0]) bd0[b_wa] <= q_wdata[7:0];
        if (b_we && q_be[1]) bd1[b_wa] <= q_wdata[15:8];
        if (b_we && q_be[2]) bd2[b_wa] <= q_wdata[23:16];
        if (b_we && q_be[3]) bd3[b_wa] <= q_wdata[31:24];
        if (c_wbe[0]) cd0[c_wa] <= c_wd[7:0];
        if (c_wbe[1]) cd1[c_wa] <= c_wd[15:8];
        if (c_wbe[2]) cd2[c_wa] <= c_wd[23:16];
        if (c_wbe[3]) cd3[c_wa] <= c_wd[31:24];
    end
    assign g_wmask = b_mask[op_buf][g_wr_idx];

    integer k;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            b_st[0] <= B_FREE; b_st[1] <= B_FREE;
            cur <= 1'b0; quiet <= '0;
            c_on <= 1'b0; c_have <= '0; c_kill <= 1'b0;
            rd_wait <= 1'b0; rd_issued <= 1'b0;
            op_on <= 1'b0; op_acked <= 1'b0;
            r_valid <= 1'b0;
        end else begin
            r_valid <= 1'b0;

            // --- запись: в буфер сборки -----------------------------------------------------------
            if (q_ready && q_we) begin
                quiet <= '0;
                if (wr_hit) begin
                    put(cur, q_word, q_be, q_wdata);
                end else if (b_st[cur] == B_FREE) begin
                    start(cur, q_blk, q_word, q_be, q_wdata);
                end else begin
                    // текущий собирался — в очередь на запись; новый пакет — во второй
                    if (b_st[cur] == B_FILL)
                        b_st[cur] <= B_FLUSH;
                    start(!cur, q_blk, q_word, q_be, q_wdata);
                    cur <= !cur;
                end
                // запись в пакет кэша правит кэш (данные — cw_q), если он пришёл весь
                if (c_on && c_blk == q_blk && c_have != 8'hFF)
                    c_kill <= 1'b1;
            end else if (b_st[cur] == B_FILL) begin
                quiet <= quiet + 7'd1;
                if (q_flush || quiet == 7'(IDLE_FLUSH) || (rd_wait && !c_hit_wait))
                    b_st[cur] <= B_FLUSH;
            end
            // полный пакет — сразу в очередь
            if (b_st[cur] == B_FILL && &{b_mask[cur][0], b_mask[cur][1], b_mask[cur][2],
                                         b_mask[cur][3], b_mask[cur][4], b_mask[cur][5],
                                         b_mask[cur][6], b_mask[cur][7]} && !(q_ready && q_we))
                b_st[cur] <= B_FLUSH;

            // --- чтение ---------------------------------------------------------------------------
            if (q_ready && !q_we) begin
                if (c_hit) begin
                    r_valid <= 1'b1;
                    r_data  <= c_q;
                end else begin
                    rd_wait   <= 1'b1;
                    rd_addr   <= q_waddr;
                    rd_issued <= c_on && c_blk == q_blk;   // пакет уже читается
                end
            end
            if (rd_wait && c_on && c_blk == rd_addr[20:3] && c_have[rd_addr[2:0]] && !c_kill) begin
                rd_wait <= 1'b0;
                r_valid <= 1'b1;
                r_data  <= c_q;
            end

            // --- операции SDRAM -------------------------------------------------------------------
            if (!op_on) begin
                if (want0 || want1) begin
                    op_on    <= 1'b1;
                    op_we    <= 1'b1;
                    op_buf   <= next_buf;
                    op_blk   <= b_blk[next_buf];
                    op_acked <= 1'b0;
                    b_st[next_buf] <= B_BUSY;
                end else if (rd_wait && !rd_issued && bufs_free) begin
                    // промах: всё собранное уже в SDRAM — читаем пакет
                    op_on     <= 1'b1;
                    op_we     <= 1'b0;
                    op_blk    <= rd_addr[20:3];
                    op_acked  <= 1'b0;
                    rd_issued <= 1'b1;
                    c_on      <= 1'b1;
                    c_blk     <= rd_addr[20:3];
                    c_have    <= '0;
                    c_kill    <= 1'b0;
                end
            end else begin
                if (g_ack)
                    op_acked <= 1'b1;
                if (op_we && g_wr_take && g_wr_idx == 3'd7) begin
                    op_on <= 1'b0;
                    b_st[op_buf] <= B_FREE;
                end
                if (!op_we && g_rd_valid) begin
                    c_have[g_rd_idx] <= 1'b1;
                    if (g_rd_idx == 3'd7) begin
                        op_on <= 1'b0;
                        if (c_kill) begin
                            c_on <= 1'b0;
                            c_kill <= 1'b0;
                            if (rd_wait) rd_issued <= 1'b0;   // перечитать: в пакет писали
                        end
                    end
                    // слово, которого ждут, — сразу в ответ
                    if (rd_wait && !c_kill && c_blk == rd_addr[20:3] && g_rd_idx == rd_addr[2:0]) begin
                        rd_wait <= 1'b0;
                        r_valid <= 1'b1;
                        r_data  <= g_rd_data;
                    end
                end
            end
        end
    end


    // данные — в bd (b_we), здесь только маска
    task automatic put(input logic bi, input logic [2:0] w, input logic [3:0] be,
                       input logic [31:0] d);
        b_mask[bi][w] <= b_mask[bi][w] | be;
    endtask

    task automatic start(input logic bi, input logic [17:0] blk, input logic [2:0] w,
                         input logic [3:0] be, input logic [31:0] d);
        b_st[bi]  <= B_FILL;
        b_blk[bi] <= blk;
        for (int j = 0; j < 8; j++)
            b_mask[bi][j] <= (j == w) ? be : 4'h0;
    endtask
endmodule
