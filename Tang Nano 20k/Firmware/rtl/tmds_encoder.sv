// SPDX-License-Identifier: Apache-2.0
// DVI/HDMI TMDS 8b/10b video channel encoder.
module tmds_encoder (
    input  wire       pixel_clk,
    input  wire       reset_n,
    input  wire [7:0] video_data,
    input  wire [1:0] control_data,
    input  wire       data_enable,
    output reg  [9:0] tmds
);
    reg signed [5:0] disparity;
    reg [8:0] q_m;
    integer i;
    integer ones_data;
    integer ones_qm;
    reg use_xnor;
    reg signed [5:0] balance;

    always @* begin
        ones_data = video_data[0] + video_data[1] + video_data[2] +
                    video_data[3] + video_data[4] + video_data[5] +
                    video_data[6] + video_data[7];
        use_xnor = (ones_data > 4) || ((ones_data == 4) && !video_data[0]);
        q_m[0] = video_data[0];
        for (i = 1; i < 8; i = i + 1)
            q_m[i] = use_xnor ? ~(q_m[i-1] ^ video_data[i]) :
                                (q_m[i-1] ^ video_data[i]);
        q_m[8] = !use_xnor;
        ones_qm = q_m[0] + q_m[1] + q_m[2] + q_m[3] +
                  q_m[4] + q_m[5] + q_m[6] + q_m[7];
        balance = (ones_qm * 2) - 8;
    end

    always @(posedge pixel_clk or negedge reset_n) begin
        if (!reset_n) begin
            tmds <= 10'b1101010100;
            disparity <= 0;
        end else if (!data_enable) begin
            disparity <= 0;
            case (control_data)
                2'b00: tmds <= 10'b1101010100;
                2'b01: tmds <= 10'b0010101011;
                2'b10: tmds <= 10'b0101010100;
                default: tmds <= 10'b1010101011;
            endcase
        end else if ((disparity == 0) || (balance == 0)) begin
            tmds <= {~q_m[8], q_m[8], q_m[8] ? q_m[7:0] : ~q_m[7:0]};
            disparity <= disparity + (q_m[8] ? balance : -balance);
        end else if (((disparity > 0) && (balance > 0)) ||
                     ((disparity < 0) && (balance < 0))) begin
            tmds <= {1'b1, q_m[8], ~q_m[7:0]};
            disparity <= disparity + (q_m[8] ? 6'sd2 : 6'sd0) - balance;
        end else begin
            tmds <= {1'b0, q_m[8], q_m[7:0]};
            disparity <= disparity - (!q_m[8] ? 6'sd2 : 6'sd0) + balance;
        end
    end
endmodule
