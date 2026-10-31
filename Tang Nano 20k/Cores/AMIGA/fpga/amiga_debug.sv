// SPDX-License-Identifier: GPL-3.0-or-later
// Passive CPU observer: first eight completed external bus cycles and last cycle.
// A trace entry is address followed by {CPU read data, CPU write data}.
module amiga_debug(
    input wire clk, reset, cpu_reset_n, cpu_clkena,
    input wire [1:0] cpu_state,
    input wire [31:0] cpu_address, status,
    input wire [15:0] cpu_read, cpu_write,
    input wire [23:1] chip_address,
    input wire [15:0] chip_read, chip_write,
    output wire [31:0] words [0:23],
    input wire [8:0] trace_index,
    input wire [31:0] trace_start,
    output wire [31:0] trace_out [0:3]
);
    // Synchronous read/write arrays infer block RAM; no reset of the RAM contents.
    reg [31:0] history_address [0:511], history_data [0:511], history_status [0:511];
    reg [31:0] history_address_q, history_data_q, history_status_q;
    reg [9:0] history_count;
    reg [8:0] history_index_q;
    reg history_valid_q;
    always @(posedge clk) begin
        history_address_q<=history_address[trace_index];
        history_data_q<=history_data[trace_index];
        history_status_q<=history_status[trace_index];
        history_index_q<=trace_index;
        history_valid_q<=!reset && {1'b0,trace_index}<history_count;
        if (!reset && cpu_reset_n && cpu_clkena && cpu_state!=2'b01 &&
            history_count<512 && cycles>=trace_start) begin
            history_address[history_count[8:0]]<=cpu_address;
            history_data[history_count[8:0]]<={cpu_read,cpu_write};
            history_status[history_count[8:0]]<=status;
        end
    end
    assign trace_out[0]=history_valid_q ? history_address_q : 0;
    assign trace_out[1]=history_valid_q ? history_data_q : 0;
    assign trace_out[2]=history_valid_q ? history_status_q : 0;
    assign trace_out[3]={12'b0,history_valid_q,history_index_q,history_count};
    reg [31:0] cycles, last_address, last_data;
    reg [3:0] trace_count;
    reg [31:0] trace_words [0:15];
    always @(posedge clk) begin
        if (reset) begin
            cycles<=0; last_address<=0; last_data<=0; trace_count<=0; history_count<=0;
            for (integer i=0;i<16;i=i+1) trace_words[i]<=0;
        end else if (cpu_reset_n && cpu_clkena && cpu_state!=2'b01) begin
            cycles<=cycles+1'b1;
            last_address<=cpu_address;
            last_data<={cpu_read,cpu_write};
            if (history_count<512 && cycles>=trace_start) history_count<=history_count+1'b1;
            if (trace_count<8) begin
                trace_words[{trace_count[2:0],1'b0}]<=cpu_address;
                trace_words[{trace_count[2:0],1'b1}]<={cpu_read,cpu_write};
                trace_count<=trace_count+1'b1;
            end
        end
    end
    assign words[0]=status;
    assign words[1]=cycles;
    assign words[2]=cpu_address;
    assign words[3]={cpu_read,cpu_write};
    assign words[4]=last_address;
    assign words[5]=last_data;
    assign words[6]={8'b0,chip_address,1'b0};
    assign words[7]={chip_read,chip_write};
    for (genvar j=0;j<16;j=j+1) assign words[8+j]=trace_words[j];
endmodule
