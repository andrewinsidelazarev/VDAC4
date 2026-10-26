// SPDX-License-Identifier: Apache-2.0
//
// Связь с BL616 — домен ядра: события cp_spi → очередь → порт слов ft_core; буфер чтения;
// снимок состояния; прерывание BL616.
//
// Очередь (BSRAM 512 × 34): адрес WRITE, слова WRITE, запросы чтения — по порядку, так что
// чтение видит всё, что записано до него. Слова идут в ft_core как записи сопроцессора
// (p_cp): ему можно поднимать флаги REG_INT_FLAGS. Место в очереди BL616 узнаёт из STATUS и
// за раз пишет не больше; если очередь всё же переполнилась — флаг, слова отброшены.
// Буфер чтения (BSRAM 512 × 32): пишется ядром, читается cp_spi по SCK.
//
// Снимок STATUS (16 байт, младшим вперёд), снимается по EV_SNAP:
//   0: 0xC5, 1: версия 1,
//   2: флаги {перезагрузка ПЛИС, 0, свап ждёт, кольцо не пусто, переполнение, сброс держит,
//             очередь пуста, чтение готово},
//   3: счётчик сбросов сопроцессора (REG_CPURESET 1 → 0 и сброс ядра FT812),
//   4–5: REG_CMD_WRITE, 6–7: REG_CMD_READ, 8–9: REG_CMD_DL,
//   10: {REG_CPURESET[2:0], REG_DLSWAP[1:0]}, 12–13: свободно в очереди (слов).
// IRQ_N (к BL616) — низкий, пока в кольце есть невыполненное (и нет ошибки 0xFFF), не
// держит сброс, или после сброса сопроцессора до чтения STATUS, или пока ядро просит
// перезагрузку ПЛИС (boot, ядро 1.13+): свой RECONFIG_N ПЛИС не перезагружает (стенд
// 18.10.2026), поэтому перезагрузку по адресу перехода multiboot делает BL616 командой JTAG
// Reconfig (0x3C, bl616/app/fpga_jtag.c).
// Запись по адресу 0x3FFFFC (вне карты FT812) — в ft_core не идёт: это слово состояния
// прошивки BL616 (info), ESP читает его по LINK.
// Почтовый ящик ESP → BL616 (BSRAM 512 × 32): ESP пишет его по LINK (окно, домен пикселей,
// mb_*), BL616 читает READ_REQ по адресам 0x3F0000…0x3F07FC (вне карты FT812) — так приходит
// новая прошивка BL616 (bl616/app/fw_update.c); ответы BL616 — в слове info.
module cp_link (
    input  wire         clk,
    input  wire         rst_n,

    // cp_spi
    input  wire         sck,
    input  wire         spi_tg,
    input  wire [1:0]   spi_kind,
    input  wire [31:0]  spi_data,
    output reg  [127:0] st_vec,
    input  wire [8:0]   rb_addr,
    output reg  [31:0]  rb_q,

    // порт слов ft_core (через арбитр)
    output reg          p_req,
    output reg          p_we,
    output reg  [21:2]  p_addr,
    output reg  [31:0]  p_wdata,
    input  wire         p_ack,
    input  wire [31:0]  p_rdata,

    // состояние ft_core
    input  wire [11:0]  cmd_read,
    input  wire [11:0]  cmd_write,
    input  wire [12:0]  cmd_dl,
    input  wire [1:0]   dlswap,
    input  wire [2:0]   cpureset,
    input  wire [7:0]   resets,
    input  wire         boot,           // ядро просит перезагрузку ПЛИС (домен clk)

    output wire         irq_n,
    output wire         overflow,
    output reg  [31:0]  info,

    // почтовый ящик: запись — со стороны LINK, своим клоком
    input  wire         mb_clk,
    input  wire         mb_we,
    input  wire [8:0]   mb_waddr,
    input  wire [31:0]  mb_wdata
);
    localparam logic [1:0] EV_SNAP = 2'd0, EV_ADDR = 2'd1, EV_WORD = 2'd2, EV_RREQ = 2'd3;

    // --- события ------------------------------------------------------------------------------
    reg  [2:0] tg_s;
    always @(posedge clk) tg_s <= {tg_s[1:0], spi_tg};
    wire ev = tg_s[2] ^ tg_s[1];

    // --- очередь ------------------------------------------------------------------------------
    reg  [33:0] q_mem [0:511];
    reg  [33:0] q_q;
    reg  [8:0]  q_wr, q_rd;
    reg  [9:0]  q_cnt;
    wire        push;
    // слово пишется в такте события: q_cnt станет не 0 тактом позже, а голова (q_q) читается
    // ещё через такт — уже записанной
    always @(posedge clk) begin
        if (push)
            q_mem[q_wr] <= {spi_kind, spi_data};
        q_q <= q_mem[q_rd];
    end

    // --- буфер чтения: запись — clk, чтение — SCK ------------------------------------------------
    reg  [31:0] rb_mem [0:511];
    reg         rb_we;
    reg  [8:0]  rb_wa;
    reg  [31:0] rb_wd;
    always @(posedge clk)
        if (rb_we)
            rb_mem[rb_wa] <= rb_wd;
    always @(posedge sck)
        rb_q <= rb_mem[rb_addr];

    // --- почтовый ящик: запись — mb_clk, чтение — clk -----------------------------------------
    reg  [31:0] mb_mem [0:511];
    reg  [8:0]  mb_ra;
    reg  [31:0] mb_q;
    always @(posedge mb_clk)
        if (mb_we)
            mb_mem[mb_waddr] <= mb_wdata;
    always @(posedge clk)
        mb_q <= mb_mem[mb_ra];

    function automatic logic [9:0] q_free(input logic [9:0] n);
        q_free = 10'd512 - n;
    endfunction

    // --- исполнитель ----------------------------------------------------------------------------
    // X_MB_W / X_MB_D — чтение почтового ящика в буфер: адрес, затем слово (такт BSRAM)
    localparam logic [2:0] X_IDLE = 3'd0, X_TAKE = 3'd1, X_PORT = 3'd2, X_MB_W = 3'd3, X_MB_D = 3'd4;
    reg  [2:0]  xs;
    reg  [21:2] waddr;
    reg         x_rd;                // порт: чтение для буфера
    reg  [8:0]  r_i, r_last;
    reg  [3:0]  r_pend;              // запросов чтения в очереди и в работе
    reg         ovf;
    reg  [7:0]  resets_seen;

    wire ring_pending = cmd_read != cmd_write && cmd_read != 12'hFFF && !cpureset[0];
    wire q_empty = q_cnt == 10'd0;
    wire idle    = q_empty && xs == X_IDLE;
    assign push  = ev && spi_kind != EV_SNAP && q_cnt != 10'd512;
    wire pop     = xs == X_TAKE;
    wire r_inc   = push && spi_kind == EV_RREQ;
    wire r_dec   = (xs == X_PORT && p_ack && x_rd && r_i == r_last) || (xs == X_MB_D && r_i == r_last);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            q_wr <= '0; q_rd <= '0; q_cnt <= '0;
            rb_we <= 1'b0; rb_wa <= '0; rb_wd <= '0;
            xs <= X_IDLE; waddr <= '0; x_rd <= 1'b0; r_i <= '0; r_last <= '0; r_pend <= '0;
            mb_ra <= '0;
            p_req <= 1'b0; p_we <= 1'b0; p_addr <= '0; p_wdata <= '0;
            ovf <= 1'b0; resets_seen <= '0; st_vec <= '0; info <= '0;
        end else begin
            rb_we <= 1'b0;

            // событие: в очередь или снимок
            if (ev) begin
                if (spi_kind == EV_SNAP) begin
                    st_vec <= {16'd0, 6'd0, q_free(q_cnt), 8'd0, {3'd0, cpureset, dlswap},
                               {3'd0, cmd_dl}, {4'd0, cmd_read}, {4'd0, cmd_write}, resets,
                               {boot, 1'b0, dlswap != 2'd0, ring_pending, ovf, cpureset[0], idle,
                                r_pend == 4'd0},
                               8'h01, 8'hC5};
                    resets_seen <= resets;
                end else if (q_cnt == 10'd512) begin
                    ovf <= 1'b1;
                end else begin
                    q_wr <= q_wr + 9'd1;
                end
            end
            q_cnt  <= q_cnt + (push ? 10'd1 : 10'd0) - (pop ? 10'd1 : 10'd0);
            r_pend <= r_pend + (r_inc ? 4'd1 : 4'd0) - (r_dec ? 4'd1 : 4'd0);

            case (xs)
                X_IDLE:
                    // q_q — голова очереди: её слово записано раньше, чем q_cnt стал не 0
                    if (!q_empty)
                        xs <= X_TAKE;
                X_TAKE: begin
                    q_rd <= q_rd + 9'd1;
                    case (q_q[33:32])
                        EV_ADDR: begin
                            waddr <= q_q[21:2];
                            xs    <= X_IDLE;
                        end
                        EV_WORD: begin
                            waddr <= waddr + 20'd1;
                            if (waddr == 20'hFFFFF) begin            // 0x3FFFFC — слово состояния
                                info <= q_q[31:0];
                                xs   <= X_IDLE;
                            end else begin
                                p_req   <= 1'b1;
                                p_we    <= 1'b1;
                                p_addr  <= waddr;
                                p_wdata <= q_q[31:0];
                                x_rd    <= 1'b0;
                                xs      <= X_PORT;
                            end
                        end
                        default: begin                       // EV_RREQ
                            r_i    <= '0;
                            r_last <= {1'b0, q_q[31:24]};
                            if (q_q[21:11] == 11'h7E0) begin         // 0x3F0000… — почтовый ящик
                                mb_ra <= q_q[10:2];
                                xs    <= X_MB_W;
                            end else begin
                                p_req  <= 1'b1;
                                p_we   <= 1'b0;
                                p_addr <= q_q[21:2];
                                x_rd   <= 1'b1;
                                xs     <= X_PORT;
                            end
                        end
                    endcase
                end
                X_MB_W:
                    xs <= X_MB_D;
                X_MB_D: begin
                    rb_we <= 1'b1;
                    rb_wa <= r_i;
                    rb_wd <= mb_q;
                    if (r_i == r_last) begin
                        xs <= X_IDLE;
                    end else begin
                        r_i   <= r_i + 9'd1;
                        mb_ra <= mb_ra + 9'd1;
                        xs    <= X_MB_W;
                    end
                end
                default: begin                               // X_PORT
                    if (p_ack) begin
                        p_req <= 1'b0;
                        if (x_rd) begin
                            rb_we <= 1'b1;
                            rb_wa <= r_i;
                            rb_wd <= p_rdata;
                            if (r_i == r_last) begin
                                xs     <= X_IDLE;
                            end else begin
                                r_i    <= r_i + 9'd1;
                                p_addr <= p_addr + 20'd1;
                                p_req  <= 1'b1;
                            end
                        end else begin
                            xs <= X_IDLE;
                        end
                    end
                end
            endcase
        end
    end


    assign irq_n    = !(ring_pending || resets != resets_seen || boot);
    assign overflow = ovf;

    initial tg_s = 3'b000;
endmodule
