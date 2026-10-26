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
// Раскладка буфера: адрес слова = BASE + буфер × BUF_WORDS + {строка[9:0], слово[8:0]},
// одно слово — два пикселя, строка до 1024 пикселей. Буферы лежат подряд, каждый —
// V_ACTIVE строк по 512 слов (720×576 — 1,125 МБ, три — 3,375 МБ); ниже BASE в SDRAM —
// RAM_G и шрифты FT812 (ts_fb_core).
//
// Выборка строк. Домен вывода заранее (за две строки) просит строку L: переключает
// req_toggle, а req_line/req_first/req_two/req_ft держит неизменными до следующего запроса.
// Строка читается запросами по 8 пакетов (64 слова, sdram_burst) в четверть bank = L[1:0]
// четырёхстрочного буфера fb_line_ram; req_two — сначала строка L − 1, потом L (при
// уменьшении картинки строка вывода может задеть две новые строки источника).
//
// FRAME BLEND (AVG = 1, avg_on): выводится среднее двух последних целых кадров — latest и
// кадра перед ним (prev), точка в точку. Демо, которые чередуют кадры A и B (мерцание 50 Гц:
// гигаскрин, лишние цвета), дают ровный цвет (A + B) / 2; повтор входного кадра (48,83 → 50 Гц)
// чередования не сбивает — пара та же. Буферов четыре: читатель на начало выходного кадра
// берёт пару R = latest, RP = prev, писатель пишет в буфер вне {R, RP, latest, prev}. Если за
// один выходной кадр опубликованы два входных и свободного буфера нет, писатель пишет поверх
// prev, тот перестаёт годиться (have_prev = 0), и следующий выходной кадр — без смешения.
// Строка пары читается дважды: сначала строка RP — во временный буфер (t_mem), потом строка R;
// к каждому слову R из t_mem берётся слово RP того же места, и точки, которые в двух кадрах
// различаются, смешиваются по таблице fb_avg_rom (среднее по яркости, по каналам);
// одинаковые проходят как есть. Смешанная точка пишется как {PAL_SEL = 1, B, G, R} — развёртка
// и палитра её не отличают. Буфер 3 — с BUF3_BASE (в ядре без FT812 — место RAM_G, оно свободно).
// Запись строчного буфера — через две стадии (смешение), с AVG = 0 — тоже: на такт позже прежнего.
//
// РЕЖИМ FT812 (ft_mode, при M_SEL = 1). Запись Evo стоит: FIFO захвата сливается, кадры
// Evo в SDRAM не пишутся — их место занимают два кадра FT812 растеризатора (ft_render):
// кадры 0 и 1 — с BASE + b·FT_BUF_WORDS, кадр 2 — FT_T_LO / FT_T_HI (две половины строк),
// строка — 512 слов (до 1024 точек RGB565). Растеризатор
// объявляет готовый кадр (ft_pub_v, ft_pub_buf, ft_words — слов в строке); выходной кадр,
// запрошенный с req_ft, читается из объявленного на его начало — ft_front, ft_showing.
// Растеризатор пишет следующий кадр в другой буфер, только когда ft_front = объявленному:
// читатель его уже не трогает. После выхода из режима FT кадров Evo нет (их место занято) —
// до первого нового кадра Evo выход чёрный.
//
module fb_core #(
    parameter int H_ACTIVE = 720,           // кратно 16: строка = целое число пакетов
    parameter int V_ACTIVE = 576,
    parameter logic [20:0] BASE = 21'h000000,  // адрес слова буфера 0
    parameter logic [20:0] FT_BUF_WORDS = 21'h060000, // кадр FT812: 768 строк × 512 слов
    parameter logic [20:0] FT_T_LO = 21'h080000,      // кадр FT 2 (ft_render): строки 0…511
    parameter logic [20:0] FT_T_HI = 21'h0C9000,      //                        строки 512…767
    parameter bit          AVG = 1'b0,                // FRAME BLEND: четвёртый буфер и смешение пар
    parameter logic [20:0] BUF3_BASE = 21'h000000     // AVG: адрес слова буфера 3
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
    input  wire        req_two,              // сначала строка req_line − 1
    input  wire        req_ft,               // выходной кадр — из кадров FT812
    input  wire        avg_on,               // AVG: выводить среднее двух кадров (FRAME BLEND)

    // Режим FT812 (M_SEL = 1) и кадры растеризатора
    input  wire        ft_mode,
    input  wire        ft_pub_v,             // есть готовый кадр FT812
    input  wire [1:0]  ft_pub_buf,           // его буфер (0…2)
    input  wire [9:0]  ft_words,             // слов в его строке (1…512)
    output reg  [1:0]  ft_front,             // буфер FT, который сейчас выводится
    output reg         ft_showing,           // выводится кадр FT812

    // Запись в двухстрочный буфер вывода
    output reg         lb_we,
    output reg  [10:0] lb_addr,              // {строка[1:0], слово[8:0]}
    output reg  [31:0] lb_wdata,

    // Клиентская сторона sdram_burst
    output wire        mem_req,
    output wire        mem_we,
    output wire [20:0] mem_addr,
    output wire        mem_tag,              // 1 — читатель, 0 — писатель
    output wire [2:0]  mem_len,              // пакетов в запросе − 1
    input  wire        mem_ack,
    input  wire        mem_ack_tag,          // метка клиента: у ack, у wr_take, у rd_valid (мост
    input  wire        mem_wr_tag,           // sd_bridge ядра FT812 выдаёт их порознь: в одном такте
    input  wire        mem_rd_tag,           // ack одной операции и слова другой)
    input  wire        mem_wr_take,
    input  wire [5:0]  mem_wr_idx,           // слово в запросе записи (до 8 пакетов)
    output wire [31:0] mem_wdata,
    input  wire        mem_rd_valid,
    input  wire [31:0] mem_rd_data,

    // Состояние (для OSD, отладки и теста)
    output reg  [15:0] frames_written,       // опубликовано целых кадров
    output reg  [15:0] frames_rejected,      // отброшено сбойных
    output reg         fetch_late,           // липкий: новый запрос строки до окончания старого
    output reg         have_frame,           // есть хотя бы один целый кадр
    output wire        avg_shown             // выводимый кадр — среднее двух (AVG)
);
    localparam int H_WORDS  = H_ACTIVE / 2;
    localparam int H_BURSTS = H_ACTIVE / 16;
    localparam [9:0] H_WORDS_C  = H_WORDS;
    localparam [6:0] H_BURSTS_C = H_BURSTS;
    localparam [9:0] V_LAST_C   = V_ACTIVE - 1;
    localparam logic [20:0] BUF_WORDS = 21'(V_ACTIVE * 512);

    function automatic logic [20:0] buf_base(input logic [1:0] b);
        case (b)
            2'd0:    buf_base = BASE;
            2'd1:    buf_base = BASE + BUF_WORDS;
            2'd2:    buf_base = BASE + 21'(2 * BUF_WORDS);
            default: buf_base = BUF3_BASE;                  // только AVG
        endcase
    endfunction
    localparam [9:0] V_ACTIVE_C = V_ACTIVE;

    initial begin
        if (H_ACTIVE % 16 != 0) $error("fb_core: H_ACTIVE должен быть кратен 16");
        if (H_ACTIVE > 1024)    $error("fb_core: H_ACTIVE не больше 1024");
    end

    // =====================================================================
    // Тройная буферизация (AVG — четверная). Инвариант: w_buf не равен ни r_buf, ни latest,
    // при AVG — ни r_prev, ни prev (пока have_prev); r_prev = r_buf, если кадр не смешивается.
    // =====================================================================
    reg [1:0] w_buf, r_buf, latest;
    reg [1:0] prev, r_prev;             // AVG: кадр перед latest; вторая половина выводимой пары
    reg       have_prev;                // prev — целый кадр сразу перед latest
    reg       r_avg;                    // выводимый кадр — среднее r_buf и r_prev
    reg       publish;                  // писатель закончил целый кадр
    reg       reader_switch;            // читатель начинает новый выходной кадр
    wire      avg_take = AVG && avg_on && have_prev;
    assign    avg_shown = r_avg;

    // Буфер вне четырёх занятых (младший из свободных); [2] — свободный нашёлся
    function automatic [2:0] free_buf(input [1:0] a, input [1:0] b, input [1:0] c, input [1:0] d);
        free_buf = 3'b000;
        for (int k = 3; k >= 0; k = k - 1)
            if (2'(k) != a && 2'(k) != b && 2'(k) != c && 2'(k) != d) free_buf = {1'b1, 2'(k)};
    endfunction

    reg ft_mode_d;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            w_buf      <= 2'd0;
            r_buf      <= 2'd1;
            r_prev     <= 2'd1;
            latest     <= 2'd2;
            prev       <= 2'd3;
            have_frame <= 1'b0;
            have_prev  <= 1'b0;
            r_avg      <= 1'b0;
            ft_mode_d  <= 1'b0;
        end else begin : tb_update
            logic [1:0] r_next, rp_next;
            logic [2:0] f;
            r_next  = (reader_switch && have_frame) ? latest : r_buf;
            rp_next = (reader_switch && have_frame) ? (avg_take ? prev : latest) : r_prev;
            r_buf  <= r_next;
            r_prev <= rp_next;
            if (reader_switch && have_frame) r_avg <= avg_take;
            ft_mode_d <= ft_mode;
            if (publish) begin
                latest     <= w_buf;
                have_frame <= 1'b1;
                if (AVG) begin
                    prev      <= latest;
                    have_prev <= have_frame;
                    f = free_buf(r_next, rp_next, w_buf, latest);
                    if (f[2]) begin
                        w_buf <= f[1:0];
                    end else begin
                        // Заняты все четыре (два кадра за выходной кадр): поверх нового prev
                        w_buf     <= latest;
                        have_prev <= 1'b0;
                    end
                end else begin
                    w_buf <= 2'd3 - w_buf - r_next;     // третий, ни L, ни R
                end
            end
            if (ft_mode) begin
                have_frame <= 1'b0;                     // кадры Evo затёрты кадрами FT812
                have_prev  <= 1'b0;
            end
        end
    end

    // =====================================================================
    // Писатель: FIFO → запросы до 8 пакетов (64 слова) в одной строке SDRAM
    // =====================================================================
    // С 1.32 писатель, как и читатель, пишет пачкой: кусок строки до 64 слов (с кратного 64:
    // строку SDRAM в 256 слов запрос не пересекает) — одним запросом ~72 такта. Прежде он писал
    // по пакету (8 слов, ~14 тактов) и набирал следующий только после него; между его ходами
    // читатель занимал шину на целый запрос, и при FRAME BLEND (чтение вдвое) писателю на 720p
    // доставалось ~4 млн слов/с при нужных ~7 млн во время строки — FIFO захвата переполнялся.
    typedef enum logic [1:0] { W_FETCH, W_DATA, W_BURST } wstate_t;
    wstate_t   wst;
    reg [9:0]  w_word;                  // слов строки уже принято
    reg [9:0]  w_y;                     // текущая строка
    reg [9:0]  w_y_exp;                 // какую строку ждём следующей
    reg [9:0]  w_lines;                 // целых строк в текущем кадре
    reg        w_frame_ok;
    reg        w_line_closed;           // предыдущая строка записана целиком
    reg        w_complete;              // все строки кадра записаны, ждём конец кадра
    reg        w_req;
    reg [20:0] w_addr;
    reg [2:0]  w_len;                   // пакетов в запросе − 1
    reg        w_sent;                  // запрос принят контроллером (был ack)

    // Буфер куска строки — BSRAM 64 × 32. Слово для контроллера читается заранее: адрес —
    // следующее слово запроса (после взятого wr_idx — wr_idx + 1), поэтому в такт wr_take на
    // выходе уже нужное слово (контроллер берёт слова подряд, wdata — в тот же такт).
    reg         w_we;
    reg  [5:0]  w_wa;
    reg  [31:0] w_wd;
    reg  [5:0]  w_next;                 // следующее слово запроса, пока контроллер не берёт
    reg  [31:0] wbuf_q;
    wire        w_take = mem_wr_take && !mem_wr_tag && wst == W_BURST;
    wire [5:0]  w_ra   = w_take ? mem_wr_idx + 6'd1 : w_next;
    (* ram_style = "block" *) reg [31:0] wbuf [0:63];
    always_ff @(posedge clk) begin
        if (w_we) wbuf[w_wa] <= w_wd;
        wbuf_q <= wbuf[w_ra];
    end
    assign mem_wdata = wbuf_q;

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
            w_len           <= '0;
            w_sent          <= 1'b0;
            w_we            <= 1'b0;
            w_wa            <= '0;
            w_wd            <= '0;
            w_next          <= '0;
            publish         <= 1'b0;
            frames_written  <= '0;
            frames_rejected <= '0;
        end else begin
            fifo_rd <= 1'b0;
            publish <= 1'b0;
            w_we    <= 1'b0;
            if (w_take) w_next <= mem_wr_idx + 6'd1;
            case (wst)
            W_FETCH: begin
                if (!fifo_empty) begin
                    fifo_rd <= 1'b1;
                    wst     <= W_DATA;
                end
            end
            W_DATA: if (!fifo_rd) begin        // данные FIFO пришли (fifo_rd был тактом раньше)
                wst <= W_FETCH;
                if (ft_mode) begin
                    // Режим FT812: захват сливается, кадр Evo не собирается
                    w_complete    <= 1'b0;
                    w_frame_ok    <= 1'b0;
                    w_lines       <= '0;
                    w_line_closed <= 1'b0;
                end else if (fifo_q[32] && fifo_q[11]) begin
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
                    w_we   <= 1'b1;
                    w_wa   <= w_word[5:0];
                    w_wd   <= fifo_q[31:0];
                    w_word <= w_word + 10'd1;
                    // кусок до 64 слов набран (или строка кончилась) — один запрос
                    if (w_word[5:0] == 6'd63 || w_word == H_WORDS_C - 10'd1) begin
                        w_req  <= 1'b1;
                        w_sent <= 1'b0;
                        w_len  <= w_word[5:3];
                        w_addr <= buf_base(w_buf) + {2'b00, w_y, w_word[8:6], 6'b000000};
                        w_next <= 6'd0;
                        wst    <= W_BURST;
                    end
                end
            end
            W_BURST: begin
                if (mem_ack && !mem_ack_tag) begin
                    w_req  <= 1'b0;
                    w_sent <= 1'b1;
                end
                // Буфер свободен, когда контроллер забрал последнее слово запроса
                if (w_take && mem_wr_idx == {w_len, 3'b111}) begin
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
    reg        pend_first, pend_two, pend_ft;

    typedef enum logic [1:0] { R_IDLE, R_FETCH, R_ZERO } rstate_t;
    rstate_t   rst_q;
    reg        r_req;
    reg [20:0] r_addr;
    reg [9:0]  r_line;
    reg [1:0]  r_bank;
    reg        r_valid;                 // выводимый кадр действителен (иначе чёрный)
    reg [6:0]  r_burst;                 // пакетов запрошено
    reg [2:0]  r_len;                   // пакетов в запросе − 1
    reg [9:0]  r_rx;                    // слов принято / записано нулей
    reg        r_ft;                    // выходной кадр — FT812
    reg        r_second;                // после этой строки — ещё одна (req_two)
    reg        r_pass;                  // AVG: строка кадра RP уже в t_mem, читается строка R
    wire       r_ppass = r_avg && !r_ft && !r_pass;    // читается строка RP — в t_mem
    reg [9:0]  ft_words_q;              // слов в строке выводимого кадра FT812
    // Стадия A записи строчного буфера: слово кадра R, слово RP того же места (t_mem), смешивать ли
    reg        s_we, s_avg;
    reg [10:0] s_addr;
    reg [31:0] s_l, s_p;
    // Запись временного буфера строки RP
    reg        t_we;
    reg [8:0]  t_wa;
    reg [31:0] t_wd;
    wire [31:0] t_q;
    wire [9:0] r_words  = r_ft ? ft_words_q : H_WORDS_C;
    wire [6:0] r_bursts = r_ft ? 7'((ft_words_q + 10'd7) >> 3) : H_BURSTS_C;
    wire [6:0] r_left   = r_bursts - r_burst;
    // кадр FT b: база половины строк (0…511 / 512…767) — буфер 2 лежит двумя кусками
    function automatic logic [20:0] ft_lbase(input logic [1:0] b, input logic hi);
        case (b)
            2'd0:    ft_lbase = BASE + (hi ? 21'h040000 : 21'd0);
            2'd1:    ft_lbase = BASE + FT_BUF_WORDS + (hi ? 21'h040000 : 21'd0);
            default: ft_lbase = hi ? FT_T_HI : FT_T_LO;
        endcase
    endfunction
    wire [20:0] r_base  = buf_base(r_buf);

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            tog_sync      <= '0;
            req_pending   <= 1'b0;
            pend_line     <= '0;
            pend_first    <= 1'b0;
            pend_two      <= 1'b0;
            pend_ft       <= 1'b0;
            rst_q         <= R_IDLE;
            r_req         <= 1'b0;
            r_addr        <= '0;
            r_line        <= '0;
            r_bank        <= '0;
            r_valid       <= 1'b0;
            r_burst       <= '0;
            r_len         <= '0;
            r_rx          <= '0;
            r_ft          <= 1'b0;
            r_second      <= 1'b0;
            r_pass        <= 1'b0;
            ft_words_q    <= 10'd512;
            ft_front      <= 2'd0;
            ft_showing    <= 1'b0;
            reader_switch <= 1'b0;
            s_we          <= 1'b0;
            s_avg         <= 1'b0;
            s_addr        <= '0;
            s_l           <= '0;
            s_p           <= '0;
            t_we          <= 1'b0;
            t_wa          <= '0;
            t_wd          <= '0;
            fetch_late    <= 1'b0;
        end else begin
            reader_switch <= 1'b0;
            s_we          <= 1'b0;
            t_we          <= 1'b0;

            // Переход запроса из домена вывода. req_line/req_first к моменту смены
            // синхронизированного переключателя стабильны уже больше строки.
            tog_sync <= {tog_sync[1:0], req_toggle};
            if (tog_sync[2] ^ tog_sync[1]) begin
                if (req_pending || rst_q != R_IDLE) fetch_late <= 1'b1;
                req_pending <= 1'b1;
                pend_line   <= req_line;
                pend_first  <= req_first;
                pend_two    <= req_two;
                pend_ft     <= req_ft;
            end

            case (rst_q)
            R_IDLE: if (r_second) begin
                // вторая строка запроса req_two
                r_second <= 1'b0;
                r_line   <= r_line + 10'd1;
                r_bank   <= r_bank + 2'd1;
                r_burst  <= '0;
                r_rx     <= '0;
                r_pass   <= 1'b0;
                if (r_valid) rst_q <= R_FETCH; else rst_q <= R_ZERO;
            end else if (req_pending && !(tog_sync[2] ^ tog_sync[1])) begin
                req_pending <= 1'b0;
                r_line   <= pend_two ? pend_line - 10'd1 : pend_line;
                r_bank   <= pend_two ? pend_line[1:0] - 2'd1 : pend_line[1:0];
                r_second <= pend_two;
                r_burst  <= '0;
                r_rx     <= '0;
                r_pass   <= 1'b0;
                if (pend_first) begin
                    r_ft <= pend_ft;
                    if (pend_ft) begin
                        // Кадр FT812: тот, что объявлен сейчас
                        ft_front   <= ft_pub_buf;
                        ft_showing <= ft_pub_v;
                        ft_words_q <= ft_words;
                        r_valid    <= ft_pub_v;
                        if (ft_pub_v) rst_q <= R_FETCH; else rst_q <= R_ZERO;
                    end else begin
                        // Новый выходной кадр Evo: берём последний готовый (сам выбор — в
                        // блоке тройной буферизации, r_buf обновится к следующему такту)
                        ft_showing    <= 1'b0;
                        reader_switch <= 1'b1;
                        r_valid       <= have_frame;
                        if (have_frame) rst_q <= R_FETCH; else rst_q <= R_ZERO;
                    end
                end else begin
                    if (r_valid) rst_q <= R_FETCH; else rst_q <= R_ZERO;
                end
            end
            R_FETCH: begin
                // r_buf и ft_front уже учли переключение (прошёл минимум такт). Запрос — до
                // 8 пакетов; строка начинается с 512 слов, запросы — с кратных 64 словам,
                // поэтому строку SDRAM (256 слов) запрос не пересекает.
                if (!r_req && r_burst != r_bursts && !reader_switch) begin
                    r_req  <= 1'b1;
                    r_len  <= r_left >= 7'd8 ? 3'd7 : 3'(r_left - 7'd1);
                    r_addr <= r_ft ? ft_lbase(ft_front, r_line[9]) + {3'b000, r_line[8:0], r_burst[5:0], 3'b000}
                                   : (r_ppass ? buf_base(r_prev) : r_base) + {2'b00, r_line, r_burst[5:0], 3'b000};
                end
                if (r_req && mem_ack && mem_ack_tag) begin
                    r_req   <= 1'b0;
                    r_burst <= r_burst + 7'(r_len) + 7'd1;
                end
                if (mem_rd_valid && mem_rd_tag) begin
                    // слова сверх строки (последний пакет неполный) не пишутся
                    if (r_ppass) begin
                        t_we <= r_rx < r_words;
                        t_wa <= r_rx[8:0];
                        t_wd <= mem_rd_data;
                    end else begin
                        s_we   <= r_rx < r_words;
                        s_addr <= {r_bank, r_rx[8:0]};
                        s_l    <= mem_rd_data;
                        s_p    <= t_q;
                        s_avg  <= r_avg && !r_ft;
                    end
                    r_rx <= r_rx + 10'd1;
                    if (r_rx == {r_bursts, 3'b000} - 10'd1) begin
                        if (r_ppass) begin
                            // строка RP — в t_mem; теперь та же строка кадра R
                            r_pass  <= 1'b1;
                            r_burst <= '0;
                            r_rx    <= '0;
                        end else begin
                            rst_q <= R_IDLE;
                        end
                    end
                end
            end
            R_ZERO: begin
                // Кадра ещё нет — строка чёрная
                s_we   <= 1'b1;
                s_addr <= {r_bank, r_rx[8:0]};
                s_l    <= '0;
                s_avg  <= 1'b0;
                r_rx   <= r_rx + 10'd1;
                if (r_rx == r_words - 10'd1) rst_q <= R_IDLE;
            end
            default: rst_q <= R_IDLE;
            endcase
        end
    end

    // =====================================================================
    // Смешение пары кадров (AVG) и запись строчного буфера
    // =====================================================================
    // Стадия A — регистры s_* и выход ПЗУ (адрес — по словам, пришедшим из SDRAM и t_mem);
    // стадия B — точка: смешанная {PAL_SEL = 1, B, G, R}, если кадры в ней различаются, иначе — как есть
    wire [29:0] mix_q;                  // {B, G, R} точки 1, {B, G, R} точки 0 — по 5 бит
    generate if (AVG) begin : g_avg
        // Временный буфер строки кадра RP. Адрес чтения — слово, которое придёт следующим:
        // к приходу слова r_rx кадра R на выходе t_q — слово r_rx кадра RP
        (* ram_style = "block" *) reg [31:0] t_mem [0:511];
        reg  [31:0] t_q_r;
        wire [8:0]  t_ra = r_rx[8:0] + {8'd0, mem_rd_valid && mem_rd_tag && rst_q == R_FETCH};
        always_ff @(posedge clk) begin
            if (t_we) t_mem[t_wa] <= t_wd;
            t_q_r <= t_mem[t_ra];
        end
        assign t_q = t_q_r;

        // Порт k = 3·точка + канал (0 R, 1 G, 2 B): {PAL_SEL и канал кадра R, то же кадра RP}
        wire [71:0] rom_a;
        for (genvar k = 0; k < 6; k = k + 1) begin : g_a
            localparam int P = (k / 3) * 16, C = (k % 3) * 5;
            assign rom_a[k*12 +: 12] = {mem_rd_data[P + 15], mem_rd_data[P + C +: 5],
                                        t_q_r[P + 15], t_q_r[P + C +: 5]};
        end
        fb_avg_rom #(.N(6)) rom (.clk(clk), .addr(rom_a), .q(mix_q));
    end else begin : g_no_avg
        assign t_q   = '0;
        assign mix_q = '0;
    end endgenerate

    function automatic [15:0] mix_px(input avg, input [15:0] l, input [15:0] p, input [14:0] q);
        mix_px = (avg && l != p) ? {1'b1, q} : l;
    endfunction

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            lb_we    <= 1'b0;
            lb_addr  <= '0;
            lb_wdata <= '0;
        end else begin
            lb_we    <= s_we;
            lb_addr  <= s_addr;
            lb_wdata <= {mix_px(s_avg, s_l[31:16], s_p[31:16], mix_q[29:15]),
                         mix_px(s_avg, s_l[15:0],  s_p[15:0],  mix_q[14:0])};
        end
    end

    // =====================================================================
    // Арбитр: по очереди, если просят оба. Владельца операции несёт метка.
    // =====================================================================
    reg last_writer;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) last_writer <= 1'b0;
        else if (mem_ack) last_writer <= !mem_ack_tag;
    end
    wire sel_reader = r_req && (!w_req || last_writer);
    assign mem_req  = r_req | w_req;
    assign mem_tag  = sel_reader;
    assign mem_we   = !sel_reader;
    assign mem_addr = sel_reader ? r_addr : w_addr;
    assign mem_len  = sel_reader ? r_len : w_len;

endmodule
