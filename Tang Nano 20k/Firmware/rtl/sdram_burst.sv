// SPDX-License-Identifier: Apache-2.0
//
// Контроллер встроенной SDRAM GW2AR-18 (64 Мбит, 32 бит: 4 банка × 2048 строк × 256 слов)
// с пакетами по 8 слов (32 байта); запрос — до 8 пакетов подряд в одной строке (64 слова).
//
// Зачем свой, а не third_party/sdram/sdram.v: тот побайтовый и без пакетов — 5 тактов
// на слово, около 50 МБ/с на 66 МГц. Кадровому буферу 576p50 нужно ~82 МБ/с
// (запись 48,83 кадра/с + чтение 50 кадров/с по 720×576×2 байта), 1024×768 — больше.
// Пакет из 8 слов с автопредзарядом занимает ~14 тактов, это ~2,3 байта/такт; запрос из
// 8 пакетов подряд (команды READ/WRITE через 8 тактов без закрытия строки) — ~72 такта,
// ~3,6 байта/такт: так читается и пишется кадр 1024×768.
//
// Адрес — номер 32-битного слова: {строка[10:0], банк[1:0], столбец[7:0]}.
// Пакет всегда выровнен на 8 слов (addr[2:0] игнорируются); запрос из req_len + 1 пакетов
// не должен пересекать строку (256 слов) — за этим следит клиент.
//
// Интерфейс клиента:
//   req/we/addr/req_len/req_tag держатся до ack. op_tag повторяет метку принятого запроса
//   на всё время операции — по ней арбитр раздаёт ack, wr_take и rd_valid.
//   После ack для записи контроллер 8·(req_len + 1) тактов подряд выставляет wr_take=1 и
//   номер слова в запросе wr_idx — клиент в тот же такт отдаёт wdata и wmask
//   (комбинационно из своего буфера; wmask — какие байты слова писать, через DQM). Для
//   чтения приходят 8·(req_len + 1) импульсов rd_valid подряд с rd_idx и rd_data. Следующий
//   запрос можно подавать сразу после ack.
//
// Упрощения: одна операция за раз (банки не чередуются), строка закрывается автопредзарядом
// последнего пакета запроса.
//
// Клок микросхемы — инверсия клока логики (верхний модуль): команды и данные записи
// меняются на фронте логики и устойчивы на фронте микросхемы через полтакта.
//
// Выборка данных чтения (rd_phase, меняется на лету). Пусть E0 — фронт логики, на котором
// выставлена команда READ. Микросхема видит её через полтакта (E0,5) и при CL=2 выдаёт
// слово 0 через tAC после E1,5; держит его до E2,5 + tOH. По паспорту (tAC ≤ 6 нс,
// tOH ≥ 2,5 нс, такт 14,8 нс) слово 0 действительно примерно с E0+28 до E0+40 нс, а
// задержки клока и данных на кристалле сдвигают это окно позже на неизвестные единицы нс.
// Поэтому точка выборки слова 0 выбирается из четырёх с шагом в полтакта:
//   0 — фронт E2 (29,6 нс)   1 — спад E2,5 (37,0 нс)   2 — фронт E3 (44,4 нс)   3 — спад E3,5 (51,8 нс)
// Слово i берётся на i тактов позже. По умолчанию 1; рабочую точку находит калибровка
// (fb_ui) по испытательной таблице, её можно сменить вручную в экранном меню. При CL=3 (ядро
// FT812: контроллер на 123,75 МГц, CL=2 там паспортом не обещан) все точки — на такт позже.
//
// IOB_CAP = 1 (ядро FT812 с 1.38, вместе с sdram_train ниже): клок микросхемы — не инверсия клока
// логики, а CLKOUTP той же PLL со сдвигом, который подбирает sdram_train. DQ чтения берёт регистр
// dq_q по фронту clk прямо с вывода, без логики перед ним: он встаёт в блок ввода-вывода (IREG), у всех
// 32 линий одна задержка (у прежней выборки по фронту перед регистром стоял выбор точки, и каждая
// линия шла своей разводкой). Тонкая точка выборки — сдвиг клока, а rd_phase — только такт: слово 0
// — из dq_q, взятого на фронте E(CL + rd_phase), как у ядра ZX Next на этой же SDRAM (регистр у вывода
// один, по фронту). IDDR (и по спаду) пробовался: с ним в блок вывода не встают выходные регистры DQ
// (слова записи, OREG) — каждая линия записи шла бы своей разводкой (сборка 27.10.2026).
//
module sdram_burst #(
    parameter int TAGW      = 1,       // ширина метки клиента
    parameter int CLK_KHZ   = 67_500,
    parameter bit IOB_CAP   = 1'b0,    // 1 — выборка регистром у вывода, rd_phase — такт (см. выше)
    parameter bit EARLY     = 1'b0,    // 1 — следующая операция через BL + tRP после последнего READ
                                       //     (слова прошлой ещё идут — их операция в rd_tag)
    parameter int CL        = 2,       // CAS latency, записывается в mode register
    parameter int T_RCD     = 2,       // ACTIVE → READ/WRITE
    parameter int T_RP      = 2,       // PRECHARGE → ACTIVE
    parameter int T_WR      = 2,       // последнее слово записи → PRECHARGE
    parameter int T_RFC     = 5,       // AUTO REFRESH → следующая команда
    parameter int INIT_US   = 200,     // пауза после включения питания
    parameter int REFI_NS   = 7_800    // период регенерации: 8192 строк за 64 мс
) (
    input  wire        clk,
    input  wire        rst_n,
    input  wire [1:0]  rd_phase,     // точка выборки чтения, см. выше (меняется редко)

    // Клиент
    input  wire        req,
    input  wire        we,
    input  wire [20:0] addr,
    input  wire [2:0]  req_len,      // пакетов в запросе − 1 (0…7)
    input  wire [TAGW-1:0] req_tag,  // метка клиента; возвращается в op_tag на всё время операции
    output reg         ack,
    output reg  [TAGW-1:0] op_tag,
    output reg         wr_take,
    output reg  [5:0]  wr_idx,       // слово в запросе
    input  wire [31:0] wdata,
    input  wire [3:0]  wmask,
    output reg         rd_valid,
    output reg  [5:0]  rd_idx,       // слово в запросе
    output reg  [31:0] rd_data,
    output reg  [TAGW-1:0] rd_tag,   // метка операции слова rd_data (op_tag на её READ)
    output wire        init_done,

    // Микросхема
    output reg         sdram_cs_n,
    output reg         sdram_ras_n,
    output reg         sdram_cas_n,
    output reg         sdram_we_n,
    output reg  [10:0] sdram_a,
    output reg  [1:0]  sdram_ba,
    output reg  [3:0]  sdram_dqm,
    output reg  [31:0] sdram_dq_out,
    output reg         sdram_dq_oe,
    input  wire [31:0] sdram_dq_in
);
    localparam int INIT_CYCLES = INIT_US * CLK_KHZ / 1000;
    localparam int REFI_CYCLES = REFI_NS * CLK_KHZ / 1_000_000 - 8;  // запас на занятость
    localparam [15:0] INIT_WAIT_C = INIT_CYCLES;
    // Короткие ожидания — 4-битным cnt: на 123,75 МГц (ядро FT812) 16-битное сравнение с нулём
    // не укладывалось в такт вместе с выбором команды
    localparam [3:0]  RCD_C       = T_RCD - 1;
    localparam [3:0]  RP_C        = T_RP;
    localparam [3:0]  RFC_C       = T_RFC;
    localparam [3:0]  RD_WAIT_C   = CL + 8 + T_RP;
    // EARLY: банк после READ с автопредзарядом готов через BL + tRP (предзаряд — с фронта BL после
    // READ), ACTIVE следующей — ровно тогда (S_IDLE через cnt + 2 такта после READ); слова этого
    // READ ещё идут (CL + 8), но следующий READ — не раньше tRCD после ACTIVE: пакеты на шине
    // не перекрываются. Раньше ждали и все слова (CL + 8 + tRP): у пакета в 8 слов — 17 тактов
    // на операцию вместо 13, у куска в 16 слов — 25 вместо 21
    localparam [3:0]  RD_EARLY_C  = 8 + T_RP - 2;
    localparam [3:0]  WR_WAIT_C   = T_WR + T_RP;
    localparam [11:0] REFI_C      = REFI_CYCLES;

    initial begin
        if (T_RCD < 2) $error("sdram_burst: T_RCD должен быть не меньше 2 тактов");
        if (CL < 2 || CL > 3) $error("sdram_burst: CL — 2 или 3");
        if (T_RCD > 16 || T_RFC > 15 || CL + 8 + T_RP > 15 || T_WR + T_RP > 15)
            $error("sdram_burst: ожидание длиннее 15 тактов — cnt 4-битный");
    end

    // Команды {RAS, CAS, WE}
    localparam [2:0] CMD_NOP = 3'b111, CMD_ACT = 3'b011, CMD_RD = 3'b101, CMD_WR = 3'b100,
                     CMD_PRE = 3'b010, CMD_REF = 3'b001, CMD_MRS = 3'b000;

    typedef enum logic [3:0] {
        S_INIT_WAIT, S_INIT_PRE, S_INIT_REF, S_INIT_MRS,
        S_IDLE, S_ACT, S_WRITE, S_READ, S_WAIT
    } state_t;
    state_t state;

    reg [15:0] init_cnt;           // пауза после включения
    reg [3:0]  cnt;                // короткие ожидания
    reg        st_wr;              // state == S_WRITE — отдельным триггером: от него sdram_dq_oe
                                   // у выводов (дешифратор состояния туда не успевал на 123,75 МГц)
    reg [3:0]  init_refs;
    reg [11:0] refi_cnt;
    reg        refresh_due;
    reg        cur_we;
    reg [20:0] cur_addr;
    reg [2:0]  cur_len;            // пакетов в запросе − 1
    reg [2:0]  rd_k;               // пакет следующей команды READ
    reg [5:0]  burst_pos;          // слово записи в запросе
    reg        done_init;
    assign init_done = done_init;

    // Конвейер отслеживания чтения: единица проходит такты после READ (до E13 — IOB_CAP, CL 3, такт 3)
    reg [14:0] rd_pipe;
    reg        rd_blk;                 // rd_pipe[9:0] != 0 — регистром на такт раньше (путь S_IDLE)

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state        <= S_INIT_WAIT;
            init_cnt     <= '0;
            cnt          <= '0;
            st_wr        <= 1'b0;
            init_refs    <= '0;
            refi_cnt     <= '0;
            refresh_due  <= 1'b0;
            done_init    <= 1'b0;
            ack          <= 1'b0;
            wr_take      <= 1'b0;
            wr_idx       <= '0;
            sdram_cs_n   <= 1'b1;
            {sdram_ras_n, sdram_cas_n, sdram_we_n} <= CMD_NOP;
            sdram_a      <= '0;
            sdram_ba     <= '0;
            sdram_dqm    <= 4'hF;
            sdram_dq_out <= '0;
            sdram_dq_oe  <= 1'b0;
            cur_we       <= 1'b0;
            cur_addr     <= '0;
            cur_len      <= '0;
            rd_k         <= '0;
            burst_pos    <= '0;
            op_tag       <= '0;
        end else begin
            ack        <= 1'b0;
            wr_take    <= 1'b0;
            sdram_cs_n <= 1'b0;
            {sdram_ras_n, sdram_cas_n, sdram_we_n} <= CMD_NOP;
            // Данные записи — каждый такт (на выводы идут только при dq_oe); dq_oe — такт S_WRITE
            sdram_dq_out <= wdata;
            sdram_dq_oe  <= st_wr;

            // Счётчик регенерации идёт всегда после инициализации
            if (done_init) begin
                if (refi_cnt == REFI_C) begin
                    refi_cnt    <= '0;
                    refresh_due <= 1'b1;
                end else begin
                    refi_cnt <= refi_cnt + 12'd1;
                end
            end

            case (state)
            // ---------------- инициализация ----------------
            S_INIT_WAIT: begin
                sdram_dqm <= 4'hF;
                if (init_cnt == INIT_WAIT_C) begin
                    state <= S_INIT_PRE;
                end else init_cnt <= init_cnt + 16'd1;
            end
            S_INIT_PRE: begin
                {sdram_ras_n, sdram_cas_n, sdram_we_n} <= CMD_PRE;
                sdram_a[10] <= 1'b1;                 // все банки
                cnt   <= RP_C;
                state <= S_INIT_REF;
            end
            S_INIT_REF: begin
                if (cnt != 0) cnt <= cnt - 4'd1;
                else if (init_refs == 4'd8) begin
                    state <= S_INIT_MRS;
                end else begin
                    {sdram_ras_n, sdram_cas_n, sdram_we_n} <= CMD_REF;
                    init_refs <= init_refs + 4'd1;
                    cnt <= RFC_C;
                end
            end
            S_INIT_MRS: begin
                {sdram_ras_n, sdram_cas_n, sdram_we_n} <= CMD_MRS;
                sdram_ba <= 2'b00;
                // A9=0 пакетная запись, A6..A4 = CL, A3=0 последовательный, A2..A0=011 — 8 слов
                sdram_a  <= 11'd0;
                sdram_a[2:0] <= 3'b011;
                sdram_a[6:4] <= CL[2:0];
                cnt      <= 4'd3;
                state    <= S_WAIT;
                done_init <= 1'b1;
            end

            // ---------------- работа ----------------
            S_IDLE: begin
                sdram_dqm <= 4'h0;
                if (refresh_due) begin
                    {sdram_ras_n, sdram_cas_n, sdram_we_n} <= CMD_REF;
                    refresh_due <= 1'b0;
                    cnt   <= RFC_C;
                    state <= S_WAIT;
                end else if (req && !(EARLY && we && rd_blk)) begin
                    // (EARLY: запись — не раньше 11 тактов после READ: слова чтения ушли с шины,
                    // DQ ведёт контроллер со своего WRITE — два такта зазора)
                    ack      <= 1'b1;
                    cur_we   <= we;
                    cur_addr <= addr;
                    cur_len  <= req_len;
                    op_tag   <= req_tag;
                    {sdram_ras_n, sdram_cas_n, sdram_we_n} <= CMD_ACT;
                    sdram_ba <= addr[9:8];
                    sdram_a  <= addr[20:10];
                    cnt      <= RCD_C;
                    state    <= S_ACT;
                end
            end
            S_ACT: begin
                // ACTIVE и READ/WRITE на выводах разделены ровно T_RCD тактами.
                // Для записи слово 0 запрашивается на такт раньше команды WRITE.
                if (cur_we && cnt == 4'd1) begin
                    wr_take   <= 1'b1;
                    wr_idx    <= 6'd0;
                    burst_pos <= 6'd0;
                    state     <= S_WRITE;
                    st_wr     <= 1'b1;
                end else if (!cur_we && cnt == 4'd0) begin
                    // A10=1 — автопредзаряд, только у последнего пакета запроса
                    {sdram_ras_n, sdram_cas_n, sdram_we_n} <= CMD_RD;
                    sdram_ba <= cur_addr[9:8];
                    sdram_a  <= {cur_len == 3'd0, 2'b00, cur_addr[7:3], 3'b000};
                    if (cur_len == 3'd0) begin
                        cnt   <= EARLY ? RD_EARLY_C : RD_WAIT_C;
                        state <= S_WAIT;
                    end else begin
                        rd_k  <= 3'd1;
                        cnt   <= 4'd7;             // следующий READ — через 8 тактов
                        state <= S_READ;
                    end
                end else begin
                    cnt <= cnt - 4'd1;
                end
            end
            S_READ: begin
                // Пакеты запроса без закрытия строки: READ каждые 8 тактов, данные подряд
                if (cnt == 4'd0) begin
                    {sdram_ras_n, sdram_cas_n, sdram_we_n} <= CMD_RD;
                    sdram_ba <= cur_addr[9:8];
                    sdram_a  <= {rd_k == cur_len, 2'b00, 5'(cur_addr[7:3] + 5'(rd_k)), 3'b000};
                    if (rd_k == cur_len) begin
                        cnt   <= EARLY ? RD_EARLY_C : RD_WAIT_C;
                        state <= S_WAIT;
                    end else begin
                        rd_k <= rd_k + 3'd1;
                        cnt  <= 4'd7;
                    end
                end else begin
                    cnt <= cnt - 4'd1;
                end
            end
            S_WRITE: begin
                // wr_take был выставлен тактом раньше: wdata сейчас — слово burst_pos
                // (sdram_dq_out и sdram_dq_oe — выше, каждый такт)
                sdram_dqm    <= ~wmask;          // DQM записи действует в такт данных
                if (burst_pos[2:0] == 3'd0) begin
                    // WRITE каждого пакета; автопредзаряд — у последнего
                    {sdram_ras_n, sdram_cas_n, sdram_we_n} <= CMD_WR;
                    sdram_ba <= cur_addr[9:8];
                    sdram_a  <= {burst_pos[5:3] == cur_len, 2'b00,
                                 5'(cur_addr[7:3] + 5'(burst_pos[5:3])), 3'b000};
                end
                if (burst_pos == {cur_len, 3'd7}) begin
                    cnt   <= WR_WAIT_C;
                    state <= S_WAIT;
                    st_wr <= 1'b0;
                    // DQM снова 0 — в S_IDLE; до этого тактов с данными нет
                end else begin
                    burst_pos <= burst_pos + 6'd1;
                    wr_take   <= 1'b1;
                    wr_idx    <= burst_pos + 6'd1;
                end
            end
            S_WAIT: begin
                if (cnt != 0) cnt <= cnt - 4'd1;
                else state <= S_IDLE;
            end
            default: state <= S_IDLE;
            endcase
        end
    end

    // Приём данных чтения. rd_pipe[k] = 1 после фронта E(k), если READ выставлен на E0.
    // Выборка на фронте E(k+1) берёт DQ как есть, выборка на спаде — регистр dq_neg,
    // защёлкнутый полтакта раньше этого фронта. Отсюда для rd_phase:
    //   0: E2 = условие rd_pipe[1], DQ;      1: E2,5 = rd_pipe[2], dq_neg;
    //   2: E3 = rd_pipe[2], DQ;              3: E3,5 = rd_pipe[3], dq_neg.
    wire rd_issue = ((state == S_ACT) && (cnt == 4'd0) && !cur_we) ||
                    ((state == S_READ) && (cnt == 4'd0));
    localparam int CB = CL - 2;        // сдвиг точек выборки от CL=2 (CL — 2 или 3)
    reg  [1:0] rd_ph;                  // фаза держится на время пакета
    reg        rd_now;                 // сейчас выборка одного из 8 слов пакета
    wire [31:0] rd_src;                // откуда rd_data берёт слово
    generate if (IOB_CAP) begin : g_cap_iob
        // Регистр у вывода: каждый такт, без разрешения и сброса — иначе он не встанет в блок
        // ввода-вывода (UG289 4.1: регистр, вход которого — буфер вывода и только он, — это IREG).
        // Микросхема выдаёт слово 0 через tAC после своего фронта CL − 1 от READ и держит до tOH
        // после фронта CL. Слово 0 взято на фронте E(CL + rd_ph): rd_pipe[k] после E(k) —
        // dq_q в этом такте хранит DQ фронта E(k), rd_data возьмёт его на следующем.
        reg [31:0] dq_q;
        always_ff @(posedge clk) dq_q <= sdram_dq_in;
        assign rd_src = dq_q;
        always_comb rd_now = |rd_pipe[(CL + rd_ph) +: 8];
    end else begin : g_cap_edge
        wire [31:0] dq_neg;
`ifdef SIMULATION
        reg [31:0] dq_neg_q;
        always_ff @(negedge clk) dq_neg_q <= sdram_dq_in;
        assign dq_neg = dq_neg_q;
`else
        // Явные триггеры по спаду на глобальном клоке: иначе синтез берёт для них клоком
        // инвертор O_sdram_clk (LUT), и путь dq_neg → rd_data выпадает из анализа времени.
        genvar gi;
        for (gi = 0; gi < 32; gi = gi + 1) begin : g_dq_neg
            DFFN dq_ff (.Q(dq_neg[gi]), .D(sdram_dq_in[gi]), .CLK(clk));
        end
`endif
        assign rd_src = rd_ph[0] ? dq_neg : sdram_dq_in;
        always_comb begin
            case (rd_ph)
                2'd0:       rd_now = |rd_pipe[8+CB:1+CB];
                2'd1, 2'd2: rd_now = |rd_pipe[9+CB:2+CB];
                default:    rd_now = |rd_pipe[10+CB:3+CB];
            endcase
        end
    end endgenerate
    // Метка операции слова: op_tag по тактам от READ (tg_pipe[j] — op_tag j + 1 тактов назад, как
    // rd_pipe). Слово пакета, чей READ был в такт R, берёт tg_pipe начала своего окна выборки — op_tag
    // в такты R…R + 7: у своей операции (её ack — до READ; ack следующей у EARLY — не раньше R + 10)
    reg  [TAGW-1:0] tg_pipe [0:14];
    wire [3:0]  tg_lo = IOB_CAP ? 4'(CL) + {2'b00, rd_ph}
                                : 4'(1 + CB) + (rd_ph == 2'd0 ? 4'd0 : rd_ph == 2'd3 ? 4'd2 : 4'd1);
    always_ff @(posedge clk) begin
        tg_pipe[0] <= op_tag;
        for (int k = 1; k < 15; k++) tg_pipe[k] <= tg_pipe[k-1];
    end
    reg [5:0] rd_cnt;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rd_pipe  <= '0;
            rd_blk   <= 1'b0;
            rd_valid <= 1'b0;
            rd_idx   <= '0;
            rd_data  <= '0;
            rd_tag   <= '0;
            rd_cnt   <= '0;
            rd_ph    <= 2'd1;
        end else begin
            // Бит 0 — такт выдачи команды READ; пакет занимает 8 тактов подряд
            rd_pipe  <= {rd_pipe[13:0], rd_issue};
            rd_blk   <= |{rd_pipe[8:0], rd_issue};
            rd_valid <= 1'b0;
            if (rd_pipe == '0 && !rd_issue) rd_ph <= rd_phase;   // менять только между пакетами
            if (rd_now) begin
                rd_valid <= 1'b1;
                rd_data  <= rd_src;
                rd_tag   <= tg_pipe[tg_lo];
                rd_idx   <= rd_cnt;
                rd_cnt   <= rd_cnt + 6'd1;
            end else begin
                rd_cnt <= '0;
            end
        end
    end

endmodule

// ---------------------------------------------------------------------------------------------
// sdram_train — контроллер sdram_burst (IOB_CAP) с подбором сдвига клока микросхемы и такта
// выборки чтения после включения (ядро FT812, с 1.38).
//
// Зачем. Клок микросхемы — CLKOUTP той же PLL, что даёт клок логики (fb_pll_sd): сдвиг между ними
// задаёт сама PLL (PSDA, 1/16 периода), как у ядер ZX Next и NES на этой же SDRAM. От сдвига
// зависят сразу два окна: в каком месте такта микросхема берёт команды, адрес и слова записи (их
// выставляют регистры у выводов на фронте логики) и где относительно фронтов логики лежит слово
// чтения у регистра dq_q. Где эти окна на плате, заранее не знать (задержки клока, команд и данных
// внутри корпуса GW2AR); Next подбирал сдвиг на плате вручную. Здесь его ищет само ядро — сразу
// после включения, пока SDRAM никому не нужна: клиенты видят init_done только после подбора
// (~0,45 мс вместе с паузой включения INIT_US).
//
// Как. Пары (такт L = 0…3, сдвиг p = 15…0) идут по моменту выборки относительно слова:
// k = 16·L + (15 − p) — следующая пара берёт слово на 1/16 такта позже. Для каждой контроллер
// стоит в сбросе (CS_N = 1: микросхема клок не слушает), сдвиг меняется, через PH_WAIT тактов
// контроллер заново инициализирует микросхему (предзаряд, регенерации, mode register — сдвиг, при
// котором команды берутся неверно, мог испортить и их), пишет два блока по 64 слова (A и B: строка,
// банк и столбцы разные во всех разрядах адреса — неверно взятый разряд кладёт блоки друг на друга)
// и читает оба со сверкой. Годные пары — сплошной участок оси k (окно слова чтения), урезанный там,
// где сдвиг не годится командам. Берётся середина самого длинного сплошного участка; на ней — ещё
// раз инициализация и сверка. Ни одной годной пары — DEF_PHASE и DEF_LAT, признак в train_st.
// Блоки A и B — в RAM_G и в кадрах: после включения там ещё ничего нет.
//
// Слово i блока b: в соседних словах инвертированы все 32 линии (самое частое переключение), номер
// слова и блока — в разрядах (слово соседнего такта или чужого блока не сойдётся).
//
// train_st: [3:0] сдвиг, [5:4] такт, 6 подбор идёт, 7 подбор закончен, 8 годных пар нет, 9 сверка
// на выбранной паре не сошлась, [16:10] длина годного участка (пар), [22:17] его середина k.
// train_win: [5:0] первая годная пара k, [13:8] последняя, [22:16] годных пар всего. После подбора
// не меняются.
//
module sdram_train #(
    parameter int TAGW      = 1,
    parameter int CLK_KHZ   = 121_500,
    parameter int CL        = 3,
    parameter int T_RCD     = 2,
    parameter int T_RP      = 2,
    parameter int T_WR      = 2,
    parameter int T_RFC     = 8,
    parameter int INIT_US   = 200,            // пауза после включения питания — один раз
    parameter logic [3:0] DEF_PHASE = 4'd12,  // сдвиг на время паузы и если годных пар нет
    parameter logic [1:0] DEF_LAT   = 2'd1,
    parameter int PH_WAIT   = 32,             // тактов от смены сдвига до инициализации
    parameter bit EARLY     = 1'b1            // как у sdram_burst (ядро FT812 с 1.39 — 1)
) (
    input  wire        clk,
    input  wire        rst_n,

    // Клиент — как у sdram_burst; такт выборки чтения (rd_phase) выбирает подбор
    input  wire        req,
    input  wire        we,
    input  wire [20:0] addr,
    input  wire [2:0]  req_len,
    input  wire [TAGW-1:0] req_tag,
    output wire        ack,
    output wire [TAGW-1:0] op_tag,
    output wire        wr_take,
    output wire [5:0]  wr_idx,
    input  wire [31:0] wdata,
    input  wire [3:0]  wmask,
    output wire        rd_valid,
    output wire [5:0]  rd_idx,
    output wire [31:0] rd_data,
    output wire [TAGW-1:0] rd_tag,
    output wire        init_done,

    // Сдвиг клока микросхемы (PSDA PLL) и итог подбора
    output reg  [3:0]  phase,
    output wire [22:0] train_st,
    output wire [22:0] train_win,

    // Микросхема
    output wire        sdram_cs_n,
    output wire        sdram_ras_n,
    output wire        sdram_cas_n,
    output wire        sdram_we_n,
    output wire [10:0] sdram_a,
    output wire [1:0]  sdram_ba,
    output wire [3:0]  sdram_dqm,
    output wire [31:0] sdram_dq_out,
    output wire        sdram_dq_oe,
    input  wire [31:0] sdram_dq_in
);
    localparam logic [20:0] BLK_A = {11'h2AA, 2'b01, 8'h00};   // {строка, банк, столбец}
    localparam logic [20:0] BLK_B = {11'h555, 2'b10, 8'hC0};
    localparam int PWR_CYCLES = INIT_US * CLK_KHZ / 1000;

    function automatic [31:0] pat(input blk, input [5:0] i);
        logic [15:0] h;
        h = {i, blk, ~blk, ~i, ~blk, blk};
        pat = {h, h} ^ {32{i[0]}};
    endfunction

    typedef enum logic [3:0] {
        T_PWR, T_SET, T_SETW, T_INIT, T_REQ, T_RDW, T_REC, T_PICK, T_DONE
    } tst_t;
    tst_t tst;

    reg         training;            // 1 — контроллер у подбора, клиенты ждут
    reg         ctl_rst_n;           // сброс контроллера (снимается по фронту clk)
    reg  [1:0]  lat;                 // такт выборки (rd_phase контроллера)
    reg  [14:0] wcnt;                // паузы и предел ожидания
    reg  [5:0]  k;                   // пара
    reg         fin;                 // идёт проверка выбранной пары
    reg  [1:0]  op;                  // запрос: 0 запись A, 1 запись B, 2 чтение A, 3 чтение B
    reg         t_req;
    reg         wblk;                // блок, слова которого берёт контроллер (0 — A, 1 — B)
    reg  [6:0]  rn1;                 // сверка, ступень 1: слов чтения взято (6 — блок B)
    reg  [3:0]  m4;                  //   несовпадения по байтам слова
    reg         mi, v1;              //   несовпал номер слова; слово было
    reg  [7:0]  rn;                  // ступень 2: слов чтения сверено (7 — все 128)
    reg         err;
    reg  [6:0]  run_len, best_len;   // годный участок: текущий и самый длинный
    reg  [5:0]  run_k, best_k;       //   его начало
    reg  [5:0]  first_k, last_k;     // первая и последняя годная пара
    reg  [6:0]  n_ok;                // годных пар
    reg         nowin, vfail;
    reg  [5:0]  ck;                  // середина выбранного участка

    wire        s_ack, s_wr_take, s_rd_valid, s_init_done;
    wire [5:0]  s_wr_idx, s_rd_idx;
    wire [31:0] s_rd_data;
    wire [31:0] exp1 = pat(rn1[6], rn1[5:0]);

    // Слово записи — сразу из номера слова (wr_idx — регистр контроллера) и блока: в такт wr_take
    // контроллер берёт слово wr_idx
    sdram_burst #(.TAGW(TAGW), .CLK_KHZ(CLK_KHZ), .IOB_CAP(1'b1), .EARLY(EARLY), .CL(CL), .T_RCD(T_RCD),
                  .T_RP(T_RP), .T_WR(T_WR), .T_RFC(T_RFC), .INIT_US(0)) ctl (
        .clk(clk), .rst_n(ctl_rst_n), .rd_phase(lat),
        .req(training ? t_req : req), .we(training ? !op[1] : we),
        .addr(training ? (op[0] ? BLK_B : BLK_A) : addr), .req_len(training ? 3'd7 : req_len),
        .req_tag(training ? {TAGW{1'b0}} : req_tag),
        .ack(s_ack), .op_tag(op_tag), .wr_take(s_wr_take), .wr_idx(s_wr_idx),
        .wdata(training ? pat(wblk, s_wr_idx) : wdata), .wmask(training ? 4'hF : wmask),
        .rd_valid(s_rd_valid), .rd_idx(s_rd_idx), .rd_data(s_rd_data), .rd_tag(rd_tag),
        .init_done(s_init_done),
        .sdram_cs_n(sdram_cs_n), .sdram_ras_n(sdram_ras_n), .sdram_cas_n(sdram_cas_n),
        .sdram_we_n(sdram_we_n), .sdram_a(sdram_a), .sdram_ba(sdram_ba), .sdram_dqm(sdram_dqm),
        .sdram_dq_out(sdram_dq_out), .sdram_dq_oe(sdram_dq_oe), .sdram_dq_in(sdram_dq_in)
    );
    assign ack       = s_ack && !training;
    assign wr_take   = s_wr_take && !training;
    assign wr_idx    = s_wr_idx;
    assign rd_valid  = s_rd_valid && !training;
    assign rd_idx    = s_rd_idx;
    assign rd_data   = s_rd_data;
    assign init_done = s_init_done && !training;
    assign train_st  = {ck, best_len, vfail, nowin, !training, training, lat, phase};
    assign train_win = {n_ok, 2'b00, last_k, 2'b00, first_k};

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            tst <= T_PWR; training <= 1'b1; ctl_rst_n <= 1'b0;
            phase <= DEF_PHASE; lat <= DEF_LAT;
            wcnt <= '0; k <= '0; fin <= 1'b0; op <= '0; t_req <= 1'b0; wblk <= 1'b0;
            rn1 <= '0; m4 <= '0; mi <= 1'b0; v1 <= 1'b0; rn <= '0; err <= 1'b0;
            run_len <= '0; best_len <= '0; run_k <= '0; best_k <= '0;
            first_k <= '0; last_k <= '0; n_ok <= '0;
            nowin <= 1'b0; vfail <= 1'b0; ck <= '0;
        end else begin
            // блок слов записи: B — после 64-го слова A
            if (training && s_wr_take && s_wr_idx == 6'd63) wblk <= 1'b1;
            // сверка слов чтения (сначала 64 блока A, затем 64 блока B) в две ступени
            v1 <= training && s_rd_valid;
            if (training && s_rd_valid) begin
                rn1 <= rn1 + 7'd1;
                for (int j = 0; j < 4; j++) m4[j] <= |(s_rd_data[8*j +: 8] ^ exp1[8*j +: 8]);
                mi <= s_rd_idx != rn1[5:0];
            end
            if (v1) begin
                rn <= rn + 8'd1;
                if (m4 != 4'd0 || mi) err <= 1'b1;
            end

            case (tst)
            T_PWR: begin                          // пауза после включения питания — один раз
                wcnt <= wcnt + 15'd1;
                if (wcnt == 15'(PWR_CYCLES)) tst <= T_SET;
            end
            T_SET: begin                          // контроллер в сброс, новый сдвиг и такт
                ctl_rst_n <= 1'b0;
                if (!fin) begin
                    lat   <= k[5:4];
                    phase <= ~k[3:0];
                end
                wcnt <= '0; op <= '0; wblk <= 1'b0; rn1 <= '0; rn <= '0; err <= 1'b0;
                tst  <= T_SETW;
            end
            T_SETW: begin                         // клок микросхемы встаёт на новый сдвиг
                wcnt <= wcnt + 15'd1;
                if (wcnt == 15'(PH_WAIT)) begin
                    ctl_rst_n <= 1'b1;
                    wcnt <= '0;
                    tst  <= T_INIT;
                end
            end
            T_INIT: begin                         // инициализация микросхемы заново
                wcnt <= wcnt + 15'd1;
                if (s_init_done) begin
                    t_req <= 1'b1;
                    wcnt  <= '0;
                    tst   <= T_REQ;
                end else if (wcnt[12]) begin
                    err <= 1'b1;
                    tst <= T_REC;
                end
            end
            T_REQ: begin                          // запись A, B, чтение A, B — подряд
                wcnt <= wcnt + 15'd1;
                if (s_ack) begin
                    op <= op + 2'd1;
                    if (op == 2'd3) begin
                        t_req <= 1'b0;
                        tst   <= T_RDW;
                    end
                end
                if (wcnt[12]) begin
                    t_req <= 1'b0;
                    err   <= 1'b1;
                    tst   <= T_REC;
                end
            end
            T_RDW: begin                          // все 128 слов чтения сверены
                wcnt <= wcnt + 15'd1;
                if (rn[7]) tst <= T_REC;
                else if (wcnt[12]) begin
                    err <= 1'b1;
                    tst <= T_REC;
                end
            end
            T_REC: if (!fin) begin : rec
                logic       p;
                logic [6:0] rl;
                p  = !err;
                rl = p ? run_len + 7'd1 : 7'd0;
                run_len <= rl;
                if (p && run_len == 7'd0) run_k <= k;
                if (p && rl > best_len) begin
                    best_len <= rl;
                    best_k   <= (run_len == 7'd0) ? k : run_k;
                end
                if (p) begin
                    if (n_ok == 7'd0) first_k <= k;
                    last_k <= k;
                    n_ok   <= n_ok + 7'd1;
                end
                if (k == 6'd63) tst <= T_PICK;
                else begin
                    k   <= k + 6'd1;
                    tst <= T_SET;
                end
            end else begin
                vfail    <= err;
                training <= 1'b0;                 // контроллер — клиентам, на выбранной паре
                tst      <= T_DONE;
            end
            T_PICK: begin : pick                  // середина самого длинного участка — и её проверка
                logic [5:0] c;
                fin <= 1'b1;
                if (best_len == 7'd0) begin
                    nowin <= 1'b1;
                    lat   <= DEF_LAT;
                    phase <= DEF_PHASE;
                end else begin
                    c = best_k + 6'((best_len - 7'd1) >> 1);
                    ck    <= c;
                    lat   <= c[5:4];
                    phase <= ~c[3:0];
                end
                tst <= T_SET;
            end
            default: ;
            endcase
        end
    end
endmodule
