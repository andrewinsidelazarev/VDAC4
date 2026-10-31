// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only DF0 sector mailbox. All ports are synchronous to clk (28.5 MHz).
// Host addresses are WORD addresses. Guest reset cancels I/O, retaining media.
module amiga_disk (
    input wire clk, rst_n, guest_reset,
    input wire wr_stb, input wire [17:0] wr_addr, input wire [31:0] wr_data,
    input wire [12:0] rd_addr, output reg [31:0] rd_data,
    input wire [7:0] request_rd, request_wr, input wire [31:0] request_sector,
    output reg [7:0] image_mounted, output reg [63:0] image_size,
    output wire busy, output reg done, byte_strobe,
    output reg [8:0] byte_addr, output wire [7:0] byte_data
);
    localparam IDLE=0, WAIT_HOST=1, SEND=2, FINISH=3;
    reg [1:0] state, pace;
    reg [31:0] sector, token, completed;
    reg [7:0] drive;
    reg writing, error;
    reg [127:0] filled;
    reg [31:0] buffer [0:127];
    reg [31:0] data_q;
    wire payload = wr_addr >= 18'h1400 && wr_addr < 18'h1480;
    assign busy = state != IDLE;
    assign byte_data = data_q >> (8 * byte_addr[1:0]);
    always @(posedge clk) begin
        if (wr_stb && payload && state==WAIT_HOST && !guest_reset)
            buffer[wr_addr[6:0]] <= wr_data;
        data_q <= buffer[byte_addr[8:2]];
    end
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state<=IDLE; pace<=0; sector<=0; token<=0; completed<=0;
            drive<=0; writing<=0; error<=0; filled<=0;
            image_mounted<=0; image_size<=0; done<=0; byte_strobe<=0; byte_addr<=0;
        end else begin
            image_mounted<=0; done<=0; byte_strobe<=0;
            // A pulse tells Paula that media has changed, including during reset.
            if (wr_stb && wr_addr==18'h60 && state==IDLE) begin
                if (wr_data==0 || wr_data==901120) begin
                    image_size<={32'b0,wr_data}; image_mounted<=1; error<=0;
                end else error<=1;
            end
            if (guest_reset) begin
                state<=IDLE; filled<=0; pace<=0; byte_addr<=0; error<=0;
            end else begin
                case (state)
                    IDLE: if (|request_rd || |request_wr) begin
                        state<=WAIT_HOST; sector<=request_sector;
                        drive<=request_rd | request_wr; writing<=|request_wr;
                        token<=token+1'b1; filled<=0;
                    end
                    WAIT_HOST: begin
                        if (wr_stb && payload) filled[wr_addr[6:0]]<=1;
                        if (wr_stb && wr_addr==18'h64) begin
                            // Incomplete or stale replies must never reach guest RAM.
                            if (wr_data==token && &filled && !writing && drive==1 &&
                                image_size==901120 && sector<1760) begin
                                state<=SEND; byte_addr<=0; pace<=0;
                            end else error<=1;
                        end
                    end
                    SEND: begin
                        pace<=pace+1'b1;
                        if (pace==2) byte_strobe<=1;
                        if (pace==3) begin
                            if (byte_addr==511) state<=FINISH;
                            else byte_addr<=byte_addr+1'b1;
                        end
                    end
                    FINISH: begin state<=IDLE; done<=1; completed<=token; end
                endcase
            end
        end
    end
    always @* begin
        rd_data=0;
        case (rd_addr)
            13'h60: rd_data=image_size[31:0];
            13'h61: rd_data={16'b0,drive,4'b0,writing,error,busy,state==WAIT_HOST};
            13'h62: rd_data=sector;
            13'h63: rd_data=token;
            13'h64: rd_data=completed;
            13'h66: rd_data=32'h41444631; // ADF1 protocol, independent of core VERSION
            default: ;
        endcase
    end
endmodule
