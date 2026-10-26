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
module sdram_burst #(
    parameter int TAGW      = 1,       // ширина метки клиента
    parameter int CLK_KHZ   = 67_500,
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

    // Конвейер отслеживания чтения: единица проходит такты после READ
    reg [11:0] rd_pipe;

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
                end else if (req) begin
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
                        cnt   <= RD_WAIT_C;
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
                        cnt   <= RD_WAIT_C;
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

    localparam int CB = CL - 2;        // сдвиг точек выборки от CL=2 (CL — 2 или 3)
    reg  [1:0] rd_ph;                  // фаза держится на время пакета
    reg        rd_now;                 // сейчас выборка одного из 8 слов пакета
    always_comb begin
        case (rd_ph)
            2'd0:       rd_now = |rd_pipe[8+CB:1+CB];
            2'd1, 2'd2: rd_now = |rd_pipe[9+CB:2+CB];
            default:    rd_now = |rd_pipe[10+CB:3+CB];
        endcase
    end
    reg [5:0] rd_cnt;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rd_pipe  <= '0;
            rd_valid <= 1'b0;
            rd_idx   <= '0;
            rd_data  <= '0;
            rd_cnt   <= '0;
            rd_ph    <= 2'd1;
        end else begin
            // Бит 0 — такт выдачи команды READ; пакет занимает 8 тактов подряд
            rd_pipe  <= {rd_pipe[10:0], rd_issue};
            rd_valid <= 1'b0;
            if (rd_pipe == '0 && !rd_issue) rd_ph <= rd_phase;   // менять только между пакетами
            if (rd_now) begin
                rd_valid <= 1'b1;
                rd_data  <= rd_ph[0] ? dq_neg : sdram_dq_in;
                rd_idx   <= rd_cnt;
                rd_cnt   <= rd_cnt + 6'd1;
            end else begin
                rd_cnt <= '0;
            end
        end
    end

endmodule
