`timescale 1ns/1ps

module tb_video;
    reg clk = 0;
    reg reset_n = 1;
    wire [9:0] x, y;
    wire hs, vs, de, frame_tick;
    wire [9:0] ax;
    wire [8:0] ay;
    wire [7:0] r8, g8, b8;
    wire [9:0] encoded;
    integer pixels = 0;
    integer active = 0;
    integer errors = 0;

    always #18.518 clk = ~clk;

    video_timing_480p timing(
        .pixel_clk(clk), .reset_n(reset_n), .x(x), .y(y),
        .hsync(hs), .vsync(vs), .de(de), .frame_tick(frame_tick),
        .active_x(ax), .active_y(ay)
    );
    legacy_palette palette(
        .pal_sel(1'b0), .r5(5'd1), .g5(5'd24), .b5(5'd2),
        .r8(r8), .g8(g8), .b8(b8)
    );
    tmds_encoder encoder(
        .pixel_clk(clk), .reset_n(reset_n), .video_data(8'h00),
        .control_data(2'b00), .data_enable(1'b0), .tmds(encoded)
    );

    always @(posedge clk) begin
        if (reset_n) begin
            pixels = pixels + 1;
            if (de) active = active + 1;
        end
    end

    initial begin
        #1 reset_n = 0;
        #80 reset_n = 1;
        wait (frame_tick);
        @(negedge clk);
        pixels = 0;
        active = 0;
        wait (frame_tick);
        @(negedge clk);
        // The boundary cycle containing frame_tick is excluded at both ends.
        if (pixels != 858*525-1) begin
            $display("FAIL frame pixels=%0d", pixels);
            errors = errors + 1;
        end
        if (active != 720*480) begin
            $display("FAIL active pixels=%0d", active);
            errors = errors + 1;
        end
        if (r8 !== 8'd11 || g8 !== 8'hFF || b8 !== 8'd21) begin
            $display("FAIL palette %0d %0d %0d", r8, g8, b8);
            errors = errors + 1;
        end
        if (encoded !== 10'b1101010100) begin
            $display("FAIL control TMDS=%b", encoded);
            errors = errors + 1;
        end
        if (errors == 0)
            $display("PASS tb_video");
        else
            $fatal(1, "tb_video errors=%0d", errors);
        $finish;
    end
endmodule
