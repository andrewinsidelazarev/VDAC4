// SPDX-License-Identifier: Apache-2.0
// DVI/HDMI TMDS 8b/10b video channel encoder.
module tmds_encoder #(parameter bit SHORT_PATH = 1'b0) (
    input  wire       pixel_clk,
    input  wire       reset_n,
    input  wire [7:0] video_data,
    input  wire [1:0] control_data,
    input  wire       data_enable,
    output reg  [9:0] tmds
);
    generate if (SHORT_PATH) begin : g_short
        // Накопленное отклонение TMDS всегда чётное: хранится половина. Выбор XNOR — большинство
        // среди битов 7:1, бит 0 сокращается.
        wire [3:0] ones7 = ({3'd0, video_data[1]} + {3'd0, video_data[2]} +
                             {3'd0, video_data[3]}) +
                            ({3'd0, video_data[4]} + {3'd0, video_data[5]} +
                             {3'd0, video_data[6]} + {3'd0, video_data[7]});
        wire xn = ones7 >= 4'd4;
        wire [7:0] px;
        for (genvar b = 0; b < 8; b = b + 1) begin : g_prefix
            assign px[b] = ^video_data[b:0];
        end
        wire [2:0] even_ones = ({2'd0, px[0]} + {2'd0, px[2]}) +
                              ({2'd0, px[4]} + {2'd0, px[6]});
        wire [2:0] odd_ones = ({2'd0, px[1]} + {2'd0, px[3]}) +
                             ({2'd0, px[5]} + {2'd0, px[7]});
        // XNOR лишь инвертирует нечётные позиции префикса: оба баланса считаются параллельно,
        // выбор — уже по большинству.
        wire signed [3:0] bal_xor = $signed({1'b0, even_ones}) +
                                     $signed({1'b0, odd_ones}) - 4'sd4;
        wire signed [3:0] bal_xnor = $signed({1'b0, even_ones}) -
                                      $signed({1'b0, odd_ones});
        wire signed [3:0] bal = xn ? bal_xnor : bal_xor;
        wire [7:0] qm = px ^ (xn ? 8'hAA : 8'h00);
        reg signed [3:0] rd;
        always @(posedge pixel_clk or negedge reset_n) begin
            if (!reset_n) begin
                tmds <= 10'b1101010100;
                rd <= 0;
            end else if (!data_enable) begin
                rd <= 0;
                case (control_data)
                    2'b00: tmds <= 10'b1101010100;
                    2'b01: tmds <= 10'b0010101011;
                    2'b10: tmds <= 10'b0101010100;
                    default: tmds <= 10'b1010101011;
                endcase
            end else if (rd == 0 || bal == 0) begin
                tmds <= {xn, !xn, xn ? ~qm : qm};
                rd <= rd + (xn ? -bal : bal);
            end else if (rd[3] == bal[3]) begin
                tmds <= {1'b1, !xn, ~qm};
                rd <= rd + (xn ? 4'sd0 : 4'sd1) - bal;
            end else begin
                tmds <= {1'b0, !xn, qm};
                rd <= rd - (xn ? 4'sd1 : 4'sd0) + bal;
            end
        end
    end else begin : g_original
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
    end endgenerate
endmodule
