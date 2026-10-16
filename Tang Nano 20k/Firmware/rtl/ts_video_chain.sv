// SPDX-License-Identifier: Apache-2.0
//
// Видеотракт целиком, без тактирования и без TMDS: приём с мультиплексоров,
// строчный буфер, привязка выходного кадра ко входному, выходная развёртка и цвет.
//
// Вынесено отдельно от верхнего модуля ровно затем, чтобы тракт можно было
// прогнать в симуляции с идеальными клоками, не подменяя примитивы PLL Gowin.
//
// Соотношение доменов: clk_cap = 2 × pixel_clk, оба получены из входного PCLK,
// поэтому частоты связаны жёстко, а произвольная фаза между ними гасится
// строчным буфером и переносом кадрового импульса через триггер-переключатель.
//
module ts_video_chain #(
    // Положение активного окна во входном растре (подбирается на железе)
    // H_IN_OFFSET учитывает, что первый отсчёт строки тратится на выравнивание фазы SEL:
    // приёмник нумерует пиксели начиная со второго отсчёта после HSYNC.
    parameter int H_IN_OFFSET = 131,
    parameter int V_IN_OFFSET = 45,
    // Выходной растр: по умолчанию — входные тоталы с раскладкой импульсов от 576p50
    parameter int H_TOTAL  = 896,
    parameter int H_SYNC   = 64,
    parameter int H_BACK   = 68,
    parameter int H_ACTIVE = 720,
    parameter int V_TOTAL  = 640,
    parameter int V_SYNC   = 5,
    parameter int V_BACK   = 39,
    parameter int V_ACTIVE = 576,
    // Приём
    parameter int SAMPLE_DELAY  = 1,
    parameter bit HS_ACTIVE_LOW = 1'b1,
    parameter bit VS_ACTIVE_LOW = 1'b1,
    parameter bit SEL_PHASE     = 1'b1
) (
    input  wire        clk_cap,     // 56 МГц (2 × PCLK)
    input  wire        rst_cap_n,
    input  wire        pixel_clk,   // 28 МГц (= PCLK)
    input  wire        rst_pix_n,

    input  wire [7:0]  mux_d,
    input  wire        hsync_in,
    input  wire        vsync_in,
    input  wire        blank_video, // 1 — выводить чёрное поле (режим FT812)
    output wire        mux_sel,

    output reg  [7:0]  out_r,
    output reg  [7:0]  out_g,
    output reg  [7:0]  out_b,
    output reg         out_de,
    output reg         out_hs,
    output reg         out_vs
);

    // ------------------------------------------------------------------
    // Приём двух фаз
    // ------------------------------------------------------------------
    wire       pix_valid;
    wire [4:0] pix_r, pix_g, pix_b;
    wire       pix_pal;
    wire       hs_active, vs_active, hs_start, vs_start;

    localparam [1:0] SAMPLE_DELAY_CODE = SAMPLE_DELAY - 1;

    ts_mux_capture capture (
        .clk_cap(clk_cap), .rst_n(rst_cap_n),
        .sample_delay(SAMPLE_DELAY_CODE), .sel_phase(SEL_PHASE),
        .mux_d(mux_d), .hsync_in(hsync_in), .vsync_in(vsync_in),
        .hs_active_low(HS_ACTIVE_LOW), .vs_active_low(VS_ACTIVE_LOW),
        .hs_level(), .vs_level(),
        .mux_sel(mux_sel),
        .pix_valid(pix_valid), .pix_r(pix_r), .pix_g(pix_g), .pix_b(pix_b), .pix_pal(pix_pal),
        .hs_active(hs_active), .vs_active(vs_active),
        .hs_start(hs_start), .vs_start(vs_start)
    );

    // ------------------------------------------------------------------
    // Счётчики входного растра и запись в строчный буфер
    // ------------------------------------------------------------------
    localparam int ADDR_MAX = 1023;

    reg [9:0]  in_x;
    reg [10:0] in_line;

    always_ff @(posedge clk_cap or negedge rst_cap_n) begin
        if (!rst_cap_n) begin
            in_x    <= '0;
            in_line <= '0;
        end else if (vs_start) begin
            // Начало кадра: строка 0, отсчёт пикселей начнётся с ближайшего HSYNC
            in_x    <= '0;
            in_line <= '0;
        end else if (hs_start) begin
            in_x    <= '0;
            in_line <= in_line + 11'd1;
        end else if (pix_valid && in_x != ADDR_MAX[9:0]) begin
            in_x    <= in_x + 10'd1;
        end
    end

    wire [15:0] rd_data;
    wire [9:0]  rd_addr;
    wire        rd_line;

    ts_line_buffer #(.ADDR_BITS(10)) line_buffer (
        .wr_clk(clk_cap), .wr_en(pix_valid), .wr_line(in_line[0]),
        .wr_addr(in_x), .wr_data({pix_pal, pix_b, pix_g, pix_r}),
        .rd_clk(pixel_clk), .rd_line(rd_line), .rd_addr(rd_addr), .rd_data(rd_data)
    );

    // ------------------------------------------------------------------
    // Кадровая привязка: импульс через V_START_DELAY входных строк после VSYNC.
    // Условие V_START_DELAY + V_SYNC + V_BACK = V_IN_OFFSET + 1 даёт отставание
    // вывода ровно на одну входную строку — строка читается уже дописанной.
    // ------------------------------------------------------------------
    // Импульс привязки выдаётся на HSYNC той строки, где счётчик равен V_START_DELAY,
    // то есть выходной кадр начинается со следующей входной строки. Отсюда условие
    // V_START_DELAY + 1 + V_SYNC + V_BACK = V_IN_OFFSET + 1.
    localparam int V_START_DELAY = V_IN_OFFSET - V_SYNC - V_BACK;

    initial begin
        if (V_START_DELAY < 0)
            $error("V_IN_OFFSET слишком мал: активное окно входа начинается раньше, чем выходное");
    end

    reg frame_toggle;
    always_ff @(posedge clk_cap or negedge rst_cap_n) begin
        if (!rst_cap_n)
            frame_toggle <= 1'b0;
        else if (hs_start && (in_line == V_START_DELAY[10:0]))
            frame_toggle <= ~frame_toggle;
    end

    reg [2:0] frame_sync_sr;
    reg       frame_pending;
    wire      frame_sync;

    always_ff @(posedge pixel_clk or negedge rst_pix_n) begin
        if (!rst_pix_n) begin
            frame_sync_sr <= '0;
            frame_pending <= 1'b0;
        end else begin
            frame_sync_sr <= {frame_sync_sr[1:0], frame_toggle};
            if (frame_sync_sr[2] ^ frame_sync_sr[1])
                frame_pending <= 1'b1;      // кадровый импульс пришёл, ждём конца строки
            else if (frame_sync)
                frame_pending <= 1'b0;
        end
    end

    // ------------------------------------------------------------------
    // Выходная развёртка
    // ------------------------------------------------------------------
    wire [10:0] out_x, out_y, active_x, active_y, next_active_x, next_active_y;
    wire        de_raw, hs_raw, vs_raw, next_de;

    assign frame_sync = frame_pending && (out_x == H_TOTAL[10:0] - 11'd1);

    ts_video_timing #(
        .H_TOTAL(H_TOTAL), .H_SYNC(H_SYNC), .H_BACK(H_BACK), .H_ACTIVE(H_ACTIVE),
        .V_TOTAL(V_TOTAL), .V_SYNC(V_SYNC), .V_BACK(V_BACK), .V_ACTIVE(V_ACTIVE)
    ) timing (
        .pixel_clk(pixel_clk), .rst_n(rst_pix_n), .frame_sync(frame_sync),
        .x(out_x), .y(out_y), .hsync(hs_raw), .vsync(vs_raw), .de(de_raw),
        .active_x(active_x), .active_y(active_y),
        .next_active_x(next_active_x), .next_active_y(next_active_y), .next_de(next_de)
    );

    // Адрес выставляется на такт раньше: у блочной памяти один такт задержки чтения.
    assign rd_addr = next_active_x[9:0] + H_IN_OFFSET[9:0];
    assign rd_line = next_active_y[0] ^ V_IN_OFFSET[0];

    // ------------------------------------------------------------------
    // Цвет и выравнивание сигналов на такт задержки памяти
    // ------------------------------------------------------------------
    wire [7:0] r8, g8, b8;

    legacy_palette palette (
        .pal_sel(rd_data[15]),
        .r5(rd_data[4:0]), .g5(rd_data[9:5]), .b5(rd_data[14:10]),
        .r8(r8), .g8(g8), .b8(b8)
    );

    // Данные из памяти приходят на такт позже выставленного адреса и совпадают
    // с текущим пикселем растра. Регистрируем цвет и синхро одной ступенью:
    // так вся картинка сдвигается ровно на один пиксель, а длинная комбинаторика
    // палитры (деление на 24) не попадает в критический путь.
    always_ff @(posedge pixel_clk or negedge rst_pix_n) begin
        if (!rst_pix_n) begin
            out_r  <= 8'h00;
            out_g  <= 8'h00;
            out_b  <= 8'h00;
            out_de <= 1'b0;
            out_hs <= 1'b1;
            out_vs <= 1'b1;
        end else begin
            out_de <= de_raw;
            out_hs <= hs_raw;
            out_vs <= vs_raw;
            if (de_raw && !blank_video) begin
                out_r <= r8;
                out_g <= g8;
                out_b <= b8;
            end else begin
                out_r <= 8'h00;
                out_g <= 8'h00;
                out_b <= 8'h00;
            end
        end
    end

endmodule
