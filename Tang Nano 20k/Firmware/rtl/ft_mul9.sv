// SPDX-License-Identifier: Apache-2.0
//
// Умножение 9 × 9 — примитив MULT9X9 Gowin (их четыре в блоке DSP). Иначе синтез
// кладёт умножение 8 × 8, за которым идёт сложение, в целый MULTADDALU18X18, и блоков DSP
// не хватает (растеризатор FT812: смешение, цвет текселя).
// REG = 1 — регистры входов и выхода внутри DSP: произведение входов такта t — на выходе
// в такте t + 2. Так стадии конвейера закреплены в самом блоке и синтез не переносит их.
// SIGNED = 1 — a и b со знаком (дополнительный код), p — произведение со знаком.
module ft_mul9 #(
    parameter bit REG    = 1'b0,
    parameter bit SIGNED = 1'b0
) (
    input  wire        clk,
    input  wire [8:0]  a,
    input  wire [8:0]  b,
    output wire [17:0] p
);
`ifdef SIMULATION
    // младшие 18 разрядов произведения расширенных до 18 разрядов — произведение со знаком
    function automatic [17:0] ext(input [8:0] v);
        ext = SIGNED ? {{9{v[8]}}, v} : {9'd0, v};
    endfunction
    generate if (REG) begin : g_reg
        reg [8:0]  ra, rb;
        reg [17:0] rp;
        always @(posedge clk) begin
            ra <= a;
            rb <= b;
            rp <= ext(ra) * ext(rb);
        end
        assign p = rp;
    end else begin : g_comb
        assign p = ext(a) * ext(b);
    end endgenerate
`else
    MULT9X9 #(
        .AREG(REG), .BREG(REG), .OUT_REG(REG), .PIPE_REG(1'b0),
        .ASIGN_REG(1'b0), .BSIGN_REG(1'b0), .SOA_REG(1'b0), .MULT_RESET_MODE("SYNC")
    ) m (
        .DOUT(p), .SOA(), .SOB(), .A(a), .B(b), .SIA(9'd0), .SIB(9'd0),
        .ASEL(1'b0), .BSEL(1'b0), .ASIGN(SIGNED), .BSIGN(SIGNED),
        .CLK(clk), .CE(1'b1), .RESET(1'b0)
    );
`endif
endmodule
