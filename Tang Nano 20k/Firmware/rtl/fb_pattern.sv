// SPDX-License-Identifier: Apache-2.0
//
// Источник испытательной таблицы для кадрового буфера. Домен clk_mem.
//
// Выдаёт тот же поток слов, что FIFO захвата (формат — fb_capture): заголовок строки,
// 360 пар пикселей, …, слово конца кадра с ok=1. ts_fb_core подставляет этот поток
// вместо FIFO, когда Evo нет (источник AUTO) или выбран источник TEST, — и таблица идёт
// через SDRAM тем же путём, что и картинка Evo. Поэтому по ней проверяются HDMI, SDRAM
// и точка выборки чтения ещё до подключения к Evo.
//
// Интерфейс — как у чтения async_fifo: empty — слова нет; rd — забрать слово; q
// появляется на такт позже rd и держится до следующего rd.
// Новый кадр начинается не чаще, чем раз в PERIOD тактов (20 мс), и только при enable.
//
module fb_pattern #(
    parameter int H_ACTIVE = 720,
    parameter int V_ACTIVE = 576,
    parameter int PERIOD   = 1_350_000
) (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        enable,

    input  wire        rd,
    output reg  [32:0] q,
    output wire        empty
);
    `include "fb_pattern_fn.svh"

    localparam [9:0]  H_WORDS_LAST = H_ACTIVE / 2 - 1;
    localparam [9:0]  V_LAST       = V_ACTIVE - 1;
    localparam [10:0] W            = H_ACTIVE;
    localparam [10:0] H            = V_ACTIVE;
    localparam [21:0] PERIOD_C     = PERIOD - 1;

    typedef enum logic [1:0] { P_IDLE, P_HDR, P_PIX, P_END } pstate_t;
    pstate_t   st;
    reg [21:0] timer;
    reg [9:0]  y, xw;
    reg [7:0]  f;

    assign empty = (st == P_IDLE);

    wire [10:0] x0 = {xw, 1'b0};
    wire [15:0] p0 = fb_pattern_px(x0,          {1'b0, y}, f, W, H);
    wire [15:0] p1 = fb_pattern_px(x0 + 11'd1, {1'b0, y}, f, W, H);

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            st    <= P_IDLE;
            timer <= '0;
            y     <= '0;
            xw    <= '0;
            f     <= '0;
            q     <= '0;
        end else begin
            if (timer != PERIOD_C) timer <= timer + 22'd1;
            if (!enable) begin
                st <= P_IDLE;
            end else case (st)
                P_IDLE: if (timer == PERIOD_C) begin
                    timer <= '0;
                    y     <= '0;
                    st    <= P_HDR;
                end
                P_HDR: if (rd) begin
                    q  <= {1'b1, 20'd0, 1'b0, (y == 10'd0), y};
                    xw <= '0;
                    st <= P_PIX;
                end
                P_PIX: if (rd) begin
                    q  <= {1'b0, p1, p0};
                    xw <= xw + 10'd1;
                    if (xw == H_WORDS_LAST) begin
                        if (y == V_LAST) st <= P_END;
                        else begin y <= y + 10'd1; st <= P_HDR; end
                    end
                end
                P_END: if (rd) begin
                    q  <= {1'b1, 20'd0, 1'b1, 10'd0, 1'b1};
                    f  <= (f == FB_PAT_FRAMES - 1) ? 8'd0 : f + 8'd1;
                    st <= P_IDLE;
                end
            endcase
        end
    end
endmodule
