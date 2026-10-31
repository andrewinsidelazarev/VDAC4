module link_top(
    input clk, rst_n, sck, cs_n, input [3:0] io_in,
    output [3:0] io_out, output io_oe,
    output boot_req, soft_reset, rom_ready, rom_we,
    output [31:0] video_probe,
    output [17:0] rom_addr, output [15:0] rom_word,
    output hid_strobe, hid_start, hid_reset, output [7:0] hid_data,
    output sd_cs_n, sd_sck, sd_mosi,
    input disk_reset, input [7:0] disk_rd, disk_wr, input [31:0] disk_sector,
    output [7:0] disk_mounted, disk_data, output [63:0] disk_size,
    output disk_busy, disk_done, disk_strobe, output [8:0] disk_addr
);
    reg [1:0] phase=0;
    wire [31:0] debug_words [0:35];
    wire [8:0] trace_index; wire [31:0] trace_start;
    for (genvar i=0; i<36; i=i+1) assign debug_words[i]=32'hd1a60000+i;
    always @(posedge clk) if (!rst_n) phase<=0; else phase<=phase+1'b1;
    amiga_link dut(.clk7_en(phase==0),.clk7n_en(phase==2),
        .audio_left(15'h1234),.audio_right(15'h6001),.key_free(8'd8),
        .sd_miso(sd_mosi),.*);
endmodule
