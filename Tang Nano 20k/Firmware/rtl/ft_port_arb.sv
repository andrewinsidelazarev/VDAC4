// SPDX-License-Identifier: Apache-2.0
//
// Порт слов ft_core — двум клиентам: окну LINK (ESP, a) и сопроцессору BL616 (b, cp_link).
// Запрос клиента держится до его p_ack; выбор держится до ответа ft_core, b — первым.
module ft_port_arb (
    input  wire        clk,
    input  wire        rst_n,

    input  wire        a_req,
    input  wire        a_we,
    input  wire [21:2] a_addr,
    input  wire [31:0] a_wdata,
    output wire        a_ack,

    input  wire        b_req,
    input  wire        b_we,
    input  wire [21:2] b_addr,
    input  wire [31:0] b_wdata,
    output wire        b_ack,

    output wire        p_req,
    output wire        p_we,
    output wire        p_cp,              // запрос сопроцессора
    output wire [21:2] p_addr,
    output wire [31:0] p_wdata,
    input  wire        p_ack
);
    reg busy, sel_b;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            busy <= 1'b0; sel_b <= 1'b0;
        end else if (!busy) begin
            if (b_req) begin
                busy <= 1'b1; sel_b <= 1'b1;
            end else if (a_req) begin
                busy <= 1'b1; sel_b <= 1'b0;
            end
        end else if (p_ack) begin
            busy <= 1'b0;
        end
    end
    assign p_req   = busy && (sel_b ? b_req : a_req);
    assign p_we    = sel_b ? b_we : a_we;
    assign p_cp    = sel_b;
    assign p_addr  = sel_b ? b_addr : a_addr;
    assign p_wdata = sel_b ? b_wdata : a_wdata;
    assign a_ack   = p_ack && !sel_b;
    assign b_ack   = p_ack && sel_b;
endmodule
