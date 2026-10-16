// SPDX-License-Identifier: Apache-2.0
//
// Кадровый буфер TS-Config в SDRAM: запись принятых кадров, выборка строк для вывода,
// тройная буферизация. Домен clk (клок SDRAM).
//
// ЗАЧЕМ. Вход идёт с частотой 48,83 кадра/с, выход — стандартный 576p50 от кварца.
// Частоты независимы, поэтому между ними стоит память на целые кадры:
//   • писатель пишет кадр в буфер W;
//   • законченный целиком кадр становится «последним готовым» (L);
//   • читатель в начале каждого выходного кадра берёт L и выводит его весь (R).
// Три буфера нужны, чтобы писатель никогда не писал в выводимый кадр: разрывов
// картинки нет. Выход быстрее входа, поэтому примерно раз в 42 кадра один входной кадр
// показывается дважды. Незаконченный или сбойный входной кадр не публикуется.
//
// Раскладка буфера: адрес слова = {буфер[1:0], строка[9:0], слово[8:0]},
// одно слово — два пикселя, строка до 1024 пикселей. Три буфера 720×576 занимают 3,5 МБ.
//
// Выборка строк. Домен вывода заранее (за две строки) просит строку L: переключает
// req_toggle, а req_line/req_first держит неизменными до следующего запроса. Строка
// читается пакетами в половину bank = L[0] двухстрочного буфера fb_line_ram.
//
module fb_core #(
    parameter int H_ACTIVE = 720,           // кратно 16: строка = целое число пакетов
    parameter int V_ACTIVE = 576
) (
    input  wire        clk,
    input  wire        rst_n,

    // FIFO из домена захвата (fb_capture)
    output reg         fifo_rd,
    input  wire [32:0] fifo_q,               // появляется через такт после fifo_rd
    input  wire        fifo_empty,

    // Запросы строк из домена вывода
    input  wire        req_toggle,
    input  wire [9:0]  req_line,
    input  wire        req_first,

    // Запись в двухстрочный буфер вывода
    output reg         lb_we,
    output reg  [10:0] lb_addr,              // {строка[1:0], слово[8:0]}
    output reg  [31:0] lb_wdata,

    // Клиентская сторона sdram_burst
    output wire        mem_req,
    output wire        mem_we,
    output wire [20:0] mem_addr,
    output wire        mem_tag,              // 1 — читатель, 0 — писатель
    input  wire        mem_ack,
    input  wire        mem_op_tag,
    input  wire        mem_wr_take,
    input  wire [2:0]  mem_wr_idx,
    output wire [31:0] mem_wdata,
    input  wire        mem_rd_valid,
    input  wire [31:0] mem_rd_data,

    // Состояние (для OSD, отладки и теста)
    output reg  [15:0] frames_written,       // опубликовано целых кадров
    output reg  [15:0] frames_rejected,      // отброшено сбойных
    output reg         fetch_late,           // липкий: новый запрос строки до окончания старого
    output reg         have_frame            // есть хотя бы один целый кадр
);
    localparam int H_WORDS  = H_ACTIVE / 2;
    localparam int H_BURSTS = H_ACTIVE / 16;
    localparam [9:0] H_WORDS_C  = H_WORDS;
    localparam [9:0] V_LAST_C   = V_ACTIVE - 1;
    localparam [9:0] V_ACTIVE_C = V_ACTIVE;

    initial begin
        if (H_ACTIVE % 16 != 0) $error("fb_core: H_ACTIVE должен быть кратен 16");
        if (H_ACTIVE > 1024)    $error("fb_core: H_ACTIVE не больше 1024");
    end

    // =====================================================================
    // Тройная буферизация. Инвариант: w_buf, r_buf и latest попарно различны.
    // =====================================================================
    reg [1:0] w_buf, r_buf, latest;
    reg       publish;                  // писатель закончил целый кадр
    reg       reader_switch;            // читатель начинает новый выходной кадр

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            w_buf      <= 2'd0;
            r_buf      <= 2'd1;
            latest     <= 2'd2;
            have_frame <= 1'b0;
        end else begin : tb_update
            logic [1:0] r_next;
            r_next = (reader_switch && have_frame) ? latest : r_buf;
            r_buf  <= r_next;
            if (publish) begin
                latest     <= w_buf;
                have_frame <= 1'b1;
                w_buf      <= 2'd3 - w_buf - r_next;   // третий, ни L, ни R
            end
        end
    end

    // =====================================================================
    // Писатель: FIFO → пакеты по 8 слов
    // =====================================================================
    typedef enum logic [1:0] { W_FETCH, W_DATA, W_BURST } wstate_t;
    wstate_t   wst;
    reg [31:0] wbuf [0:7];
    reg [9:0]  w_word;                  // слов строки уже принято
    reg [9:0]  w_y;                     // текущая строка
    reg [9:0]  w_y_exp;                 // какую строку ждём следующей
    reg [9:0]  w_lines;                 // целых строк в текущем кадре
    reg        w_frame_ok;
    reg        w_line_closed;           // предыдущая строка записана целиком
    reg        w_complete;              // все строки кадра записаны, ждём конец кадра
    reg        w_req;
    reg [20:0] w_addr;
    reg        w_sent;                  // пакет принят контроллером (был ack)

    assign mem_wdata = wbuf[mem_wr_idx];

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            wst             <= W_FETCH;
            fifo_rd         <= 1'b0;
            w_word          <= '0;
            w_y             <= '0;
            w_y_exp         <= '0;
            w_lines         <= '0;
            w_frame_ok      <= 1'b0;
            w_line_closed   <= 1'b0;
            w_complete      <= 1'b0;
            w_req           <= 1'b0;
            w_addr          <= '0;
            w_sent          <= 1'b0;
            publish         <= 1'b0;
            frames_written  <= '0;
            frames_rejected <= '0;
        end else begin
            fifo_rd <= 1'b0;
            publish <= 1'b0;
            case (wst)
            W_FETCH: begin
                if (!fifo_empty) begin
                    fifo_rd <= 1'b1;
                    wst     <= W_DATA;
                end
            end
            W_DATA: if (!fifo_rd) begin        // данные FIFO пришли (fifo_rd был тактом раньше)
                wst <= W_FETCH;
                if (fifo_q[32] && fifo_q[11]) begin
                    // Конец кадра: публикуем, только если все строки записаны и захват
                    // подтвердил, что кадр целиком прошёл в одном растре.
                    if (w_complete && fifo_q[0]) begin
                        publish        <= 1'b1;
                        frames_written <= frames_written + 16'd1;
                    end else if (w_complete || w_frame_ok || w_lines != 0) begin
                        frames_rejected <= frames_rejected + 16'd1;
                    end
                    w_complete <= 1'b0;
                    w_frame_ok <= 1'b0;
                    w_lines    <= '0;
                end else if (fifo_q[32]) begin
                    // Заголовок строки. Недописанная предыдущая строка портит кадр.
                    if (fifo_q[10]) begin
                        if (w_complete || w_frame_ok || w_lines != 0)
                            frames_rejected <= frames_rejected + 16'd1;   // конец кадра потерян
                        w_complete <= 1'b0;
                        w_frame_ok <= 1'b1;
                        w_lines    <= '0;
                    end else if (fifo_q[9:0] != w_y_exp || !w_line_closed) begin
                        w_frame_ok <= 1'b0;
                    end
                    w_y           <= fifo_q[9:0];
                    w_word        <= '0;
                    w_line_closed <= 1'b0;
                end else if (w_word < H_WORDS_C) begin
                    wbuf[w_word[2:0]] <= fifo_q[31:0];
                    w_word <= w_word + 10'd1;
                    if (w_word[2:0] == 3'd7) begin
                        w_req  <= 1'b1;
                        w_sent <= 1'b0;
                        w_addr <= {w_buf, w_y, w_word[8:3], 3'b000};
                        wst    <= W_BURST;
                    end
                end
            end
            W_BURST: begin
                if (mem_ack && !mem_op_tag) begin
                    w_req  <= 1'b0;
                    w_sent <= 1'b1;
                end
                // Буфер свободен, когда контроллер забрал последнее слово пакета
                if (w_sent && mem_wr_take && !mem_op_tag && mem_wr_idx == 3'd7) begin
                    wst <= W_FETCH;
                    if (w_word == H_WORDS_C) begin
                        // Строка записана целиком
                        w_y_exp       <= w_y + 10'd1;
                        w_line_closed <= 1'b1;
                        if (w_frame_ok && w_y == w_lines) begin
                            w_lines <= w_lines + 10'd1;
                            if (w_y == V_LAST_C) begin
                                w_complete <= 1'b1;         // ждём конца кадра
                                w_frame_ok <= 1'b0;
                                w_lines    <= '0;
                            end
                        end else begin
                            w_frame_ok <= 1'b0;
                        end
                    end
                end
            end
            default: wst <= W_FETCH;
            endcase
        end
    end

    // =====================================================================
    // Читатель: запрос строки → пакеты чтения → строчный буфер
    // =====================================================================
    reg [2:0]  tog_sync /* synthesis syn_srlstyle = "registers" */;
    reg        req_pending;
    reg [9:0]  pend_line;
    reg        pend_first;

    typedef enum logic [1:0] { R_IDLE, R_FETCH, R_ZERO } rstate_t;
    rstate_t   rst_q;
    reg        r_req;
    reg [20:0] r_addr;
    reg [9:0]  r_line;
    reg [1:0]  r_bank;
    reg        r_valid;                 // выводимый кадр действителен (иначе чёрный)
    reg [5:0]  r_burst;                 // пакетов запрошено
    reg [9:0]  r_rx;                    // слов принято / записано нулей

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            tog_sync      <= '0;
            req_pending   <= 1'b0;
            pend_line     <= '0;
            pend_first    <= 1'b0;
            rst_q         <= R_IDLE;
            r_req         <= 1'b0;
            r_addr        <= '0;
            r_line        <= '0;
            r_bank        <= '0;
            r_valid       <= 1'b0;
            r_burst       <= '0;
            r_rx          <= '0;
            reader_switch <= 1'b0;
            lb_we         <= 1'b0;
            lb_addr       <= '0;
            lb_wdata      <= '0;
            fetch_late    <= 1'b0;
        end else begin
            reader_switch <= 1'b0;
            lb_we         <= 1'b0;

            // Переход запроса из домена вывода. req_line/req_first к моменту смены
            // синхронизированного переключателя стабильны уже больше строки.
            tog_sync <= {tog_sync[1:0], req_toggle};
            if (tog_sync[2] ^ tog_sync[1]) begin
                if (req_pending || rst_q != R_IDLE) fetch_late <= 1'b1;
                req_pending <= 1'b1;
                pend_line   <= req_line;
                pend_first  <= req_first;
            end

            case (rst_q)
            R_IDLE: if (req_pending && !(tog_sync[2] ^ tog_sync[1])) begin
                req_pending <= 1'b0;
                r_line  <= pend_line;
                r_bank  <= pend_line[1:0];
                r_burst <= '0;
                r_rx    <= '0;
                if (pend_first) begin
                    // Новый выходной кадр: берём последний готовый (сам выбор — в блоке
                    // тройной буферизации, r_buf обновится к следующему такту)
                    reader_switch <= 1'b1;
                    r_valid       <= have_frame;
                    if (have_frame) rst_q <= R_FETCH; else rst_q <= R_ZERO;
                end else begin
                    if (r_valid) rst_q <= R_FETCH; else rst_q <= R_ZERO;
                end
            end
            R_FETCH: begin
                // r_buf уже учёл переключение (прошёл минимум такт)
                if (!r_req && r_burst != H_BURSTS[5:0] && !reader_switch) begin
                    r_req  <= 1'b1;
                    r_addr <= {r_buf, r_line, r_burst, 3'b000};
                end
                if (r_req && mem_ack && mem_op_tag) begin
                    r_req   <= 1'b0;
                    r_burst <= r_burst + 6'd1;
                end
                if (mem_rd_valid && mem_op_tag) begin
                    lb_we    <= 1'b1;
                    lb_addr  <= {r_bank, r_rx[8:0]};
                    lb_wdata <= mem_rd_data;
                    r_rx     <= r_rx + 10'd1;
                    if (r_rx == H_WORDS_C - 10'd1) rst_q <= R_IDLE;
                end
            end
            R_ZERO: begin
                // Кадра ещё нет — строка чёрная
                lb_we    <= 1'b1;
                lb_addr  <= {r_bank, r_rx[8:0]};
                lb_wdata <= '0;
                r_rx     <= r_rx + 10'd1;
                if (r_rx == H_WORDS_C - 10'd1) rst_q <= R_IDLE;
            end
            default: rst_q <= R_IDLE;
            endcase
        end
    end

    // =====================================================================
    // Арбитр: по очереди, если просят оба. Владельца операции несёт метка.
    // =====================================================================
    reg last_writer;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) last_writer <= 1'b0;
        else if (mem_ack) last_writer <= !mem_op_tag;
    end
    wire sel_reader = r_req && (!w_req || last_writer);
    assign mem_req  = r_req | w_req;
    assign mem_tag  = sel_reader;
    assign mem_we   = !sel_reader;
    assign mem_addr = sel_reader ? r_addr : w_addr;

endmodule
