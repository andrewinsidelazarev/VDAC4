// SPDX-License-Identifier: Apache-2.0
//
// Генератор выходной развёртки HDMI, привязанный к входному кадру TS-Config.
//
// Идея «исправления развёртки» без кадрового буфера.
//   Вход в VGA-режиме Evo: 896 отсчётов PCLK на строку (28 МГц / 31,25 кГц),
//   640 строк на кадр, 48,828125 Гц (docs/tsconfig-tang20k-consolidated-notes.md, 10.1).
//   Выход тактируется тем же PCLK, поэтому частоты строк и кадров совпадают со входом
//   бит в бит — кадры не разъезжаются и полный буфер не нужен.
//
//   Исправляется при этом всё, что можно исправить без перекладки кадра во времени:
//     • длительности синхроимпульсов и полки приводятся к раскладке 576p50
//       (H: 64 синхро + 68 задний + 720 актив + 44 передний = 896);
//     • активное окно ровно 720×576 и стоит в известном месте растра;
//     • синхроимпульсы генерируются заново, без дрожания и артефактов входных;
//     • полярность синхро — как в 576p50 (оба отрицательные).
//
//   Что НЕ исправляется этим модулем: кадровая частота остаётся входной,
//   48,83 Гц вместо 50,00 Гц. Это принципиально: без кадрового буфера в SDRAM
//   и повтора кадров частоту не поднять. Строчная частота при этом уже точная —
//   31,25 кГц, ровно как у 576p50, поэтому у приёмника есть шанс принять картинку
//   как «576p с нестандартной кадровой». Проверка на конкретных мониторах открыта.
//
// Привязка к входу.
//   Счётчики перезапускаются импульсом frame_sync, который верхний модуль формирует
//   через V_START_DELAY входных строк после входного VSYNC. Эта задержка выбрана так,
//   чтобы выводимая строка всегда была уже полностью принята (отставание ровно на одну
//   входную строку), иначе строчный буфер читался бы под записью.
//
module ts_video_timing #(
    parameter int H_TOTAL  = 896,  // = входная строка: 28 МГц / 31,25 кГц
    parameter int H_SYNC   = 64,   // как в 576p50
    parameter int H_BACK   = 68,   // как в 576p50
    parameter int H_ACTIVE = 720,
    parameter int V_TOTAL  = 640,  // = входной кадр: 48,83 Гц при 31,25 кГц
    parameter int V_SYNC   = 5,    // как в 576p50
    parameter int V_BACK   = 39,   // как в 576p50
    parameter int V_ACTIVE = 576
) (
    input  wire        pixel_clk,
    input  wire        rst_n,
    input  wire        frame_sync,   // однотактовый импульс начала кадра (домен pixel_clk)

    output reg  [10:0] x,
    output reg  [10:0] y,
    output wire        hsync,        // отрицательная полярность, как в 576p50
    output wire        vsync,
    output wire        de,
    output wire [10:0] active_x,     // 0..H_ACTIVE-1 внутри активного окна
    output wire [10:0] active_y,     // 0..V_ACTIVE-1
    output wire [10:0] next_active_x,// адрес следующего пикселя: память читается с задержкой такт
    output wire [10:0] next_active_y,// номер строки для следующего пикселя (нужен на границе строк)
    output wire        next_de
);

    localparam int H_ACT_START = H_SYNC + H_BACK;
    localparam int H_ACT_END   = H_ACT_START + H_ACTIVE;
    localparam int V_ACT_START = V_SYNC + V_BACK;
    localparam int V_ACT_END   = V_ACT_START + V_ACTIVE;

    // Счётчик следующего такта — нужен, чтобы выставить адрес строчного буфера заранее.
    wire [10:0] x_next = (x == H_TOTAL - 1) ? 11'd0 : x + 11'd1;
    wire [10:0] y_next = (x == H_TOTAL - 1)
                       ? ((y == V_TOTAL - 1) ? 11'd0 : y + 11'd1)
                       : y;

    always_ff @(posedge pixel_clk or negedge rst_n) begin
        if (!rst_n) begin
            x <= '0;
            y <= '0;
        end else if (frame_sync) begin
            // Жёсткая привязка к входному кадру: любой набег фазы снимается раз в кадр.
            x <= '0;
            y <= '0;
        end else begin
            x <= x_next;
            y <= y_next;
        end
    end

    assign hsync = ~(x < H_SYNC);
    assign vsync = ~(y < V_SYNC);

    assign de = (x >= H_ACT_START) && (x < H_ACT_END) &&
                (y >= V_ACT_START) && (y < V_ACT_END);

    assign active_x = x - H_ACT_START[10:0];
    assign active_y = y - V_ACT_START[10:0];

    assign next_de = (x_next >= H_ACT_START) && (x_next < H_ACT_END) &&
                     (y_next >= V_ACT_START) && (y_next < V_ACT_END);
    assign next_active_x = x_next - H_ACT_START[10:0];
    assign next_active_y = y_next - V_ACT_START[10:0];

endmodule
