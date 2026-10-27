// SPDX-License-Identifier: Apache-2.0
// VDAC4 LINK: keyboard/audio ABI shared with Next; a bounded cartridge mailbox.
// 44: R {SDRAM busy[4], loader error[3], done[2], packet busy[1], loading[0]}
//     W 1=start, 2=finish (only after parser DONE, no packet pending).
// 48: W {packet_seq[15:0], byte_count[15:0]} commit; R last accepted packet_seq.
// 4c: R bytes delivered; 50: R CRC32 of delivered bytes; 54: R protocol error.
// 4000..40fc: W 256-byte packet, least significant byte first per word.
// A packet_seq is accepted once. Data stays immutable until the packet completes.
// Status/CRC/count are polled only between packets, after BUSY clears.
// VDAC4 0.7 — меню и карта Tang во время игры:
// c0: W {sel[13:8], show[0]} окно меню (nes_osd); R {16'h4F53, 2'b0, sel, 6'b0, paused, show}.
// c4: W {pause[0]} остановить NES (меню, снимок); R {paused[0]} — машина уже стоит.
// 100, 104, 108: SD_CTRL, SD_PTR, SD_CMD/SD_STAT моста к microSD Tang (fb_sdspi, как в
//     TS-Config 1.30: прошивка ESP монтирует карту тем же кодом); 2000..27fc — его окна TX/RX.
// 3000..37fc: W текст окна меню (512 слов, см. nes_osd).
// 6000..7ffc: R кольцо звука (до 0.7 — 2000..3ffc).
// VDAC4 0.8 — снимки (F5/F6):
// c8: W {адрес слова шины снимка[9:0]}; R {16'h5353, 6'b0, адрес}.
// cc: R слово по адресу c8; W слово (только на паузе), затем адрес c8 +1.
// 6c: мост SDRAM на паузе (см. nes_save_bridge): 5c — полный адрес, пакеты 60/64/5000.
// VDAC4 0.9 — размер картинки (против вылета за край на старых HDMI-приёмниках):
// 84: W {16'h535A, 14'b0, size[1:0]} — 0: 100 %, 1: 95 %, 2: 90 % (как SIZE TS-Config);
//     без метки 535A запись не действует; R {16'h535A, 14'b0, size}. Сброс игры его не меняет.
// VDAC4 0.10 — оформление окна меню (nes_osd), W/R:
// d0: строки на подложке [29:0]; d4: строки с разделителем [29:0];
// d8: полоса хода {вкл[31], строка[30:26], столбец[25:20], столбцов[19:14], 4'b0, заполнено пикселей[9:0]}.
module nes_link(
    input wire sck, cs_n, input wire [3:0] io_in,
    output wire [3:0] io_out, output wire io_oe,
    input wire clk, rst_n,
    output reg [127:0] keyboard, output reg [7:0] joystick0,
    input wire [15:0] sample, output reg boot_req,
    input wire aspect_requested, output reg aspect_restore, output reg aspect_value,
    output reg [1:0] picture_size,
    output wire osd_wr, output wire [8:0] osd_addr, output wire [31:0] osd_data,
    output reg osd_show, output reg [5:0] osd_sel,
    output reg [29:0] osd_band, output reg [29:0] osd_rule, output reg [31:0] osd_bar,
    output reg pause, input wire paused,
    output wire sd_cs_n, sd_sck, sd_mosi, input wire sd_miso,
    output reg loading, output reg [7:0] rom_data, output reg rom_valid,
    input wire loader_done, loader_fail, sdram_busy,
    output wire save_hold, save_full,
    output wire [21:0] save_offset,
    output wire save_read, save_write,
    output wire [7:0] save_dout,
    input wire [7:0] save_din,
    output reg [9:0] ss_addr, output reg [31:0] ss_wdata, output reg ss_we,
    input wire [31:0] ss_rdata
);
    wire wr_stb;
    wire [17:0] wr_addr;
    wire [31:0] wr_data;
    wire [12:0] rd_addr;
    reg [31:0] rd_data;
    next_qspi qspi(.sck(sck), .cs_n(cs_n), .io_in(io_in), .io_out(io_out), .io_oe(io_oe),
        .clk(clk), .rst_n(rst_n), .wr_stb(wr_stb), .wr_addr(wr_addr), .wr_data(wr_data),
        .rd_addr(rd_addr), .rd_data(rd_data));
    wire [31:0] save_rd_data;
    nes_save_bridge saves(.clk(clk),.rst_n(rst_n),.sck(sck),.wr_stb(wr_stb),
        .wr_addr(wr_addr),.wr_data(wr_data),.rd_addr(rd_addr),.rd_data(save_rd_data),
        .paused(paused),.ss_mem(save_full),
        .hold_nes(save_hold),.mem_offset(save_offset),.mem_read(save_read),
        .mem_write(save_write),.mem_dout(save_dout),.mem_din(save_din));
    // шина снимка: адрес c8, слово cc (запись — на паузе, адрес после неё +1)
    reg [31:0] ss_q;
    always @(posedge clk) begin
        ss_q<=ss_rdata;
        ss_we<=wr_stb && wr_addr==18'h33 && paused;
        if (wr_stb && wr_addr==18'h33) ss_wdata<=wr_data;
        if (wr_stb && wr_addr==18'h32) ss_addr<=wr_data[9:0];
        else if (ss_we) ss_addr<=ss_addr+1'b1;
    end
    // окно меню: запись текста — прямо из обмена LINK
    assign osd_wr = wr_stb && wr_addr >= 18'h0c00 && wr_addr < 18'h0e00;
    assign osd_addr = wr_addr[8:0];
    assign osd_data = wr_data;
    // мост к microSD Tang (fb_sdspi из ядра TS-Config, те же регистры и окна)
    wire [31:0] sd_ctrl_rd, sd_ptr_rd, sd_stat, sd_rx_q;
    fb_sdspi #(.CLK_KHZ(21600)) sdspi(.clk(clk), .rst_n(rst_n),
        .sd_cs_n(sd_cs_n), .sd_sck(sd_sck), .sd_mosi(sd_mosi), .sd_miso(sd_miso),
        .ctrl_we(wr_stb && wr_addr == 18'h40), .ptr_we(wr_stb && wr_addr == 18'h41),
        .cmd_we(wr_stb && wr_addr == 18'h42), .wdata(wr_data),
        .ctrl_rd(sd_ctrl_rd), .ptr_rd(sd_ptr_rd), .status(sd_stat),
        .tx_we(wr_stb && wr_addr[17:9] == 9'd4), .tx_waddr(wr_addr[8:0]), .tx_wdata(wr_data),
        .rd_clk(sck), .rd_addr(rd_addr[8:0]), .rd_q(sd_rx_q));
    reg [31:0] scratch;
    reg [31:0] packet[0:63] /* synthesis syn_ramstyle="block_ram" */;
    reg [31:0] packet_q;
    reg [8:0] pos, count;
    reg [3:0] pace;
    reg packet_busy, protocol_error;
    reg [15:0] packet_seq;
    reg [31:0] received, crc;
    // Synchronous read is settled well before pace=8 samples a byte.
    always @(posedge clk) begin
        if (wr_stb && wr_addr>=18'h1000 && wr_addr<18'h1040 && !packet_busy)
            packet[wr_addr[5:0]]<=wr_data;
        packet_q<=packet[pos[7:2]];
    end
    function automatic [31:0] crc_byte(input [31:0] c, input [7:0] b);
        reg [31:0] v;
        begin
            v=c ^ {24'b0,b};
            for (integer i=0;i<8;i=i+1) v=(v>>1) ^ (v[0] ? 32'hedb88320 : 32'd0);
            crc_byte=v;
        end
    endfunction
    wire [7:0] next_byte=packet_q >> (8*pos[1:0]);
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            keyboard<=0; joystick0<=0; scratch<=0; boot_req<=0;
            aspect_restore<=0; aspect_value<=0; picture_size<=0;
            osd_show<=0; osd_sel<=6'd63; pause<=0; osd_band<=0; osd_rule<=0; osd_bar<=0;
            loading<=0; rom_valid<=0; rom_data<=0; pos<=0; count<=0; pace<=0;
            packet_busy<=0; protocol_error<=0; packet_seq<=0; received<=0; crc<=32'hffffffff;
        end else begin
            rom_valid<=0; aspect_restore<=0;
            if (packet_busy) begin
                pace<=pace+1'b1;
                if (pace==8) begin
                    rom_data<=next_byte;
                    rom_valid<=1;
                    crc<=crc_byte(crc,next_byte);
                    received<=received+1'b1;
                end
                if (pace==15) begin
                    if (pos+1'b1==count) packet_busy<=0;
                    else pos<=pos+1'b1;
                end
            end
            if (wr_stb) case(wr_addr)
                18'h04: keyboard[wr_data[6:0]]<=!wr_data[7];
                18'h06: joystick0<=wr_data[7:0];
                18'h07: begin keyboard<=0; joystick0<=0; end
                18'h0f: scratch<=wr_data;
                18'h11: begin
                    if (wr_data==1 && !packet_busy && !sdram_busy) begin
                        loading<=1; packet_seq<=0; received<=0; crc<=32'hffffffff; protocol_error<=0;
                    end else if (wr_data==2 && !packet_busy && loader_done && !loader_fail && !protocol_error)
                        loading<=0;
                end
                18'h12: begin
                    if (wr_data[31:16]!=packet_seq) begin
                        if (!packet_busy && loading && wr_data[31:16]==packet_seq+16'd1 &&
                            wr_data[15:0]>0 && wr_data[15:0]<=256) begin
                            packet_seq<=wr_data[31:16]; count<=wr_data[8:0];
                            pos<=0; pace<=0; packet_busy<=1;
                        end else protocol_error<=1;
                    end
                end
                // Выбор пропорций из соседнего .cfg; сброс игры его не меняет.
                18'h20: if (wr_data[31:1]==31'h20a98000) begin
                    aspect_value<=wr_data[0]; aspect_restore<=1;
                end
                // Размер картинки (0.9): из SIZE слота 0 или из .cfg — пишет NES.ELF.
                18'h21: if (wr_data[31:16]==16'h535a) picture_size<=wr_data[1:0];
                18'h30: begin osd_show<=wr_data[0]; osd_sel<=wr_data[13:8]; end
                18'h31: pause<=wr_data[0];
                18'h34: osd_band<=wr_data[29:0];
                18'h35: osd_rule<=wr_data[29:0];
                18'h36: osd_bar<=wr_data;
                18'h3c: if (wr_data==32'h424f4f54) boot_req<=1;
                default: ;
            endcase
        end
    end
    // Same 2048-frame audio ring and gray pointer as the Next VDAC4 ABI.
    localparam AUDIO_DIV=450; // exact 21600000/450 = 48000 Hz
    reg [8:0] a_cnt;
    reg [10:0] a_wp, a_wp_g;
    reg [15:0] a_wraps, a_wraps_r;
    reg [31:0] aram[0:2047] /* synthesis syn_ramstyle="block_ram" */;
    reg a_we;
    reg [10:0] a_wa;
    reg [31:0] a_wd, a_q;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin a_cnt<=0; a_wp<=0; a_wraps<=0; a_wp_g<=0; a_wraps_r<=0; end
        else begin
            if (a_cnt==AUDIO_DIV-1) begin
                a_cnt<=0; a_wp<=a_wp+1'b1;
                if (a_wp==2047) a_wraps<=a_wraps+1'b1;
            end else a_cnt<=a_cnt+1'b1;
            a_wp_g<=a_wp ^ (a_wp>>1); a_wraps_r<=a_wraps;
        end
    end
    always @(posedge clk) begin
        a_we<=rst_n && a_cnt==AUDIO_DIV-1;
        a_wa<=a_wp;
        a_wd<=(loading || pause) ? 32'b0 : {sample,sample};   // на паузе (меню) — тишина
        if (a_we) aram[a_wa]<=a_wd;
    end
    always @(posedge sck) a_q<=aram[rd_addr[10:0]];
    always @* begin
        rd_data=save_rd_data;
        if (rd_addr>=13'h1800) rd_data=a_q;
        else if (rd_addr>=13'h0800 && rd_addr<13'h0a00) rd_data=sd_rx_q;
        else case(rd_addr)
            13'h00: rd_data=32'h56444334;
            13'h01: rd_data=32'h0401000a;
            13'h30: rd_data={16'h4f53,2'b0,osd_sel,6'b0,paused,osd_show};
            13'h31: rd_data={31'b0,paused};
            13'h34: rd_data={2'b0,osd_band};
            13'h35: rd_data={2'b0,osd_rule};
            13'h36: rd_data=osd_bar;
            13'h32: rd_data={16'h5353,6'b0,ss_addr};
            13'h33: rd_data=ss_q;
            13'h40: rd_data=sd_ctrl_rd;
            13'h41: rd_data=sd_ptr_rd;
            13'h42: rd_data=sd_stat;
            13'h02: rd_data={30'b0,boot_req,protocol_error};
            13'h08: rd_data={a_wraps_r,5'b0,a_wp_g};
            13'h09: rd_data=AUDIO_DIV;
            13'h0a: rd_data=21600;
            13'h0f: rd_data=scratch;
            13'h11: rd_data={27'b0,sdram_busy,loader_done && loader_fail,loader_done,packet_busy,loading};
            13'h12: rd_data={16'b0,packet_seq};
            13'h13: rd_data=received;
            13'h14: rd_data=~crc;
            13'h15: rd_data={31'b0,protocol_error};
            13'h20: rd_data={16'h4153,7'b0,aspect_requested,7'b1111111,~aspect_requested};
            13'h21: rd_data={16'h535a,14'b0,picture_size};
            default: ;
        endcase
    end
endmodule
