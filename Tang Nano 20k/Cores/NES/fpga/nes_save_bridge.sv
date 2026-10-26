// SPDX-License-Identifier: Apache-2.0
// Сохранения: остановленный NES, пакеты 256 байтов через штатный порт B SDRAM.
// 58: удержание NES в reset; 5c: смещение RAM (0..3ffff, EEPROM 40000..400ff).
// 60: W {seq[15:0], write[15], 6'b0, count[8:0]}, R последний завершённый seq.
// 64: CRC32 пакета; 68: {ss_mem[3], error[2], busy[1], hold[0]}; 5000..50fc: буфер.
// 6c (VDAC4 0.8, снимок): W {ss_mem[0]} — SDRAM мосту при NES на паузе (без сброса), смещение
//     5c — полный адрес SDRAM (22 бита, кратен 256); снимается сам, когда пауза кончилась.
module nes_save_bridge(
    input wire clk, rst_n, sck, wr_stb,
    input wire [17:0] wr_addr,
    input wire [31:0] wr_data,
    input wire [12:0] rd_addr,
    output reg [31:0] rd_data,
    input wire paused,
    output reg hold_nes, ss_mem,
    output wire [21:0] mem_offset,
    output wire mem_read, mem_write,
    output wire [7:0] mem_dout,
    input wire [7:0] mem_din
);
    reg [31:0] buffer[0:63] /* synthesis syn_ramstyle="block_ram" */;
    reg [31:0] host_q, ram_q, acc;
    reg [21:0] offset, base;
    reg [8:0] count, pos;
    reg [5:0] phase;
    reg busy, error, writing;
    reg [15:0] requested_seq, completed_seq;
    reg [31:0] crc;
    wire [7:0] out_byte = ram_q >> (8 * pos[1:0]);
    assign mem_offset = base + {13'b0,pos};
    assign mem_dout = out_byte;
    // 64 такта NES на байт. Запрос держится 16 тактов, затем достаточно времени
    // для выборки и снятия edge detector контроллера во всех фазах clkref.
    assign mem_read = busy && !writing && phase >= 8 && phase < 24;
    assign mem_write = busy && writing && phase >= 8 && phase < 24;
    function automatic [31:0] crc_byte(input [31:0] c, input [7:0] b);
        reg [31:0] v;
        begin
            v=c ^ {24'b0,b};
            for(integer i=0;i<8;i=i+1) v=(v>>1) ^ (v[0] ? 32'hedb88320 : 0);
            crc_byte=v;
        end
    endfunction
    always @(posedge clk) begin
        ram_q <= buffer[pos[7:2]];
        if (wr_stb && wr_addr>=18'h1400 && wr_addr<18'h1440 && !busy)
            buffer[wr_addr[5:0]] <= wr_data;
        else if (busy && !writing && phase==48 && (pos[1:0]==3 || pos+1'b1==count))
            buffer[pos[7:2]] <= {mem_din,acc[31:8]};
    end
    always @(posedge sck) host_q <= buffer[rd_addr[5:0]];
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            hold_nes<=0; ss_mem<=0; offset<=0; base<=0; count<=0; pos<=0; phase<=0;
            busy<=0; error<=0; writing<=0; requested_seq<=0; completed_seq<=0;
            crc<=32'hffffffff; acc<=0;
        end else begin
            if (busy) begin
                phase<=phase+1'b1;
                if (phase==48) begin
                    crc<=crc_byte(crc,writing ? out_byte : mem_din);
                    acc<={mem_din,acc[31:8]};
                end
                if (phase==63) begin
                    if (pos+1'b1==count) begin busy<=0; completed_seq<=requested_seq; end
                    else pos<=pos+1'b1;
                end
            end
            if (wr_stb) case(wr_addr)
                18'h16: if (!busy && !ss_mem) begin hold_nes<=wr_data[0]; error<=0; end
                18'h17: if (!busy) offset<=wr_data[21:0];
                18'h18: if (wr_data[31:16]!=completed_seq && !busy) begin
                    // Последний пакет EEPROM всегда дополнен до 256 байтов.
                    if ((hold_nes && offset<=22'h40000 || ss_mem) && wr_data[31:16]==completed_seq+16'd1 &&
                        wr_data[8:0]==256 && offset[7:0]==0) begin
                        base<=offset; count<=wr_data[8:0]; writing<=wr_data[15];
                        requested_seq<=wr_data[31:16]; busy<=1; pos<=0; phase<=0;
                        crc<=32'hffffffff; acc<=0; error<=0;
                    end else error<=1;
                end
                18'h1b: if (!busy && !hold_nes) begin ss_mem<=wr_data[0] && paused; error<=0; end
                default: ;
            endcase
            if (!paused) ss_mem<=0;
        end
    end
    always @* begin
        rd_data=0;
        if (rd_addr>=13'h1400 && rd_addr<13'h1440) rd_data=host_q;
        else case(rd_addr)
            13'h16: rd_data={31'b0,hold_nes};
            13'h17: rd_data={10'b0,offset};
            13'h18: rd_data={16'b0,completed_seq};
            13'h19: rd_data=~crc;
            13'h1a: rd_data={28'b0,ss_mem,error,busy,hold_nes};
            13'h1b: rd_data={31'b0,ss_mem};
            default: ;
        endcase
    end
endmodule
