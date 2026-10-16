// SPDX-License-Identifier: Apache-2.0
//
// Переходы между доменами для настроек и состояния кадрового буфера.
//
// cdc_bus  — шина, которая меняется редко (настройки из меню). Два триггера на бит,
//            затем значение принимается, только если два такта подряд одинаково: шина,
//            пойманная посреди смены, не попадёт на выход.
// cdc_view — шина состояния для экранного меню: два триггера на бит без фильтра.
//            Рваное значение может показаться на один кадр — для индикации это безвредно.
//
// Атрибут syn_srlstyle = "registers" не даёт синтезу превратить цепочки синхронизаторов
// в сдвиговые регистры на распределённой памяти (SSRAM): первый каскад должен быть триггером.
//
module cdc_bus #(
    parameter int W = 8,
    parameter logic [W-1:0] INIT = '0
) (
    input  wire         clk,
    input  wire         rst_n,
    input  wire [W-1:0] d,
    output reg  [W-1:0] q /* synthesis syn_srlstyle = "registers" */
);
    reg [W-1:0] s1, s2, s3 /* synthesis syn_srlstyle = "registers" */;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            s1 <= INIT; s2 <= INIT; s3 <= INIT; q <= INIT;
        end else begin
            s1 <= d; s2 <= s1; s3 <= s2;
            if (s3 == s2) q <= s3;
        end
    end
endmodule

module cdc_view #(
    parameter int W = 8
) (
    input  wire         clk,
    input  wire [W-1:0] d,
    output reg  [W-1:0] q /* synthesis syn_srlstyle = "registers" */
);
    reg [W-1:0] s1 /* synthesis syn_srlstyle = "registers" */;
    always_ff @(posedge clk) begin
        s1 <= d;
        q  <= s1;
    end
endmodule

// Частота PCLK Evo в кГц, домен пикселей. Окно — WINDOW тактов пиксельного клока (1 мс
// при 27 МГц): на каждой границе окна переключатель уходит в домен захвата, тот отдаёт
// число своих тактов (56 МГц = 2 × PCLK) за прошлое окно и возвращает переключатель.
// Если за окно возврат не пришёл (PCLK пропал, домен захвата стоит или в сбросе) —
// показывается 0.
module pclk_meter #(
    parameter int WINDOW = 27_000
) (
    input  wire        pixel_clk,
    input  wire        rst_pix_n,
    input  wire        clk_cap,
    input  wire        rst_cap_n,
    output reg  [15:0] khz
);
    localparam [16:0] WINDOW_LAST = WINDOW - 1;

    reg [16:0] wcnt;
    reg        tog;
    reg [2:0]  echo_s /* synthesis syn_srlstyle = "registers" */;
    reg        fresh;                  // за это окно возврат пришёл
    reg [16:0] cnt_s1, cnt_s2 /* synthesis syn_srlstyle = "registers" */;

    // Домен захвата
    reg [2:0]  tog_s /* synthesis syn_srlstyle = "registers" */;
    reg [16:0] ccnt, cap_count;
    reg        echo;
    always_ff @(posedge clk_cap or negedge rst_cap_n) begin
        if (!rst_cap_n) begin
            tog_s <= '0; ccnt <= '0; cap_count <= '0; echo <= 1'b0;
        end else begin
            tog_s <= {tog_s[1:0], tog};
            if (tog_s[2] ^ tog_s[1]) begin
                cap_count <= ccnt;
                ccnt      <= 17'd1;
                echo      <= tog_s[1];
            end else if (ccnt != 17'h1FFFF) begin
                ccnt <= ccnt + 17'd1;
            end
        end
    end

    // Домен пикселей
    always_ff @(posedge pixel_clk or negedge rst_pix_n) begin
        if (!rst_pix_n) begin
            wcnt <= '0; tog <= 1'b0; echo_s <= '0; fresh <= 1'b0;
            cnt_s1 <= '0; cnt_s2 <= '0; khz <= '0;
        end else begin
            echo_s <= {echo_s[1:0], echo};
            cnt_s1 <= cap_count;           // стабильна почти всё окно
            cnt_s2 <= cnt_s1;
            if (echo_s[2] ^ echo_s[1]) fresh <= 1'b1;
            if (wcnt == WINDOW_LAST) begin
                wcnt  <= '0;
                khz   <= fresh ? cnt_s2[16:1] : 16'd0;
                fresh <= 1'b0;
                tog   <= ~tog;
            end else begin
                wcnt <= wcnt + 17'd1;
            end
        end
    end
endmodule
