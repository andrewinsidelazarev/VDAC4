// SPDX-License-Identifier: GPL-3.0-or-later
// VDAC4 A1200 bring-up: bounded ROM packets, HID transport and stereo audio.
// Register addresses in this module are WORD addresses; host uses byte addresses.
module amiga_link (
    input wire clk, rst_n, sck, cs_n,
    input wire [3:0] io_in,
    output wire [3:0] io_out, output wire io_oe,
    output wire sd_cs_n, sd_sck, sd_mosi, input wire sd_miso,
    input wire clk7_en, clk7n_en,
    input wire [14:0] audio_left, audio_right,
    input wire [7:0] key_free,
    input wire [31:0] debug_words [0:35],
    output reg [8:0] trace_index,
    output reg [31:0] trace_start,
    output reg [31:0] video_probe,
    input wire disk_reset,
    input wire [7:0] disk_rd, disk_wr, input wire [31:0] disk_sector,
    output wire [7:0] disk_mounted, disk_data, output wire [63:0] disk_size,
    output wire disk_busy, disk_done, disk_strobe, output wire [8:0] disk_addr,
    output reg hid_strobe, hid_start, hid_reset, output reg [7:0] hid_data,
    output reg boot_req, output wire soft_reset,
    output reg rom_ready, output reg rom_we,
    output reg [17:0] rom_addr, output reg [15:0] rom_word
);
    wire wr_stb;
    wire [17:0] wr_addr;
    wire [31:0] wr_data;
    wire [12:0] rd_addr;
    reg [31:0] rd_data;
    next_qspi qspi(.sck(sck), .cs_n(cs_n), .io_in(io_in), .io_out(io_out),
        .io_oe(io_oe), .clk(clk), .rst_n(rst_n), .wr_stb(wr_stb),
        .wr_addr(wr_addr), .wr_data(wr_data), .rd_addr(rd_addr), .rd_data(rd_data));

    wire [31:0] disk_status;
    amiga_disk disk(.clk(clk),.rst_n(rst_n),.guest_reset(disk_reset),
        .wr_stb(wr_stb),.wr_addr(wr_addr),.wr_data(wr_data),.rd_addr(rd_addr),.rd_data(disk_status),
        .request_rd(disk_rd),.request_wr(disk_wr),.request_sector(disk_sector),
        .image_mounted(disk_mounted),.image_size(disk_size),.busy(disk_busy),.done(disk_done),
        .byte_strobe(disk_strobe),.byte_addr(disk_addr),.byte_data(disk_data));

    // Standard VDAC4 microSD SPI bridge, ABI identical to TS-Config and NES.
    wire [31:0] sd_ctrl_rd, sd_ptr_rd, sd_stat, sd_rx_q;
    fb_sdspi #(.CLK_KHZ(28500)) sdspi(.clk(clk),.rst_n(rst_n),
        .sd_cs_n(sd_cs_n),.sd_sck(sd_sck),.sd_mosi(sd_mosi),.sd_miso(sd_miso),
        .ctrl_we(wr_stb && wr_addr==18'h40),.ptr_we(wr_stb && wr_addr==18'h41),
        .cmd_we(wr_stb && wr_addr==18'h42),.wdata(wr_data),
        .ctrl_rd(sd_ctrl_rd),.ptr_rd(sd_ptr_rd),.status(sd_stat),
        .tx_we(wr_stb && wr_addr[17:9]==9'd4),.tx_waddr(wr_addr[8:0]),.tx_wdata(wr_data),
        .rd_clk(sck),.rd_addr(rd_addr[8:0]),.rd_q(sd_rx_q));

    reg [31:0] packet[0:63];
    reg [31:0] packet_q;
    reg [63:0] filled;
    reg [7:0] pos, first_byte;
    reg [3:0] pace;
    reg loading, busy, error, pending, have_first, expected_set;
    reg [15:0] sequence_no;
    reg [31:0] received, crc, expected_crc, scratch;
    reg [7:0] reset_count;
    assign soft_reset = |reset_count;
    wire [7:0] next_byte = packet_q >> (8 * pos[1:0]);
    function automatic [31:0] crc_byte(input [31:0] c, input [7:0] b);
        reg [31:0] v;
        begin
            v = c ^ {24'b0,b};
            for (integer i=0; i<8; i=i+1)
                v = (v >> 1) ^ (v[0] ? 32'hedb88320 : 32'd0);
            crc_byte = v;
        end
    endfunction
    always @(posedge clk) begin
        if (wr_stb && wr_addr >= 18'h1000 && wr_addr < 18'h1040 && loading && !busy)
            packet[wr_addr[5:0]] <= wr_data;
        packet_q <= packet[pos[7:2]];
    end
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            filled<=0; pos<=0; pace<=0; first_byte<=0; have_first<=0;
            loading<=0; busy<=0; error<=0; pending<=0; sequence_no<=0;
            received<=0; crc<=32'hffffffff; expected_crc<=0; expected_set<=0;
            rom_ready<=0; rom_we<=0; rom_addr<=0; rom_word<=0;
            boot_req<=0; scratch<=0; reset_count<=0; trace_index<=0; trace_start<=0;
            video_probe<=0;
            hid_strobe<=0; hid_start<=0; hid_data<=0; hid_reset<=0;
        end else begin
            hid_strobe<=0; hid_reset<=0;
            // Diagnostic output only; a guest reset restores the ordinary picture.
            if (disk_reset) video_probe<=0;
            if (reset_count != 0) reset_count<=reset_count-1'b1;
            // Same phase schedule as NanoMig's flash-to-SDRAM writer.
            // Data/address stay constant until the falling 7 MHz enable.
            if (clk7_en && pending) begin rom_we<=1; pending<=0; end
            if (clk7n_en && rom_we) begin rom_we<=0; rom_addr<=rom_addr+1'b1; end
            if (busy) begin
                pace<=pace+1'b1;
                if (pace==8) begin
                    crc<=crc_byte(crc,next_byte);
                    received<=received+1'b1;
                    if (!have_first) first_byte<=next_byte;
                    else begin rom_word<={first_byte,next_byte}; pending<=1; end
                    have_first<=!have_first;
                end
                if (pace==15) begin
                    if (pos==255) begin busy<=0; filled<=0; end
                    else pos<=pos+1'b1;
                end
            end
            if (wr_stb) begin
                if (wr_addr>=18'h1000 && wr_addr<18'h1040) begin
                    if (loading && !busy) filled[wr_addr[5:0]]<=1;
                    else begin error<=1; rom_ready<=0; end
                end
                case (wr_addr)
                    18'h04: begin hid_strobe<=1; hid_start<=wr_data[8]; hid_data<=wr_data[7:0]; end
                    18'h07: hid_reset<=1;
                    18'h0f: scratch<=wr_data;
                    18'h11: begin
                        if (wr_data==1 && !busy && !pending && !rom_we) begin
                            loading<=1; rom_ready<=0; sequence_no<=0; received<=0;
                            crc<=32'hffffffff; error<=0; expected_set<=0;
                            rom_addr<=0; filled<=0; have_first<=0;
                        end else if (wr_data==2) begin
                            if (loading && !busy && !pending && !rom_we && !error &&
                                received==524288 && expected_set && ~crc==expected_crc) begin
                                loading<=0; rom_ready<=1;
                            end else begin error<=1; rom_ready<=0; end
                        end else if (wr_data==3 && rom_ready) reset_count<=255;
                    end
                    18'h12: begin
                        // A repeated COMMIT is idempotent, including while busy.
                        if (wr_data[31:16]!=sequence_no) begin
                            if (loading && !busy && !pending && !rom_we && !error &&
                                wr_data[31:16]==sequence_no+16'd1 && wr_data[15:0]==256 &&
                                received<524288 && &filled) begin
                                sequence_no<=wr_data[31:16]; pos<=0; pace<=0; busy<=1;
                            end else begin error<=1; rom_ready<=0; end
                        end
                    end
                    18'h16: if (loading && !busy) begin expected_crc<=wr_data; expected_set<=1; end
                    18'h3c: if (wr_data==32'h424f4f54) boot_req<=1;
                    18'h3d: trace_index<=wr_data[8:0];
                    18'h3e: trace_start<=wr_data;
                    18'h68: if (!disk_reset) video_probe<=wr_data;
                    default: ;
                endcase
            end
        end
    end

    // 1024 stereo frames, exact 48000 Hz average from the 28.5 MHz system clock.
    reg [24:0] audio_phase;
    reg [9:0] audio_wp, audio_gray;
    reg [31:0] audio_ram[0:1023];
    reg [31:0] audio_q;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin audio_phase<=0; audio_wp<=0; audio_gray<=0; end
        else begin
            if (audio_phase >= 28500000-48000) begin
                audio_phase<=audio_phase+48000-28500000;
                audio_wp<=audio_wp+1'b1;
            end else audio_phase<=audio_phase+48000;
            // Publish pointer one cycle after the RAM write.
            audio_gray<=audio_wp ^ (audio_wp>>1);
        end
    end
    always @(posedge clk)
        if (rst_n && audio_phase>=28500000-48000)
            audio_ram[audio_wp] <= rom_ready && !soft_reset ?
                {audio_right[14],audio_right,audio_left[14],audio_left} : 32'b0;
    always @(posedge sck) audio_q<=audio_ram[rd_addr[9:0]];
    always @* begin
        rd_data=0;
        if (rd_addr>=13'h1800 && rd_addr<13'h1c00) rd_data=audio_q;
        else if (rd_addr>=13'h0800 && rd_addr<13'h0a00) rd_data=sd_rx_q;
        else if (rd_addr>=13'h18 && rd_addr<13'h3c) rd_data=debug_words[rd_addr-13'h18];
        else if (rd_addr>=13'h60 && rd_addr<=13'h66) rd_data=disk_status;
        else case (rd_addr)
            13'h00: rd_data=32'h56444334;
            13'h01: rd_data=32'h05010001;
            13'h02: rd_data={29'd0,rom_ready,boot_req,error};
            13'h03: rd_data={24'd0,key_free};
            13'h08: rd_data={22'd0,audio_gray};
            13'h09: rd_data=48000;
            13'h0a: rd_data=28500;
            13'h0f: rd_data=scratch;
            13'h11: rd_data={28'd0,error,rom_ready,busy,loading};
            13'h12: rd_data={16'd0,sequence_no};
            13'h13: rd_data=received;
            13'h14: rd_data=~crc;
            13'h15: rd_data={31'd0,error};
            13'h16: rd_data=expected_crc;
            13'h3d: rd_data={23'b0,trace_index};
            13'h3e: rd_data=trace_start;
            13'h40: rd_data=sd_ctrl_rd;
            13'h41: rd_data=sd_ptr_rd;
            13'h42: rd_data=sd_stat;
            13'h68: rd_data=video_probe;
            13'h69: rd_data=32'h56505231; // VPR1: A5RRGGBB solid-output probe
            default: ;
        endcase
    end
endmodule
