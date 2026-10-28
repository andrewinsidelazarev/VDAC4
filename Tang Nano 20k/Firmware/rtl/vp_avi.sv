// SPDX-License-Identifier: Apache-2.0
//
// Ядро видео: разбор потока AVI (MJPEG) из медиа-FIFO, по байту за такт. Плагин FTVIEW4 шлёт
// файл, как штатному CMD_PLAYVIDEO FT812: заголовок (RIFF, LIST hdrl, avih …), затем чанки movi;
// при перемотке — заголовок и чанки с места перемотки (avi_restart).
//
// Чанки читаются подряд, вложенность не нужна: RIFF и LIST — только заголовок и тип (4 байта),
// дальше их содержимое идёт теми же чанками. Остальные чанки:
//   avih     — первое слово, мкс на кадр (us_frame), остальное пропускается;
//   00dc, 00db — кадр JPEG (поток 0 — видео: плагин берёт только такие ролики): байты — в
//            декодер (jpeg_core) словами по 4, младший первым; конец чанка — неполное слово
//            (байты без строба декодер берёт нулями: после EOI они безвредны). Пустой чанк
//            (0 байтов) — повтор кадра: слот темпа без кадра;
//   01wb — звук (поток 1 — первая звуковая дорожка: её формат знает VIDEO.ELF): при включённом
//            звуке (aud_en) байты идут в порт звука (au_*, с обратным давлением: FIFO звука полон —
//            разбор ждёт), иначе пропускаются;
//   прочие (JUNK, idx1, strh, strf, с 1.38 — и другие дорожки: 02wb …) — пропускаются.
// Нечётный размер чанка добивается байтом выравнивания.
//
// Кадр начинается только когда запись предыдущего кадра (vp_fbwrite) свободна: frame_start —
// перед первым байтом чанка, frame_fed — когда последний байт ушёл в декодер. Пустой чанк тоже
// ждёт свободную запись (предыдущий кадр объявлен), затем просит свой слот (slot_req) и ждёт
// slot_go — у кадров и повторов один порядок.
//
// skip (сторож vp_core: декодер встал) — недоданное слово выбрасывается, остаток чанка и байт
// выравнивания пропускаются; декодер к этому времени сбрасывается.
//
// Кадр без DHT (камеры, «AVI1») декодируется стандартными таблицами K.3, как у FT812: маркеры
// кадра проходятся по длинам до SOS; если DHT не было, перед байтом DA маркера SOS (его FF уже
// в декодере) вставляются 420 байтов vp_dht_std.svh — сегмент DHT без своего FF и FF для DA.
//
module vp_avi (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        clr,             // сброс разбора (REG_CPURESET): дальше — поток с начала
    input  wire        skip,            // импульс: кадр битый (сторож) — остаток чанка пропустить

    // поток из медиа-FIFO
    input  wire        in_valid,
    input  wire [7:0]  in_byte,
    output wire        in_ready,

    // слова JPEG → jpeg_core (держатся до j_accept)
    output wire        j_valid,
    output wire [31:0] j_data,
    output wire [3:0]  j_strb,
    input  wire        j_accept,

    // кадры
    input  wire        wr_busy,         // vp_fbwrite: кадр в работе (до объявления)
    output reg         frame_start,     // импульс: перед первым байтом чанка кадра
    output reg         frame_fed,       // импульс: последний байт чанка — в декодере
    output reg         slot_req,        // пустой чанк ждёт своего слота темпа
    input  wire        slot_go,         //   слот прошёл (vp_pace)
    output reg  [31:0] us_frame,        // avih: мкс на кадр (0 — заголовка ещё не было)
    output reg  [15:0] n_dht,           // кадров со вставленными таблицами (для отладки)

    // звук: байты чанков 01wb (aud_en — берёт их приложение ESP)
    input  wire        aud_en,
    output wire        au_valid,
    output wire [7:0]  au_byte,
    input  wire        au_ready
);
    `include "vp_dht_std.svh"

    // --- чанки ----------------------------------------------------------------------------------
    localparam logic [3:0] P_HDR = 4'd0, P_FORM = 4'd1, P_AVIH = 4'd2, P_SKIP = 4'd3,
                           P_VWAIT = 4'd4, P_VDATA = 4'd5, P_VFLUSH = 4'd6, P_EMPTY = 4'd7,
                           P_DEC = 4'd8, P_ADATA = 4'd9;
    reg [3:0]  pst;
    reg [63:0] hdr;                      // 8 байтов заголовка чанка, первый — в [7:0]
    reg [2:0]  hcnt;
    reg [31:0] left;                     // сколько байтов чанка осталось
    reg        vpad;                     // нечётный размер чанка: после него байт выравнивания
    reg [2:0]  acnt;                     // avih: байт первого слова (4 — слово прочитано)

    // Заголовок разбирается тактом позже, из регистра (P_DEC): сравнения fourcc и размер — не
    // в одном такте с приходом восьмого байта (путь не укладывался в 61,875 МГц)
    wire [63:0] hdr_n = {in_byte, hdr[63:8]};
    wire [31:0] fcc   = hdr[31:0];
    wire [31:0] csize = hdr[63:32];
    wire is_cont  = fcc == 32'h4646_4952 || fcc == 32'h5453_494C;          // RIFF, LIST
    wire is_avih  = fcc == 32'h6869_7661;                                   // avih
    // до 1.38 — любые NN: вторая звуковая дорожка (02wb) шла бы в FIFO звука вперемешку с первой
    wire is_video = fcc[23:0] == 24'h64_3030 &&
                    (fcc[31:24] == 8'h63 || fcc[31:24] == 8'h62);           // 00dc, 00db
    wire is_audio = fcc == 32'h6277_3130;                                   // 01wb

    // --- маркеры JPEG до SOS --------------------------------------------------------------------
    localparam logic [2:0] J_PRE = 3'd0, J_FF = 3'd1, J_LENH = 3'd2, J_LENL = 3'd3, J_SEG = 3'd4,
                           J_ENT = 3'd5;
    reg [2:0]  jst;
    reg        seen_dht;
    reg [7:0]  len_hi;
    reg [15:0] seg_left;
    reg        inj;                      // идёт вставка таблиц (байт DA ждёт на входе)
    reg [8:0]  inj_i;
    wire       inj_start = pst == P_VDATA && jst == J_FF && in_valid && in_byte == 8'hDA &&
                           !seen_dht && !inj;

    // --- упаковка в слова -----------------------------------------------------------------------
    // Слова уходят в декодер через очередь на два: готовность упаковки (и разбора, и адрес
    // чтения FIFO) не зависит от приёма декодера в том же такте — путь «слово → разбор маркеров
    // jpeg_input → приём → готовность → адрес FIFO» не укладывался в такт 61,875 МГц. Декодер
    // берёт слово за 4 такта, упаковка даёт не больше — очередь его не тормозит.
    reg [23:0] pk_acc;
    reg [1:0]  pk_n;
    reg [31:0] oq_d [0:1];
    reg [3:0]  oq_s [0:1];
    reg [1:0]  oq_n;
    assign j_valid = oq_n != 2'd0;
    assign j_data  = oq_d[0];
    assign j_strb  = oq_s[0];
    wire       oq_room  = oq_n != 2'd2;
    wire       pk_ready = pk_n != 2'd3 || oq_room;
    // байт в упаковку: вставка таблиц или байт кадра
    wire       pk_take_in = pst == P_VDATA && in_valid && !inj && !inj_start && pk_ready;
    wire       pk_take    = pk_take_in || (inj && pk_ready);
    wire [7:0] pk_byte    = inj ? vp_dht_byte(inj_i) : in_byte;

    // звук — прямо в порт: байт уходит, когда порт его берёт
    assign au_valid = pst == P_ADATA && in_valid;
    assign au_byte  = in_byte;

    assign in_ready = (pst == P_HDR) || (pst == P_FORM) || (pst == P_AVIH) || (pst == P_SKIP) ||
                      pk_take_in || (pst == P_ADATA && au_ready);
    wire   take = in_valid && in_ready;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            pst <= P_HDR; hdr <= '0; hcnt <= '0; left <= '0; vpad <= 1'b0; acnt <= '0;
            frame_start <= 1'b0; frame_fed <= 1'b0; slot_req <= 1'b0; us_frame <= '0; n_dht <= '0;
            jst <= J_PRE; seen_dht <= 1'b0; len_hi <= '0; seg_left <= '0; inj <= 1'b0; inj_i <= '0;
            pk_acc <= '0; pk_n <= '0; oq_n <= '0;
            oq_d[0] <= '0; oq_d[1] <= '0; oq_s[0] <= '0; oq_s[1] <= '0;
        end else if (clr) begin
            pst <= P_HDR; hcnt <= '0; left <= '0; vpad <= 1'b0; acnt <= '0;
            frame_start <= 1'b0; frame_fed <= 1'b0; slot_req <= 1'b0;
            jst <= J_PRE; seen_dht <= 1'b0; inj <= 1'b0; inj_i <= '0;
            pk_n <= '0; oq_n <= '0;
        end else if (skip && (pst == P_VDATA || pst == P_VFLUSH)) begin
            // битый кадр: недоданное — в никуда, остаток чанка — пропуском
            frame_start <= 1'b0; frame_fed <= 1'b0;
            inj <= 1'b0; pk_n <= '0; oq_n <= '0;
            if (pst == P_VDATA) begin
                pst <= P_SKIP;                         // остаток, затем байт выравнивания (vpad)
            end else if (vpad) begin
                pst <= P_SKIP; left <= 32'd1; vpad <= 1'b0;
            end else
                pst <= P_HDR;
        end else begin
            frame_start <= 1'b0;
            frame_fed   <= 1'b0;

            // ---- упаковка: полное слово (или хвост чанка — неполное) — в очередь
            begin : pack
                logic        push, pop;
                logic [31:0] wd;
                logic [3:0]  ws;
                push = 1'b0; wd = '0; ws = '0;
                pop  = j_valid && j_accept;
                if (pk_take) begin
                    if (pk_n == 2'd3) begin
                        push = 1'b1; wd = {pk_byte, pk_acc}; ws = 4'hF; pk_n <= '0;
                    end else begin
                        case (pk_n)
                            2'd0:    pk_acc[7:0]   <= pk_byte;
                            2'd1:    pk_acc[15:8]  <= pk_byte;
                            default: pk_acc[23:16] <= pk_byte;
                        endcase
                        pk_n <= pk_n + 2'd1;
                    end
                end else if (pst == P_VFLUSH && pk_n != 2'd0 && oq_room) begin
                    push = 1'b1;
                    wd   = {8'h00, pk_n == 2'd3 ? pk_acc[23:16] : 8'h00,
                            pk_n >= 2'd2 ? pk_acc[15:8] : 8'h00, pk_acc[7:0]};
                    ws   = pk_n == 2'd3 ? 4'h7 : pk_n == 2'd2 ? 4'h3 : 4'h1;
                    pk_n <= '0;
                end
                // очередь: снять голову, дописать хвост
                if (pop) begin
                    oq_d[0] <= oq_d[1]; oq_s[0] <= oq_s[1];
                end
                if (push) begin
                    if (oq_n == 2'd0 || (oq_n == 2'd1 && pop)) begin
                        oq_d[0] <= wd; oq_s[0] <= ws;
                    end else begin
                        oq_d[1] <= wd; oq_s[1] <= ws;
                    end
                end
                oq_n <= oq_n + {1'b0, push} - {1'b0, pop};
            end

            // ---- вставка таблиц
            if (inj_start) begin
                inj <= 1'b1; inj_i <= '0; n_dht <= n_dht + 16'd1;
            end else if (inj && pk_ready) begin
                if (inj_i == 9'(VP_DHT_LEN - 1)) begin
                    inj <= 1'b0; seen_dht <= 1'b1;   // байт DA пройдёт следующим как обычный
                end
                inj_i <= inj_i + 9'd1;
            end

            // ---- маркеры кадра
            if (pk_take_in) begin
                case (jst)
                    J_PRE: if (in_byte == 8'hFF) jst <= J_FF;
                    J_FF: begin
                        if (in_byte == 8'hFF)
                            jst <= J_FF;                       // заполнитель
                        else if (in_byte == 8'hD8) begin
                            jst <= J_PRE; seen_dht <= 1'b0;    // SOI
                        end else if (in_byte == 8'h00 || in_byte == 8'h01 || in_byte == 8'hD9 ||
                                 (in_byte >= 8'hD0 && in_byte <= 8'hD7))
                            jst <= J_PRE;                      // без длины
                        else if (in_byte == 8'hDA)
                            jst <= J_ENT;                      // SOS: дальше данные — как есть
                        else begin
                            if (in_byte == 8'hC4) seen_dht <= 1'b1;
                            jst <= J_LENH;
                        end
                    end
                    J_LENH: begin len_hi <= in_byte; jst <= J_LENL; end
                    J_LENL: begin
                        seg_left <= {len_hi, in_byte} - 16'd2;
                        jst <= ({len_hi, in_byte} <= 16'd2) ? J_PRE : J_SEG;
                    end
                    J_SEG: begin
                        seg_left <= seg_left - 16'd1;
                        if (seg_left == 16'd1) jst <= J_PRE;
                    end
                    default: ;                                 // J_ENT
                endcase
            end

            // ---- чанки
            case (pst)
                P_HDR: if (take) begin
                    hdr  <= hdr_n;
                    hcnt <= hcnt + 3'd1;
                    if (hcnt == 3'd7) pst <= P_DEC;
                end
                P_DEC: begin                           // заголовок чанка — в hdr
                    vpad <= csize[0];
                    left <= csize;
                    if (is_cont) begin
                        pst <= P_FORM; left <= 32'd4; vpad <= 1'b0;
                    end else if (is_avih) begin
                        pst <= P_AVIH; acnt <= '0;
                    end else if (is_video) begin
                        pst <= csize == 32'd0 ? P_EMPTY : P_VWAIT;
                    end else if (is_audio && aud_en && csize != 32'd0) begin
                        pst <= P_ADATA;
                    end else begin
                        pst <= csize == 32'd0 ? P_HDR : P_SKIP;
                    end
                end
                P_FORM, P_SKIP, P_ADATA: if (take) begin
                    left <= left - 32'd1;
                    if (left == 32'd1) begin
                        if (vpad) begin left <= 32'd1; vpad <= 1'b0; pst <= P_SKIP; end
                        else pst <= P_HDR;
                    end
                end
                P_AVIH: if (take) begin
                    left <= left - 32'd1;
                    if (left == 32'd1) begin
                        if (vpad) begin left <= 32'd1; vpad <= 1'b0; pst <= P_SKIP; end
                        else pst <= P_HDR;
                    end
                    case (acnt)                                // первое слово — мкс на кадр
                        3'd0: us_frame[7:0]   <= in_byte;
                        3'd1: us_frame[15:8]  <= in_byte;
                        3'd2: us_frame[23:16] <= in_byte;
                        3'd3: us_frame[31:24] <= in_byte;
                        default: ;
                    endcase
                    if (acnt != 3'd4) acnt <= acnt + 3'd1;
                end
                P_VWAIT: if (!wr_busy) begin
                    frame_start <= 1'b1;
                    jst <= J_PRE; seen_dht <= 1'b0;
                    pst <= P_VDATA;
                end
                P_VDATA: if (pk_take_in) begin
                    left <= left - 32'd1;
                    if (left == 32'd1) pst <= P_VFLUSH;
                end
                P_VFLUSH: if (pk_n == 2'd0 && oq_n == 2'd0) begin
                    frame_fed <= 1'b1;
                    if (vpad) begin pst <= P_SKIP; left <= 32'd1; vpad <= 1'b0; end
                    else pst <= P_HDR;
                end
                P_EMPTY: begin
                    if (!wr_busy) slot_req <= 1'b1;
                    if (slot_req && slot_go) begin slot_req <= 1'b0; pst <= P_HDR; end
                end
                default: pst <= P_HDR;
            endcase
        end
    end
endmodule
