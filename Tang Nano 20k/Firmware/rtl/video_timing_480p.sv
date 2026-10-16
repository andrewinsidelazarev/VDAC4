// SPDX-License-Identifier: Apache-2.0
// CEA-861 720x480p timing at 27 MHz (59.94/60 Hz tolerant sink timing).
module video_timing_480p (
    input  wire        pixel_clk,
    input  wire        reset_n,
    output reg  [9:0]  x,
    output reg  [9:0]  y,
    output wire        hsync,
    output wire        vsync,
    output wire        de,
    output wire        frame_tick,
    output wire [9:0]  active_x,
    output wire [8:0]  active_y
);
    localparam H_TOTAL = 858;
    localparam H_SYNC = 62;
    localparam H_ACTIVE_START = 122;
    localparam H_ACTIVE_END = H_ACTIVE_START + 720;
    localparam V_TOTAL = 525;
    localparam V_SYNC = 6;
    localparam V_ACTIVE_START = 36;
    localparam V_ACTIVE_END = V_ACTIVE_START + 480;

    always @(posedge pixel_clk or negedge reset_n) begin
        if (!reset_n) begin
            x <= 0;
            y <= 0;
        end else if (x == H_TOTAL-1) begin
            x <= 0;
            y <= (y == V_TOTAL-1) ? 0 : y + 1'b1;
        end else begin
            x <= x + 1'b1;
        end
    end

    assign hsync = !(x < H_SYNC);
    assign vsync = !(y < V_SYNC);
    assign de = (x >= H_ACTIVE_START) && (x < H_ACTIVE_END) &&
                (y >= V_ACTIVE_START) && (y < V_ACTIVE_END);
    assign frame_tick = (x == 0) && (y == 0);
    assign active_x = x - H_ACTIVE_START;
    assign active_y = y - V_ACTIVE_START;
endmodule
