// SPDX-License-Identifier: Apache-2.0
//
// Ядро видео (MJPEG AVI на месте FT812): хост — плагин FTVIEW4 на Evo по SPI FT812 (ft_spi,
// события START/WRITE/NEXT), домен clk = clk_mem.
//
// Для хоста ядро — FT812 с одним медиа-FIFO: плагин шлёт файл AVI так же, как для штатного
// CMD_PLAYVIDEO (avi_feed: DMA блока в окно FIFO, затем REG_MEDIAFIFO_WRITE), только без команд
// сопроцессора. Адреса (FT81x):
//   0x302000 REG_ID             чтение — 0x7C (проверка плагина, как у FT812);
//   0x302020 REG_CPURESET       бит 0 — сброс потока: FIFO пуст (указатели 0), разбор, декодер,
//                               темп — с начала; пока 1, поток стоит (перемотка: 1, затем 0);
//                               бит 2 — пауза: темп стоит, кадр на экране не меняется (у FT812 —
//                               сброс аудиодвижка, плагин пишет его только ядру видео);
//   0x309014 REG_MEDIAFIFO_READ  чтение — позиция чтения FIFO (байт, по модулю размера);
//   0x309018 REG_MEDIAFIFO_WRITE запись — позиция записи (берётся по старшему байту, слово
//                               целиком); чтение — она же (уже записанная в SDRAM);
//   0x0E0000…+FIFO_BYTES       окно данных FIFO (2 МБ: 0x0E0000…0x2DFFFF): плагин пишет блоки
//                               по позиции записи, подряд по кольцу, словами (длина кратна 4);
//   0x000000…0x00003F          блок состояния (чтение, 32-битные слова; слово защёлкивается
//                               при чтении младшего байта):
//       0 «VID1» (0x31444956)   1 FIFO_BYTES        2 слотов темпа с сброса (номер кадра ролика,
//                                                     с повторами — время для панели)
//       3 объявлено кадров      4 отменено кадров (битые; сторож)
//       5 {высота, ширина} последнего кадра          6 мкс на кадр (avih)
//       7 {кадров со вставленными DHT[15:0], 10'b0, запись не подряд, приёмник переполнялся,
//          пауза, FIFO пуст, запись занята, сброс}
// 32-битные значения FIFO читаются и пишутся целиком: READ защёлкивается при чтении младшего
// байта, WRITE применяется при записи старшего.
//
// МЕДИА-FIFO (1.38) — 2 МБ в SDRAM (FIFO_BASE, место RAM_G FT812 — в этом ядре её нет). До 1.38
// он был 32 КБ в BSRAM — около одного кадра MJPEG (40–130 мс): любая пауза подачи с карты Evo
// (шаг фоновой таблицы перемотки, задержка карты, проход FAT) опустошала его, и картинка
// вставала (на плате 27.10.2026 — до 0,35 с, «тормозит на некоторых видео»). Теперь запас —
// секунды. Байты хоста собираются в слова и ложатся в приёмник (stg, 512 слов, один BSRAM);
// оттуда в SDRAM — пакетами от sw до конца группы из 8 слов (слова группы вне — маской 0).
// Запись REG_MEDIAFIFO_WRITE объявляет позицию не сразу: сперва в SDRAM уходят все принятые до
// неё слова (и неполная группа) — иначе разбор прочёл бы слово, которого там ещё нет. Приёмник
// держит 2 КБ — ~0,6 мс SPI на 28 МГц; разъём SDRAM освобождается много чаще, а если приёмник
// всё же полон, слово теряется и ставится флаг (слово 7). Чтение — выборкой наперёд (pf, 32
// слова) пакетами от fw до конца группы или до объявленной позиции; разбору — байт за байтом.
//
// Поток: FIFO → vp_avi (чанки, кадры, avih) → jpeg_core → vp_fbwrite (кадры FT в SDRAM) →
// объявление по vp_pace (темп по выходным кадрам vtick).
//
// ЗВУК для приложения ESP (LINK, регистры и окно — в ts_fb_core; значения ESP приходят сюда уже в
// домене clk). Байты чанков NNwb, пока ESP включил приём (au_en), упаковываются по 4 (младший —
// первым) в FIFO звука — 1024 слова (4 КБ), запись — clk, чтение — окном LINK по SCK ESP
// (au_rd_*). au_wp — сколько слов записано, au_rp — сколько ESP прочитал (оба по модулю 2048);
// FIFO полон — разбор ждёт (звук не теряется). Сброс потока (REG_CPURESET) обнуляет au_wp и
// увеличивает эпоху au_epoch; пока ESP не подтвердит её (au_ack_epoch: сбросил свой указатель),
// звук ждёт — иначе старый au_rp ESP испортил бы счёт. aclk_en, aclk_us — часы звука для темпа
// кадров (vp_pace).
//
// Сторож: кадр в работе (запись занята, кадр не готов) не продвигается WD_CYCLES тактов — нет
// ни байта в декодер, ни точки из него, хотя данные есть (или кадр уже весь подан) — кадр битый
// (обрыв, неподдержанный JPEG: прогрессивный, DRI): декодер сбрасывается, кадр отменяется,
// остаток чанка пропускается; на экране остаётся прежний кадр. Пустой FIFO — не сбой (поток
// стоит: пауза, медленная подача).
//
`default_nettype none   // неявный провод — ошибка (1.38: d_accept без объявления выбросил декодер)
module vp_core #(
    parameter int FIFO_AW   = 19,              // FIFO: 2^FIFO_AW слов SDRAM по 4 байта (19 — 2 МБ)
    parameter logic [20:0] FIFO_BASE = 21'h000000,  // его начало в SDRAM (адрес слова)
    parameter int OUT_US    = 20_000,          // выходной кадр, мкс (720p50)
    parameter int WD_CYCLES = 1 << 22          // сторож, тактов clk (~68 мс на 61,875 МГц)
) (
    input  wire         clk,
    input  wire         rst_n,

    // хост: события ft_spi
    input  wire         spi_tg,
    input  wire [1:0]   spi_kind,
    input  wire [7:0]   spi_h0,
    input  wire [7:0]   spi_h1,
    input  wire [7:0]   spi_b,
    output reg  [7:0]   spi_rd_byte,

    input  wire         vtick,                 // импульс: начало выходного кадра (домен clk)

    // SDRAM: медиа-FIFO (запись и чтение) и запись кадров — клиент sdram_burst, пакет 8 слов;
    // метка — снаружи (запись 101, чтение 100)
    output wire         s_req,
    output wire         s_we,
    output wire [20:0]  s_addr,
    output wire [2:0]   s_len,
    input  wire         s_ack,
    input  wire         s_wr_take,
    output wire [31:0]  s_wdata,
    output wire [3:0]   s_wmask,
    input  wire         s_rd_valid,
    input  wire [31:0]  s_rd_data,

    // fb_core: кадры FT
    input  wire         fb_showing,
    input  wire [1:0]   fb_front,
    output wire         pub_v,
    output wire [1:0]   pub_buf,
    output wire [9:0]   pub_words,
    output wire [10:0]  pub_w,
    output wire [9:0]   pub_h,

    // состояние (LINK, отладка)
    output wire [31:0]  st_slots,
    output reg  [31:0]  st_frames,
    output reg  [15:0]  st_aborts,
    output wire         st_pause,           // пауза (REG_CPURESET бит 2): для ESP — пауза звука

    // звук (см. выше)
    input  wire         au_en,
    input  wire [7:0]   au_ack_epoch,
    input  wire [10:0]  au_rp,
    output reg  [10:0]  au_wp,
    output reg  [7:0]   au_epoch,
    input  wire         aclk_en,
    input  wire [31:0]  aclk_us,
    input  wire         au_rd_clk,
    input  wire [9:0]   au_rd_addr,
    output reg  [31:0]  au_rd_q
);
    localparam int FB = FIFO_AW + 2;                    // разрядов байтовой позиции
    localparam logic [31:0] FIFO_BYTES = 32'd1 << FB;
    localparam logic [22:0] WIN_LO = 23'h0E0000;        // окно данных FIFO у хоста
    localparam logic [22:0] WIN_HI = WIN_LO + 23'(FIFO_BYTES);
    localparam int SAW = 9;                             // приёмник: 2^SAW слов
    localparam int PAW = 5;                             // выборка наперёд: 2^PAW слов

    // --- события хоста -----------------------------------------------------------------------
    reg [2:0] tg_s;
    always_ff @(posedge clk) tg_s <= {tg_s[1:0], spi_tg};
    wire ev = tg_s[2] ^ tg_s[1];

    localparam logic [1:0] H_IDLE = 2'd0, H_READ = 2'd1, H_WRITE = 2'd2;
    reg  [1:0]  h_phase;
    reg  [21:0] h_addr;

    // --- медиа-FIFO в SDRAM (см. начало файла) --------------------------------------------------
    reg  [FB-1:0] rd_pos, wr_pos;                       // взято разбором; объявлено (уже в SDRAM)
    reg  [23:0]   wr_sh;                                // REG_MEDIAFIFO_WRITE: младшие три байта
    reg           creset;
    reg           pause;                                // REG_CPURESET бит 2: темп стоит
    assign        st_pause = pause;
    wire          fifo_empty = wr_pos == rd_pos;

    // приёмник: слово — по старшему байту (плагин пишет словами с позиции, кратной 4)
    wire          in_win = {1'b0, h_addr} >= WIN_LO && {1'b0, h_addr} < WIN_HI;
    wire          w_fifo = ev && spi_kind == 2'd1 && h_phase == H_WRITE && in_win;
    wire [FB-1:0] w_off  = FB'({1'b0, h_addr} - WIN_LO);
    wire          w_word = w_fifo && h_addr[1:0] == 2'd3 && !creset;
    (* ram_style = "block" *) reg [31:0] stg [0:(1 << SAW) - 1];
    reg  [SAW+1:0] stg_wp, stg_rp;                      // слов принято; ушло в SDRAM (по модулю 2^(SAW+2))
    wire [SAW+1:0] stg_n = stg_wp - stg_rp;
    wire           stg_full = stg_n[SAW];               // 2^SAW слов
    reg  [23:0]   w_acc;                                // байты 0…2 слова
    reg  [FIFO_AW-1:0] sw;                              // слово SDRAM (от FIFO_BASE) для stg[stg_rp]
    reg  [FIFO_AW-1:0] sw_tail;                         // куда ляжет следующее принятое
    reg           ovf, seq_err;                         // приёмник переполнялся; запись не подряд
    always_ff @(posedge clk)
        if (w_word && !stg_full) stg[stg_wp[SAW-1:0]] <= {spi_b, w_acc};

    // объявление позиции записи: ждёт, пока в SDRAM уйдут все слова, принятые до него
    reg           pend_v;
    reg  [FB-1:0] pend_pos;
    reg  [SAW+1:0] pend_wp;                             // stg_wp при записи регистра
    wire [SAW+1:0] pend_rem = pend_wp - stg_rp;         // слов до него не в SDRAM (< 0 — уже)
    wire          pend_done = pend_v && (pend_rem == '0 || pend_rem[SAW+1]);

    // выборка наперёд
    (* ram_style = "block" *) reg [31:0] pf [0:(1 << PAW) - 1];
    reg  [PAW:0]  pf_wp, pf_rp;
    wire [PAW:0]  pf_n = pf_wp - pf_rp;
    reg  [FIFO_AW-1:0] fw;                              // следующее слово SDRAM для выборки
    wire [FIFO_AW-1:0] avail = wr_pos[FB-1:2] - fw;     // объявлено, но не выбрано

    // пакет: слова [s, s + n) группы из 8 — маска
    function automatic [7:0] vmask(input [2:0] s, input [3:0] n);
        vmask = 8'(((16'd1 << n) - 16'd1) << s);
    endfunction
    // W — приёмник: от sw до конца группы, если столько принято; пока ждёт объявление — сколько есть
    wire [3:0]    w_end  = 4'd8 - {1'b0, sw[2:0]};
    wire          w_full = {{(SAW-2){1'b0}}, w_end} <= stg_n;
    wire          w_want = !creset && (w_full || (pend_v && stg_n != '0));
    wire [3:0]    w_n    = w_full ? w_end : stg_n[3:0];
    // R — выборка: от fw до конца группы или до объявленной позиции, если в ней есть место на пакет
    wire [3:0]    r_end  = 4'd8 - {1'b0, fw[2:0]};
    wire          r_full = avail >= FIFO_AW'(r_end);
    wire          r_want = !creset && avail != '0 && pf_n <= (PAW+1)'((1 << PAW) - 8);
    wire [3:0]    r_n    = r_full ? r_end : avail[3:0];

    // разъём SDRAM: приёмник (W), выборка (R) и запись кадров (F, vp_fbwrite) — по одному
    // запросу; кому отдан, тот ведёт и 8 слов своего пакета. Приёмник — первым (хост не ждёт),
    // затем выборка (её ждёт разбор), кадры — последними.
    localparam logic [1:0] P_NONE = 2'd0, P_W = 2'd1, P_R = 2'd2, P_F = 2'd3;
    reg  [1:0]    p_own;
    reg           p_data;                               // после ack: идут слова пакета
    reg  [2:0]    p_cnt;                                // слово пакета
    reg  [20:0]   p_addr;
    reg  [7:0]    p_vm;                                 // слова пакета из приёмника / в выборку
    reg  [3:0]    p_n;
    reg           p_kill;                               // сброс потока посреди пакета: итог не брать
    wire          f_req;
    wire [20:0]   f_addr;
    wire [31:0]   f_wdata;
    wire          p_ev  = p_data && (p_own == P_R ? s_rd_valid : s_wr_take);
    assign s_req   = p_own != P_NONE && !p_data && (p_own != P_F || f_req);
    assign s_we    = p_own != P_R;
    assign s_addr  = p_own == P_F ? f_addr : p_addr;
    assign s_len   = 3'd0;
    assign s_wmask = (p_own == P_W && !p_vm[p_cnt]) ? 4'h0 : 4'hF;

    // данные пакета W: адрес приёмника — наперёд (регистровое чтение BSRAM); первое wr_take —
    // через такт после ack
    reg  [SAW+1:0] s_ra;
    reg  [31:0]   sq;
    wire          w_take = p_own == P_W && p_data && s_wr_take && p_vm[p_cnt];
    wire [SAW+1:0] s_ra_n = (p_own == P_W && !p_data && s_ack) ? stg_rp : w_take ? s_ra + 1'b1 : s_ra;
    always_ff @(posedge clk) begin
        s_ra <= s_ra_n;
        sq   <= stg[s_ra_n[SAW-1:0]];
    end
    assign s_wdata = p_own == P_F ? f_wdata : sq;
    // данные пакета R — в выборку
    wire          r_take = p_own == P_R && p_data && s_rd_valid && p_vm[p_cnt] && !p_kill && !creset;
    always_ff @(posedge clk)
        if (r_take) pf[pf_wp[PAW-1:0]] <= s_rd_data;

    // байты разбору: слово из выборки (регистровое чтение, адрес наперёд) — в регистр, из него —
    // по байту; pq_ok: слово по адресу было в выборке ещё до чтения
    reg  [31:0]   pq;
    reg           pq_ok;
    reg           ob_v;
    reg  [31:0]   ob_w;
    reg  [1:0]    ob_b;
    wire          ld      = !ob_v && pq_ok && !creset;
    wire [PAW:0]  pf_rp_n = ld ? pf_rp + 1'b1 : pf_rp;
    always_ff @(posedge clk) pq <= pf[pf_rp_n[PAW-1:0]];
    wire          a_valid = ob_v && !creset;
    wire          a_ready;
    wire          a_take  = a_valid && a_ready;
    wire [7:0]    a_byte  = ob_w[8*ob_b +: 8];

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            stg_wp <= '0; stg_rp <= '0; w_acc <= '0; sw <= '0; sw_tail <= '0; ovf <= 1'b0; seq_err <= 1'b0;
            pf_wp <= '0; pf_rp <= '0; fw <= '0; pq_ok <= 1'b0; ob_v <= 1'b0; ob_w <= '0; ob_b <= '0;
            p_own <= P_NONE; p_data <= 1'b0; p_cnt <= '0; p_addr <= '0; p_vm <= '0; p_n <= '0; p_kill <= 1'b0;
        end else begin
            // приём
            if (w_fifo && h_addr[1:0] != 2'd3) w_acc[8*h_addr[1:0] +: 8] <= spi_b;
            if (w_word) begin
                if (stg_full) ovf <= 1'b1;
                else begin
                    stg_wp  <= stg_wp + 1'b1;
                    sw_tail <= w_off[FB-1:2] + 1'b1;
                    if (w_off[FB-1:2] != sw_tail) seq_err <= 1'b1;
                end
            end

            // разъём
            if (p_own == P_NONE) begin
                p_kill <= 1'b0;
                p_cnt  <= '0;
                if (w_want) begin
                    p_own <= P_W; p_addr <= FIFO_BASE + 21'({sw[FIFO_AW-1:3], 3'd0});
                    p_vm  <= vmask(sw[2:0], w_n); p_n <= w_n;
                end else if (r_want) begin
                    p_own <= P_R; p_addr <= FIFO_BASE + 21'({fw[FIFO_AW-1:3], 3'd0});
                    p_vm  <= vmask(fw[2:0], r_n); p_n <= r_n;
                end else if (f_req)
                    p_own <= P_F;
            end else if (!p_data) begin
                if (s_ack) p_data <= 1'b1;
            end else if (p_ev) begin
                p_cnt <= p_cnt + 3'd1;
                if (p_cnt == 3'd7) begin
                    p_own  <= P_NONE;
                    p_data <= 1'b0;
                    if (!p_kill && !creset) begin
                        if (p_own == P_W) begin stg_rp <= stg_rp + (SAW+2)'(p_n); sw <= sw + FIFO_AW'(p_n); end
                        if (p_own == P_R) fw <= fw + FIFO_AW'(p_n);
                    end
                end
            end
            if (r_take) pf_wp <= pf_wp + 1'b1;

            // разбору
            pq_ok <= (pf_wp - pf_rp_n) != '0;
            pf_rp <= pf_rp_n;
            if (ld) begin
                ob_v <= 1'b1; ob_w <= pq; ob_b <= 2'd0;
            end else if (a_take) begin
                ob_b <= ob_b + 2'd1;
                if (ob_b == 2'd3) ob_v <= 1'b0;
            end

            // сброс потока: всё с начала; пакет в пути доходит до конца, но не засчитывается
            if (creset) begin
                stg_wp <= '0; stg_rp <= '0; sw <= '0; sw_tail <= '0; ovf <= 1'b0; seq_err <= 1'b0;
                pf_wp <= '0; pf_rp <= '0; fw <= '0; pq_ok <= 1'b0; ob_v <= 1'b0;
                if (p_own != P_NONE && !(p_ev && p_cnt == 3'd7)) p_kill <= 1'b1;
            end
        end
    end

    // --- поток ---------------------------------------------------------------------------------
    wire        j_valid, j_accept;
    wire [31:0] j_data;
    wire [3:0]  j_strb;
    wire        frame_start, frame_fed, slot_req, slot_go;
    wire        au_valid, au_ready;                     // звук из разбора (ниже)
    wire [7:0]  au_byte;
    wire [31:0] us_frame;
    wire [15:0] n_dht;
    wire        wr_busy, frame_ready, pub_go;
    reg         wd_abort;                               // сторож: отменить кадр
    reg  [2:0]  dec_rst;                                // сброс декодера — несколько тактов

    vp_avi avi (
        .clk(clk), .rst_n(rst_n), .clr(creset), .skip(wd_abort),
        .in_valid(a_valid), .in_byte(a_byte), .in_ready(a_ready),
        .j_valid(j_valid), .j_data(j_data), .j_strb(j_strb), .j_accept(j_accept),
        .wr_busy(wr_busy || dec_rst != 3'd0), .frame_start(frame_start), .frame_fed(frame_fed),
        .slot_req(slot_req), .slot_go(slot_go), .us_frame(us_frame), .n_dht(n_dht),
        .aud_en(au_en), .au_valid(au_valid), .au_byte(au_byte), .au_ready(au_ready)
    );

    // --- звук: упаковка в слова и FIFO для ESP -----------------------------------------------------
    reg  [23:0] au_acc;
    reg  [1:0]  au_n;
    (* ram_style = "block" *) reg [31:0] au_mem [0:1023];
    wire        au_full  = (au_wp - au_rp) == 11'd1024;
    assign      au_ready = au_ack_epoch == au_epoch && !(au_n == 2'd3 && au_full);
    wire        au_take  = au_valid && au_ready;
    always_ff @(posedge clk) begin
        if (au_take && au_n == 2'd3)
            au_mem[au_wp[9:0]] <= {au_byte, au_acc};
    end
    always_ff @(posedge au_rd_clk) au_rd_q <= au_mem[au_rd_addr];
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            au_acc <= '0; au_n <= '0; au_wp <= '0;
        end else if (creset) begin
            au_n <= '0; au_wp <= '0;
        end else if (au_take) begin
            case (au_n)
                2'd0: au_acc[7:0]   <= au_byte;
                2'd1: au_acc[15:8]  <= au_byte;
                2'd2: au_acc[23:16] <= au_byte;
                default: au_wp <= au_wp + 11'd1;
            endcase
            au_n <= au_n + 2'd1;
        end
    end

    wire        d_valid, dec_idle;
    wire [15:0] d_x, d_y, img_w, img_h;
    wire [7:0]  d_r, d_g, d_b;
    wire [1:0]  img_mode;
    reg  [1:0]  sk_n;                                    // точек в очереди (ниже)
    // объявлен до декодера: Gowin иначе берёт неявный провод без драйвера и выбрасывает декодер
    wire        sk_flush = wd_abort || creset || dec_rst != 3'd0;
    wire        d_accept = sk_n != 2'd2 && !sk_flush;
    jpeg_core #(.SUPPORT_WRITABLE_DHT(1)) dec (
        .clk_i(clk), .rst_i(!rst_n || dec_rst != 3'd0),
        .inport_valid_i(j_valid), .inport_data_i(j_data), .inport_strb_i(j_strb),
        .inport_last_i(1'b0), .inport_accept_o(j_accept),
        .outport_accept_i(d_accept), .outport_valid_o(d_valid),
        .outport_width_o(img_w), .outport_height_o(img_h),
        .outport_pixel_x_o(d_x), .outport_pixel_y_o(d_y),
        .outport_pixel_r_o(d_r), .outport_pixel_g_o(d_g), .outport_pixel_b_o(d_b),
        .idle_o(dec_idle), .outport_mode_o(img_mode)
    );

    // точки декодера → запись кадра через очередь на 2 точки (1.38): приём декодера — от счётчика
    // очереди, а не от сравнения плиток vp_fbwrite. Путь «точка → плитка → готовность → счётчики
    // выхода декодера» не укладывался в такт (clk_mem 57 МГц): внутри декодера от приёма — ~10 нс.
    // Отмена кадра и сброс очередь опустошают.
    reg  [55:0] sk0, sk1;                                // {x, y, R, G, B}
    wire        d_take   = d_valid && d_accept;
    wire        px_ready;
    wire        px_valid = sk_n != 2'd0 && !sk_flush;
    wire        f_take   = px_valid && px_ready;
    wire [15:0] px_x = sk0[55:40], px_y = sk0[39:24];
    wire [7:0]  px_r = sk0[23:16], px_g = sk0[15:8], px_b = sk0[7:0];
    wire [55:0] d_px = {d_x, d_y, d_r, d_g, d_b};
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            sk_n <= 2'd0; sk0 <= '0; sk1 <= '0;
        end else if (sk_flush) begin
            sk_n <= 2'd0;
        end else begin
            case ({d_take, f_take})
                2'b10: begin
                    if (sk_n == 2'd0) sk0 <= d_px; else sk1 <= d_px;
                    sk_n <= sk_n + 2'd1;
                end
                2'b01: begin
                    sk0 <= sk1;
                    sk_n <= sk_n - 2'd1;
                end
                2'b11: begin                             // одна в очереди: она уходит, приходит новая
                    sk0 <= d_px;
                end
                default: ;
            endcase
        end
    end

    vp_fbwrite wr (
        .clk(clk), .rst_n(rst_n),
        .frame_start(frame_start), .frame_end(dec_idle && dec_rst == 3'd0), .abort(wd_abort || creset),
        .mode(img_mode), .img_w(img_w), .img_h(img_h),
        .px_valid(px_valid), .px_ready(px_ready), .px_x(px_x), .px_y(px_y),
        .px_r(px_r), .px_g(px_g), .px_b(px_b),
        .s_req(f_req), .s_we(), .s_addr(f_addr), .s_len(), .s_ack(s_ack && p_own == P_F && !p_data),
        .s_wr_take(s_wr_take && p_own == P_F && p_data), .s_wdata(f_wdata),
        .fb_showing(fb_showing), .fb_front(fb_front),
        .pub_v(pub_v), .pub_buf(pub_buf), .pub_words(pub_words), .pub_w(pub_w), .pub_h(pub_h),
        .frame_ready(frame_ready), .pub_go(pub_go), .busy(wr_busy)
    );

    wire go;
    vp_pace #(.OUT_US(OUT_US)) pace (
        .clk(clk), .rst_n(rst_n), .clr(creset), .vtick(vtick), .pause(pause), .us_frame(us_frame),
        .want(frame_ready || slot_req), .aclk_en(aclk_en), .aclk_us(aclk_us),
        .go(go), .slots(st_slots)
    );
    assign pub_go  = go && frame_ready;
    assign slot_go = go && !frame_ready;

    // --- сторож ----------------------------------------------------------------------------------
    localparam int WDW = $clog2(WD_CYCLES + 1);
    reg [WDW-1:0] wd_cnt;
    reg           fed;                                   // чанк кадра подан целиком
    // прогресс — и слово записи кадра в SDRAM: стирание холста (до ~0,8 млн тактов у 1024×768) — не
    // простой (записи медиа-FIFO — нет: иначе подача хоста держала бы битый кадр вечно).
    // Тактом позже: приём декодера комбинационный, и путь до счётчика сторожа не укладывался в такт
    reg  progress;
    always_ff @(posedge clk)
        progress <= j_valid && j_accept || d_take || f_take || frame_start || s_wr_take && p_own == P_F;
    wire starving = !fed && fifo_empty && !ob_v;         // ждём данных — не сбой
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            wd_cnt <= '0; fed <= 1'b0; wd_abort <= 1'b0; dec_rst <= 3'd0;
            st_frames <= '0; st_aborts <= '0;
        end else begin
            wd_abort <= 1'b0;
            if (dec_rst != 3'd0) dec_rst <= dec_rst - 3'd1;
            if (frame_start) fed <= 1'b0;
            if (frame_fed)   fed <= 1'b1;
            if (pub_go) st_frames <= st_frames + 32'd1;
            if (creset) begin
                wd_cnt <= '0; fed <= 1'b0; dec_rst <= 3'd7;
            end else if (!wr_busy || frame_ready || progress || starving) begin
                wd_cnt <= '0;
            end else if (wd_cnt == WDW'(WD_CYCLES)) begin
                wd_cnt    <= '0;
                wd_abort  <= 1'b1;
                dec_rst   <= 3'd7;
                st_aborts <= st_aborts + 16'd1;
            end else begin
                wd_cnt <= wd_cnt + 1'b1;
            end
        end
    end

    // --- регистры хоста -------------------------------------------------------------------------
    function automatic [31:0] stat_word(input [3:0] i);
        case (i)
            4'd0: stat_word = 32'h3144_4956;                       // «VID1»
            4'd1: stat_word = FIFO_BYTES;
            4'd2: stat_word = st_slots;
            4'd3: stat_word = st_frames;
            4'd4: stat_word = {16'd0, st_aborts};
            4'd5: stat_word = {6'd0, pub_h, 5'd0, pub_w};
            4'd6: stat_word = us_frame;
            4'd7: stat_word = {n_dht, 10'd0, seq_err, ovf, pause, fifo_empty, wr_busy, creset};
            default: stat_word = 32'd0;
        endcase
    endfunction

    reg [31:0] rd_latch;                                 // 32-битное значение чтения
    // байт чтения по адресу a (lat — защёлка слова при младшем байте)
    function automatic [7:0] pick(input [31:0] w, input [1:0] b);
        pick = w[8*b +: 8];
    endfunction

    reg        rd_v;                                     // такт после START/NEXT: ответить
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            h_phase <= H_IDLE; h_addr <= '0; spi_rd_byte <= 8'h00; rd_v <= 1'b0;
            rd_pos <= '0; wr_pos <= '0; wr_sh <= '0; creset <= 1'b0; pause <= 1'b0; rd_latch <= '0; au_epoch <= '0;
            pend_v <= 1'b0; pend_pos <= '0; pend_wp <= '0;
        end else begin
            rd_v <= 1'b0;
            if (a_take) rd_pos <= rd_pos + 1'b1;
            // позиция записи — разбору, когда слова до неё уже в SDRAM
            if (pend_done) begin
                wr_pos <= pend_pos;
                pend_v <= 1'b0;
            end

            if (ev) begin
                case (spi_kind)
                    2'd0: begin                                   // заголовок
                        h_addr <= {spi_h0[5:0], spi_h1, spi_b};
                        spi_rd_byte <= 8'h00;
                        case (spi_h0[7:6])
                            2'b00: begin h_phase <= H_READ; rd_v <= 1'b1; end
                            2'b10: h_phase <= H_WRITE;
                            default: h_phase <= H_IDLE;           // команды хоста — не нужны
                        endcase
                    end
                    2'd1: if (h_phase == H_WRITE) begin           // байт записи
                        if (h_addr == 22'h302020) begin
                            creset <= spi_b[0];
                            pause  <= spi_b[2];
                            if (spi_b[0]) begin
                                rd_pos <= '0; wr_pos <= '0; pend_v <= 1'b0;
                                if (!creset) au_epoch <= au_epoch + 8'd1;   // новый поток — новая эпоха звука
                            end
                        end
                        if (h_addr[21:2] == 20'(22'h309018 >> 2)) begin
                            case (h_addr[1:0])
                                2'd0: wr_sh[7:0]   <= spi_b;
                                2'd1: wr_sh[15:8]  <= spi_b;
                                2'd2: wr_sh[23:16] <= spi_b;
                                default: if (!creset) begin
                                    pend_pos <= FB'({spi_b, wr_sh});
                                    pend_wp  <= stg_wp;
                                    pend_v   <= 1'b1;
                                end
                            endcase
                        end
                        h_addr <= h_addr + 22'd1;
                    end
                    2'd2: if (h_phase == H_READ) begin            // следующий байт чтения
                        h_addr <= h_addr + 22'd1;
                        rd_v   <= 1'b1;
                    end
                    default: ;
                endcase
            end

            // ответ: байт по h_addr (уже обновлённому) — до следующей границы байта SPI
            if (rd_v) begin
                if (h_addr < 22'h000040) begin
                    if (h_addr[1:0] == 2'd0) begin
                        rd_latch    <= stat_word(h_addr[5:2]);
                        spi_rd_byte <= pick(stat_word(h_addr[5:2]), 2'd0);
                    end else
                        spi_rd_byte <= pick(rd_latch, h_addr[1:0]);
                end else if (h_addr == 22'h302000)
                    spi_rd_byte <= 8'h7C;
                else if (h_addr == 22'h302020)
                    spi_rd_byte <= {5'd0, pause, 1'b0, creset};
                else if (h_addr[21:2] == 20'(22'h309014 >> 2)) begin
                    if (h_addr[1:0] == 2'd0) begin
                        rd_latch    <= 32'(rd_pos);
                        spi_rd_byte <= pick(32'(rd_pos), 2'd0);
                    end else
                        spi_rd_byte <= pick(rd_latch, h_addr[1:0]);
                end else if (h_addr[21:2] == 20'(22'h309018 >> 2))
                    spi_rd_byte <= pick(32'(wr_pos), h_addr[1:0]);
                else
                    spi_rd_byte <= 8'h00;
            end
        end
    end
endmodule
`default_nettype wire
