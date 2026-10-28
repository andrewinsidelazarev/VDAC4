//-----------------------------------------------------------------
//                      Baseline JPEG Decoder
//                             V0.1
//                       Ultra-Embedded.com
//                        Copyright 2020
//
//                   admin@ultra-embedded.com
//-----------------------------------------------------------------
//                      License: Apache 2.0
// This IP can be freely used in commercial projects, however you may
// want access to unreleased materials such as verification environments,
// or test vectors, as well as changes to the IP for integration purposes.
// If this is the case, contact the above address.
// I am interested to hear how and where this IP is used, so please get
// in touch!
//-----------------------------------------------------------------
// Copyright 2020 Ultra-Embedded.com
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//-----------------------------------------------------------------

module jpeg_idct_x
//-----------------------------------------------------------------
// Params
//-----------------------------------------------------------------
#(
     parameter OUT_SHIFT        = 11
    ,parameter INPUT_WIDTH      = 16
)
//-----------------------------------------------------------------
// Ports
//-----------------------------------------------------------------
(
    // Inputs
     input           clk_i
    ,input           rst_i
    ,input           img_start_i
    ,input           img_end_i
    ,input           inport_valid_i
    ,input  [ 15:0]  inport_data0_i
    ,input  [ 15:0]  inport_data1_i
    ,input  [ 15:0]  inport_data2_i
    ,input  [ 15:0]  inport_data3_i
    ,input  [  2:0]  inport_idx_i

    // Outputs
    ,output          outport_valid_o
    ,output [ 31:0]  outport_data_o
    ,output [  5:0]  outport_idx_o
);




localparam [15:0] C1_16 = 4017; // cos( pi/16) x4096
localparam [15:0] C2_16 = 3784; // cos(2pi/16) x4096
localparam [15:0] C3_16 = 3406; // cos(3pi/16) x4096
localparam [15:0] C4_16 = 2896; // cos(4pi/16) x4096
localparam [15:0] C5_16 = 2276; // cos(5pi/16) x4096
localparam [15:0] C6_16 = 1567; // cos(6pi/16) x4096
localparam [15:0] C7_16 = 799;  // cos(7pi/16) x4096

wire signed [31:0] block_in_0_1 = {{16{inport_data0_i[15]}}, inport_data0_i};
wire signed [31:0] block_in_2_3 = {{16{inport_data1_i[15]}}, inport_data1_i};
wire signed [31:0] block_in_4_5 = {{16{inport_data2_i[15]}}, inport_data2_i};
wire signed [31:0] block_in_6_7 = {{16{inport_data3_i[15]}}, inport_data3_i};

//-----------------------------------------------------------------
// IDCT
//-----------------------------------------------------------------
reg signed [31:0] i0;
// VDAC4: множители 18 x 13 (MULT18X18), а не 32 x 32 (MULTALU36X18 — вдвое больше DSP):
// операнд — коэффициент или сумма двух (у x — 17 бит; у y на настоящих кадрах — до 18),
// константа — cos x 4096 (< 4096). Операнд за пределами 18 бит насыщается (sat18).
reg signed [17:0] mul0_a;
reg signed [12:0] mul0_b;
reg signed [17:0] mul1_a;
reg signed [12:0] mul1_b;
reg signed [17:0] mul2_a;
reg signed [12:0] mul2_b;
reg signed [17:0] mul3_a;
reg signed [12:0] mul3_b;
reg signed [17:0] mul4_a;
reg signed [12:0] mul4_b;
function signed [17:0] sat18;
    input signed [31:0] v;
    sat18 = (v > 32'sd131071) ? 18'sd131071 : (v < -32'sd131072) ? -18'sd131072 : v[17:0];
endfunction
// VDAC4: v * 181 в 32 битах (181 = 128 + 32 + 16 + 4 + 1)
function signed [31:0] mul181;
    input signed [31:0] v;
    mul181 = (v <<< 7) + (v <<< 5) + (v <<< 4) + (v <<< 2) + v;
endfunction

always @ (posedge clk_i )
if (rst_i)
begin
    i0     <= 32'b0;
    mul0_a <= 'b0;
    mul0_b <= 'b0;
    mul1_a <= 'b0;
    mul1_b <= 'b0;
    mul2_a <= 'b0;
    mul2_b <= 'b0;
    mul3_a <= 'b0;
    mul3_b <= 'b0;
    mul4_a <= 'b0;
    mul4_b <= 'b0;
end
else
begin
    /* verilator lint_off WIDTH */
    case (inport_idx_i)
    3'd0:
    begin
        i0     <= block_in_0_1 + block_in_4_5;
        mul0_a <= sat18(block_in_2_3);
        mul0_b <= C2_16;
        mul1_a <= sat18(block_in_6_7);
        mul1_b <= C6_16;
    end
    3'd1:
    begin
        mul0_a <= sat18(block_in_0_1);
        mul0_b <= C1_16;
        mul1_a <= sat18(block_in_6_7);
        mul1_b <= C7_16;
        mul2_a <= sat18(block_in_4_5);
        mul2_b <= C5_16;
        mul3_a <= sat18(block_in_2_3);
        mul3_b <= C3_16;
        mul4_a <= sat18(i0);
        mul4_b <= C4_16;
    end
    3'd2:
    begin
        i0     <= block_in_0_1 - block_in_4_5;
    end
    3'd3:
    begin
        mul0_a <= sat18(block_in_0_1);
        mul0_b <= C7_16;
        mul1_a <= sat18(block_in_6_7);
        mul1_b <= C1_16;
        mul2_a <= sat18(block_in_4_5);
        mul2_b <= C3_16;
        mul3_a <= sat18(block_in_2_3);
        mul3_b <= C5_16;
    end
    3'd4:
    begin
        mul0_a <= sat18(block_in_0_1);
        mul0_b <= C7_16;
        mul1_a <= sat18(block_in_6_7);
        mul1_b <= C1_16;
        mul2_a <= sat18(block_in_4_5);
        mul2_b <= C3_16;
        mul3_a <= sat18(block_in_2_3);
        mul3_b <= C5_16;
    end
    3'd5:
    begin
        mul0_a <= sat18(block_in_2_3);
        mul0_b <= C6_16;
        mul1_a <= sat18(block_in_6_7);
        mul1_b <= C2_16;
        mul4_a <= sat18(i0);
        mul4_b <= C4_16;
    end
    default:
        ;
    endcase
    /* verilator lint_on WIDTH */
end

reg signed [31:0] mul0_q;
reg signed [31:0] mul1_q;
reg signed [31:0] mul2_q;
reg signed [31:0] mul3_q;
reg signed [31:0] mul4_q;

always @ (posedge clk_i )
if (rst_i)
begin
    mul0_q <= 32'b0;
    mul1_q <= 32'b0;
    mul2_q <= 32'b0;
    mul3_q <= 32'b0;
    mul4_q <= 32'b0;
end
else
begin
    mul0_q <= mul0_a * mul0_b;
    mul1_q <= mul1_a * mul1_b;
    mul2_q <= mul2_a * mul2_b;
    mul3_q <= mul3_a * mul3_b;
    mul4_q <= mul4_a * mul4_b;
end

reg signed [31:0] mul0;
reg signed [31:0] mul1;
reg signed [31:0] mul2;
reg signed [31:0] mul3;
reg signed [31:0] mul4;

always @ (posedge clk_i )
if (rst_i)
begin
    mul0 <= 32'b0;
    mul1 <= 32'b0;
    mul2 <= 32'b0;
    mul3 <= 32'b0;
    mul4 <= 32'b0;
end
else
begin
    mul0 <= mul0_q;
    mul1 <= mul1_q;
    mul2 <= mul2_q;
    mul3 <= mul3_q;
    mul4 <= mul4_q;
end

reg        out_stg0_valid_q;
reg [2:0]  out_stg0_idx_q;

always @ (posedge clk_i )
if (rst_i)
begin
    out_stg0_valid_q <= 1'b0;
    out_stg0_idx_q   <= 3'b0;
end
else
begin
    out_stg0_valid_q <= inport_valid_i;
    out_stg0_idx_q   <= inport_idx_i;
end

reg        out_stg1_valid_q;
reg [2:0]  out_stg1_idx_q;

always @ (posedge clk_i )
if (rst_i)
begin
    out_stg1_valid_q <= 1'b0;
    out_stg1_idx_q   <= 3'b0;
end
else
begin
    out_stg1_valid_q <= out_stg0_valid_q;
    out_stg1_idx_q   <= out_stg0_idx_q;
end

reg        out_stg2_valid_q;
reg [2:0]  out_stg2_idx_q;

always @ (posedge clk_i )
if (rst_i)
begin
    out_stg2_valid_q <= 1'b0;
    out_stg2_idx_q   <= 3'b0;
end
else
begin
    out_stg2_valid_q <= out_stg1_valid_q;
    out_stg2_idx_q   <= out_stg1_idx_q;
end

reg signed [31:0] o_s5;
reg signed [31:0] o_s6;
reg signed [31:0] o_s7;
reg signed [31:0] o_t0;
reg signed [31:0] o_t1;
reg signed [31:0] o_t2;
reg signed [31:0] o_t3;
reg signed [31:0] o_t4;
reg signed [31:0] o_t5;
reg signed [31:0] o_t6;
reg signed [31:0] o_t7;
reg signed [31:0] o_t6_5;
reg signed [31:0] o_t5_6;
// VDAC4: 181 * o_t6 и 181 * o_t5 — тактом раньше (ступень 6), на ступени 7 — только разность или
// сумма и деление: по модулю 2^32 (o_t6 -/+ o_t5) * 181 = 181 * o_t6 -/+ 181 * o_t5, итог тот же,
// а цепочка сумматоров за такт вдвое короче (clk_mem 61,875 МГц)
reg signed [31:0] o_p6;
reg signed [31:0] o_p5;

always @ (posedge clk_i )
if (rst_i)
begin
    o_s5   <= 32'b0;
    o_s6   <= 32'b0;
    o_s7   <= 32'b0;
    o_t0   <= 32'b0;
    o_t1   <= 32'b0;
    o_t2   <= 32'b0;
    o_t3   <= 32'b0;
    o_t4   <= 32'b0;
    o_t5   <= 32'b0;
    o_t6   <= 32'b0;
    o_t7   <= 32'b0;
    o_t6_5 <= 32'b0;
    o_t5_6 <= 32'b0;
    o_p6   <= 32'b0;
    o_p5   <= 32'b0;
end
else
begin
    case (out_stg2_idx_q)
    3'd0:
    begin
        o_t3 <= mul0 + mul1; // s3
    end
    3'd1:
    begin
        o_s7 <= mul0 + mul1;
        o_s6 <= mul2 + mul3;
        o_t0 <= mul4;        // s0
    end
    3'd2:
    begin
        o_t0 <= o_t0 + o_t3; // t0
        o_t3 <= o_t0 - o_t3; // t3
        o_t7 <= o_s6 + o_s7;
    end
    3'd3:
    begin
        o_t4 <= (mul0 - mul1) + (mul2 - mul3);
    end
    3'd4:
    begin
        o_t0 <= mul0 - mul1; // s4
        o_s5 <= mul2 - mul3;    
    end
    3'd5:
    begin
        o_t3 <= mul0 - mul1; // s2
        o_t4 <= mul4; // s1
        o_t5 <= o_t0 - o_s5;
        o_t6 <= o_s7 - o_s6;
    end
    3'd6:
    begin
        o_t1 <= o_t4 + o_t3;
        o_t2 <= o_t4 - o_t3;
        o_t6_5 <= o_t6 - o_t5;
        o_t5_6 <= o_t5 + o_t6;
        o_p6   <= mul181(o_t6);
        o_p5   <= mul181(o_t5);
    end
    default:
    begin
        // VDAC4: x 181 сдвигами и сложениями (было MULT36X36 на каждое), произведения — с
        // прошлой ступени (o_p6, o_p5); деление то же
        o_s5 <= (o_p6 - o_p5) / 256; // 1/sqrt(2): (o_t6 - o_t5) * 181 / 256
        o_s6 <= (o_p5 + o_p6) / 256; // 1/sqrt(2): (o_t5 + o_t6) * 181 / 256
    end
    endcase
end

reg        out_stg3_valid_q;
reg [2:0]  out_stg3_idx_q;

always @ (posedge clk_i )
if (rst_i)
begin
    out_stg3_valid_q <= 1'b0;
    out_stg3_idx_q   <= 3'b0;
end
else
begin
    out_stg3_valid_q <= out_stg2_valid_q;
    out_stg3_idx_q   <= out_stg2_idx_q;
end

reg signed [31:0] block_out[0:7];
reg signed [31:0] block_out_tmp;

always @ (posedge clk_i )
if (rst_i)
begin
    block_out[0] <= 32'b0;
    block_out[1] <= 32'b0;
    block_out[2] <= 32'b0;
    block_out[3] <= 32'b0;
    block_out[4] <= 32'b0;
    block_out[5] <= 32'b0;
    block_out[6] <= 32'b0;
    block_out[7] <= 32'b0;
    block_out_tmp <= 32'b0;
end
else if (out_stg3_valid_q)
begin
    if (out_stg3_idx_q == 3'd3)
    begin
        block_out[0] <= ((o_t0 + o_t7) >>> OUT_SHIFT);
        block_out_tmp <= ((o_t0 - o_t7) >>> OUT_SHIFT); // block_out[7]
        block_out[3] <= ((o_t3 + o_t4) >>> OUT_SHIFT);
        block_out[4] <= ((o_t3 - o_t4) >>> OUT_SHIFT);
    end

    if (out_stg3_idx_q == 3'd6)
        block_out[7] <= block_out_tmp;

    if (out_stg3_idx_q == 3'd7)
    begin
        block_out[2] <= ((o_t2 + o_s5) >>> OUT_SHIFT);
        block_out[5] <= ((o_t2 - o_s5) >>> OUT_SHIFT);
        block_out[1] <= ((o_t1 + o_s6) >>> OUT_SHIFT);
        block_out[6] <= ((o_t1 - o_s6) >>> OUT_SHIFT);
    end
end

reg [7:0] valid_q;

always @ (posedge clk_i )
if (rst_i)
    valid_q  <= 8'b0;
else if (img_start_i)
    valid_q  <= 8'b0;
else
    valid_q <= {valid_q[6:0], out_stg3_valid_q};

reg [5:0] ptr_q;

always @ (posedge clk_i )
if (rst_i)
    ptr_q <= 6'd0;
else if (img_start_i)
    ptr_q <= 6'd0;
else if (outport_valid_o)
    ptr_q <= ptr_q + 6'd1;

assign outport_valid_o = valid_q[6];
assign outport_data_o  = block_out[ptr_q[2:0]];

assign outport_idx_o   = ptr_q;



endmodule
