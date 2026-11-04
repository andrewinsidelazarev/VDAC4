// Стенд LINK ядра Mega Drive (03.11.2026): md_link + md_sdram + модель SDRAM (по спаду, как с CLKOUTP
// 135°). Ведущий QSPI — md_link_test.cpp; события звука подаёт тоже он (snd_push/snd_word).
module md_link_top(
    input  wire        clk, rst_n,
    input  wire        sck, cs_n,
    input  wire [3:0]  io_in,
    output wire [3:0]  io_out,
    output wire        io_oe,
    input  wire        snd_push,
    input  wire [31:0] snd_word,
    input  wire [19:0] snd_time,
    output wire [15:0] ctrl,
    output wire [24:0] rom_size,
    output wire [23:0] pads,
    output wire        pause, osd_show, boot_req,
    output wire [2:0]  picture_size,
    output wire        sd_ready
);
wire h_req, h_ack, h_we; wire [22:2] h_addr; wire [31:0] h_d, h_q;
// снимок (ядро 0.6): ОЗУ Z80 — как dpram system.sv (чтение такт спустя), регистры Z80, банки, YM — по кругу
wire [12:0] zram_a; wire zram_we; wire [7:0] zram_d, vdp_a; reg [7:0] zram_q = 0;
reg  [7:0] zram [0:8191];
always @(posedge clk) begin if (zram_we) zram[zram_a] <= zram_d; else zram_q <= zram[zram_a]; end
wire [211:0] z80_dir; wire [63:0] sys_d, ym_d;
wire osd_wr; wire [8:0] osd_addr; wire [31:0] osd_data, osd_bar; wire [5:0] osd_sel; wire [29:0] osd_band, osd_rule;
md_link link(.sck(sck), .cs_n(cs_n), .io_in(io_in), .io_out(io_out), .io_oe(io_oe),
    .clk(clk), .rst_n(rst_n), .boot_req(boot_req),
    .osd_wr(osd_wr), .osd_addr(osd_addr), .osd_data(osd_data), .osd_show(osd_show), .osd_sel(osd_sel),
    .osd_band(osd_band), .osd_rule(osd_rule), .osd_bar(osd_bar), .picture_size(picture_size),
    .sd_cs_n(), .sd_sck(), .sd_mosi(), .sd_miso(1'b1),
    .ctrl(ctrl), .rom_size(rom_size), .pads(pads), .pause(pause),
    .frame(1'b0), .video_locked(1'b1), .diag(14'h2A5A), .z80_dbg(64'h0123456789ABCDEF),
    .snap_act(), .snap_irq(), .snap_cmd(), .snap_ovl_a(), .snap_ovl_we(), .snap_ovl_d(), .snap_ovl_q(16'hBEEF), .snap_taken(1'b1),
    .snap_z80_ctl(), .snap_z80_st(2'b10), .snap_z80_reg(z80_dir), .snap_z80_dir(z80_dir),
    .snap_zram_a(zram_a), .snap_zram_we(zram_we), .snap_zram_d(zram_d), .snap_zram_q(zram_q),
    .snap_sys_q(sys_d), .snap_ym_q(~ym_d), .snap_sys_d(sys_d), .snap_ym_d(ym_d), .snap_sys_we(),
    .snap_vdp_a(vdp_a), .snap_vdp_q(vdp_a ^ 8'h5A),
    .snd_push(snd_push), .snd_word(snd_word), .snd_time(snd_time),
    .h_req(h_req), .h_ack(h_ack), .h_we(h_we), .h_addr(h_addr), .h_d(h_d), .h_q(h_q));
wire [31:0] dq; wire [10:0] sa; wire [1:0] ba; wire [3:0] dqm; wire cs, ras, cas, we;
md_sdram #(.REFRESH_CYCLES(400)) sdram(.clk(clk), .init_n(rst_n), .ready(sd_ready),
    .sd_dq(dq), .sd_a(sa), .sd_ba(ba), .sd_dqm(dqm), .sd_cs_n(cs), .sd_ras_n(ras), .sd_cas_n(cas), .sd_we_n(we),
    .sd_cke(),
    .a_req(1'b0), .a_ack(), .a_we(1'b0), .a_addr(22'b0), .a_be(2'b0), .a_d(16'b0), .a_q(),
    .v_req(1'b0), .v_ack(), .v_we(1'b0), .v_addr(15'b0), .v_be(2'b0), .v_d(16'b0), .v_q(),
    .v32_req(1'b0), .v32_ack(), .v32_addr(14'b0), .v32_q(),
    .h_req(h_req), .h_ack(h_ack), .h_we(h_we), .h_addr(h_addr), .h_be(4'b1111), .h_d(h_d), .h_q(h_q));
sdr_chip_model chip(.clk(~clk), .dq(dq), .addr(sa), .dqm(dqm), .ba(ba), .cs_n(cs), .ras_n(ras), .cas_n(cas), .we_n(we));
endmodule
