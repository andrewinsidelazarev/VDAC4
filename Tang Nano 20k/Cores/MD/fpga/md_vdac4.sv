// SPDX-License-Identifier: GPL-3.0-or-later
// Mega Drive / Genesis на VDAC4 (03.11.2026): ядро mdtang (nand2mario, порт Genesis-MiSTer) с обвязкой
// платы VDAC4. Всё, что можно, делает ESP (MD.ELF): загрузка и разбор ROM, регион и особенности игр,
// SRAM, пады из клавиатуры и Kempston, меню, а главное — звук: YM2612 и SN76489 синтезирует ESP по
// записям в их порты (md_snd_events), в ПЛИС от них остались таймеры и флаги (md_ym_stub).
//
// Память (SDRAM 8 МБ, md_sdram на самом MCLK):
//   000000–5CFFFF  ROM (до 5,8 МБ — хватает Super Street Fighter II, 5 МБ)
//   5D0000–5DFFFF  ОЗУ 68000 (64 КБ)
//   5E0000–5FFFFF  SRAM картриджа (128 КБ)
//   600000–60FFFF  видеопамять VDP (банк 3 — своё окно контроллера)
// Видео — md_video (720p, подстроенный под кадр, без кадрового буфера).
`default_nettype none
module md_vdac4(
    input  wire        clk_27m,
    input  wire [1:0]  btn,
    input  wire        link_sck, link_cs_n,
    inout  wire [3:0]  link_io,
    output tri         link_irq_n,
    input  wire        cp_sck, cp_cs_n, cp_mosi,
    output wire        cp_miso, cp_irq_n,
    output wire        mux_sel, pa_en, ft_int_n, ft_miso,
    input  wire [7:0]  mux_d,
    input  wire        hsync_in, vsync_in, pclk_in, m_sel, ft_sck, ft_cs_n, ft_mosi,
    output wire        O_sdram_clk, O_sdram_cke, O_sdram_cs_n, O_sdram_cas_n, O_sdram_ras_n, O_sdram_wen_n,
    output wire [10:0] O_sdram_addr,
    output wire [1:0]  O_sdram_ba,
    output wire [3:0]  O_sdram_dqm,
    inout  wire [31:0] IO_sdram_dq,
    output wire        SD_SCK, SD_MOSI, SD_CS,
    input  wire        SD_MISO,
    output wire        O_tmds_clk_p, O_tmds_clk_n,
    output wire [2:0]  O_tmds_data_p, O_tmds_data_n
);

// --- такты ---
wire mclk, clk_z80, lock_sys, hclk5, hclk, lock_h;
md_pll_sys pll_sys(.clkin(clk_27m), .clkout(mclk), .clkoutd(clk_z80), .clkoutp(O_sdram_clk), .lock(lock_sys));
gowin_pll_hdmi pll_hdmi(.clkin(clk_27m), .clkout(hclk5), .lock(lock_h));
CLKDIV #(.DIV_MODE("5")) div5(.CLKOUT(hclk), .HCLKIN(hclk5), .RESETN(lock_h), .CALIB(1'b0));
reg [2:0] rst_sync=0;
always @(posedge mclk) rst_sync<={rst_sync[1:0],lock_sys};
wire rst_n=rst_sync[2];
reg [2:0] hrst_sync=0;
always @(posedge hclk) hrst_sync<={hrst_sync[1:0],lock_h};
wire hreset=!hrst_sync[2];

// --- выходы платы VDAC4 в покое, возврат в слот 0 ---
assign mux_sel=0, pa_en=0, ft_int_n=1, ft_miso=0;
assign link_irq_n=1'bz;
wire [3:0] link_out;
wire link_oe, boot_req, evo_reset;
assign link_io=link_oe ? link_out : 4'bzzzz;
next_evo_reset evo(.clk(mclk), .m_sel(m_sel), .req(evo_reset));
next_cp cp(.sck(cp_sck), .cs_n(cp_cs_n), .mosi(cp_mosi), .miso(cp_miso), .irq_n(cp_irq_n),
    .clk(mclk), .boot(boot_req | evo_reset));

// --- LINK ---
wire osd_wr, osd_show, pause; wire [8:0] osd_addr; wire [31:0] osd_data, osd_bar;
wire [5:0] osd_sel; wire [29:0] osd_band, osd_rule; wire [2:0] picture_size;
wire [15:0] ctrl; wire [24:0] rom_size; wire [23:0] pads;
wire snd_push; wire [31:0] snd_word; wire [19:0] snd_time; wire [63:0] z80_dbg;
wire snap_act, snap_irq, snap_taken, snap_ovl_we; wire [15:0] snap_cmd, snap_ovl_d, snap_ovl_q; wire [9:0] snap_ovl_a;
wire [3:0] snap_z80_ctl; wire [1:0] snap_z80_st; wire [211:0] snap_z80_reg, snap_z80_dir;
wire [12:0] snap_zram_a; wire snap_zram_we, snap_sys_we; wire [7:0] snap_zram_d, snap_zram_q, snap_vdp_a, snap_vdp_q;
wire [63:0] snap_sys_q, snap_sys_d, snap_ym_q, snap_ym_d;
wire h_req, h_ack, h_we; wire [22:2] h_addr; wire [31:0] h_d, h_q;
wire vbl, video_locked, sync_stall; wire [7:0] hframes; wire [1:0] hres_cnt, jump_cnt;
md_link link(.sck(link_sck), .cs_n(link_cs_n), .io_in(link_io), .io_out(link_out), .io_oe(link_oe),
    .clk(mclk), .rst_n(rst_n), .boot_req(boot_req),
    .osd_wr(osd_wr), .osd_addr(osd_addr), .osd_data(osd_data), .osd_show(osd_show), .osd_sel(osd_sel),
    .osd_band(osd_band), .osd_rule(osd_rule), .osd_bar(osd_bar), .picture_size(picture_size),
    .sd_cs_n(SD_CS), .sd_sck(SD_SCK), .sd_mosi(SD_MOSI), .sd_miso(SD_MISO),
    .ctrl(ctrl), .rom_size(rom_size), .pads(pads), .pause(pause),
    .frame(vbl), .video_locked(video_locked), .diag({lock_h,lock_sys,hres_cnt,jump_cnt,hframes}), .z80_dbg(z80_dbg),
    .snap_act(snap_act), .snap_irq(snap_irq), .snap_cmd(snap_cmd), .snap_ovl_a(snap_ovl_a), .snap_ovl_we(snap_ovl_we),
    .snap_ovl_d(snap_ovl_d), .snap_ovl_q(snap_ovl_q), .snap_taken(snap_taken),
    .snap_z80_ctl(snap_z80_ctl), .snap_z80_st(snap_z80_st), .snap_z80_reg(snap_z80_reg), .snap_z80_dir(snap_z80_dir),
    .snap_zram_a(snap_zram_a), .snap_zram_we(snap_zram_we), .snap_zram_d(snap_zram_d), .snap_zram_q(snap_zram_q),
    .snap_sys_q(snap_sys_q), .snap_sys_d(snap_sys_d), .snap_ym_q(snap_ym_q), .snap_ym_d(snap_ym_d),
    .snap_sys_we(snap_sys_we), .snap_vdp_a(snap_vdp_a), .snap_vdp_q(snap_vdp_q),
    .snd_push(snd_push), .snd_word(snd_word), .snd_time(snd_time),
    .h_req(h_req), .h_ack(h_ack), .h_we(h_we), .h_addr(h_addr), .h_d(h_d), .h_q(h_q));

// --- Mega Drive ---
wire md_run=rst_n && ctrl[0];
wire [24:1] mem_addr; wire [15:0] mem_data, mem_wdata; wire [1:0] mem_be; wire mem_req, mem_ack, mem_we;
wire vram_req, vram_ack, vram_we, vram32_req, vram32_ack; wire [1:0] vram_be;
wire [15:1] vram_a, vram32_a; wire [15:0] vram_d, vram_q; wire [31:0] vram32_q;
wire [3:0] red, green, blue; wire hs, vs, hbl, ce_pix, lace, field; wire [1:0] resolution;
system megadrive(
    .MCLK(mclk), .CLK_Z80(clk_z80), .RESET_N(md_run),
    .LPF_MODE(2'b00), .ENABLE_FM(1'b1), .ENABLE_PSG(1'b1), .DAC_LDATA(), .DAC_RDATA(),
    .VRAM_REQ(vram_req), .VRAM_ACK(vram_ack), .VRAM_WE(vram_we), .VRAM_BE(vram_be), .VRAM_A(vram_a),
    .VRAM_D(vram_d), .VRAM_Q(vram_q), .VRAM32_REQ(vram32_req), .VRAM32_ACK(vram32_ack),
    .VRAM32_A(vram32_a), .VRAM32_Q(vram32_q),
    .SND_PUSH(snd_push), .SND_WORD(snd_word), .SND_TIME(snd_time), .Z80_DBG(z80_dbg),
    .SNAP_ACT(snap_act), .SNAP_IRQ(snap_irq), .SNAP_TAKEN(snap_taken), .SNAP_CMD(snap_cmd),
    .SNAP_OVL_A(snap_ovl_a), .SNAP_OVL_WE(snap_ovl_we), .SNAP_OVL_D(snap_ovl_d), .SNAP_OVL_Q(snap_ovl_q),
    .SNAP_Z80_CTL(snap_z80_ctl), .SNAP_Z80_ST(snap_z80_st), .SNAP_Z80_REG(snap_z80_reg), .SNAP_Z80_DIR(snap_z80_dir),
    .SNAP_ZRAM_A(snap_zram_a), .SNAP_ZRAM_WE(snap_zram_we), .SNAP_ZRAM_D(snap_zram_d), .SNAP_ZRAM_Q(snap_zram_q),
    .SNAP_SYS_Q(snap_sys_q), .SNAP_SYS_D(snap_sys_d), .SNAP_YM_Q(snap_ym_q), .SNAP_YM_D(snap_ym_d),
    .SNAP_SYS_WE(snap_sys_we), .SNAP_VDP_A(snap_vdp_a), .SNAP_VDP_Q(snap_vdp_q),
    .LOADING(1'b0), .PAL(ctrl[2]), .EXPORT(ctrl[3]), .FAST_FIFO(ctrl[13]),
    .SRAM_QUIRK(ctrl[8]), .SRAM00_QUIRK(ctrl[9]), .EEPROM_QUIRK(1'b0), .NORAM_QUIRK(ctrl[10]),
    .PIER_QUIRK(1'b0), .SVP_QUIRK(1'b0), .FMBUSY_QUIRK(ctrl[11]), .SCHAN_QUIRK(ctrl[12]), .TURBO(2'b00),
    .GG_RESET(1'b0), .GG_EN(1'b0), .GG_CODE(129'b0), .GG_AVAILABLE(),
    .BRAM_A(15'b0), .BRAM_DI(16'b0), .BRAM_DO(), .BRAM_WE(1'b0), .BRAM_CHANGE(),
    .RED(red), .GREEN(green), .BLUE(blue), .VS(vs), .HS(hs), .HBL(hbl), .VBL(vbl), .CE_PIX(ce_pix),
    .BORDER(1'b0), .CRAM_DOTS(1'b0), .INTERLACE(lace), .FIELD(field), .RESOLUTION(resolution),
    .J3BUT(ctrl[4]), .JOY_1(pads[11:0]), .JOY_2(pads[23:12]), .JOY_3(12'b0), .JOY_4(12'b0), .JOY_5(12'b0),
    .MULTITAP(3'b0), .MOUSE(25'b0), .MOUSE_OPT(3'b0),
    .GUN_OPT(1'b0), .GUN_TYPE(1'b0), .GUN_SENSOR(1'b0), .GUN_A(1'b0), .GUN_B(1'b0), .GUN_C(1'b0),
    .GUN_START(1'b0), .SERJOYSTICK_IN(8'b0), .SERJOYSTICK_OUT(), .SER_OPT(2'b0),
    .ROMSZ(rom_size[24:1]), .MEM_ADDR(mem_addr), .MEM_DATA(mem_data), .MEM_WDATA(mem_wdata),
    .MEM_WE(mem_we), .MEM_BE(mem_be), .MEM_REQ(mem_req), .MEM_ACK(mem_ack),
    .EN_HIFI_PCM(1'b0), .LADDER(1'b0), .OBJ_LIMIT_HIGH(1'b0), .TRANSP_DETECT(),
    .PAUSE_EN(pause), .SYNC_STALL(sync_stall), .BGA_EN(1'b1), .BGB_EN(1'b1), .SPR_EN(1'b1), .DBG_M68K_A(), .DBG_VBUS_A()
);

// MEM_ADDR — слово: [23] = 0 — ROM; иначе ОЗУ ({9'b0_1000_0000, A[15:1]}) или SRAM ({8'b0_1000_001, A[16:1]})
wire [22:1] a_addr=!mem_addr[23] ? mem_addr[22:1] :
                   mem_addr[17] ? {6'b101111,mem_addr[16:1]} :      // 5E0000 + 2*A[16:1]
                                  {7'b1011101,mem_addr[15:1]};      // 5D0000 + 2*A[15:1]

md_sdram sdram(.clk(mclk), .init_n(lock_sys), .ready(),
    .sd_dq(IO_sdram_dq), .sd_a(O_sdram_addr), .sd_ba(O_sdram_ba), .sd_dqm(O_sdram_dqm),
    .sd_cs_n(O_sdram_cs_n), .sd_ras_n(O_sdram_ras_n), .sd_cas_n(O_sdram_cas_n), .sd_we_n(O_sdram_wen_n),
    .sd_cke(O_sdram_cke),
    .a_req(mem_req), .a_ack(mem_ack), .a_we(mem_we), .a_addr(a_addr), .a_be(mem_be), .a_d(mem_wdata), .a_q(mem_data),
    .v_req(vram_req), .v_ack(vram_ack), .v_we(vram_we), .v_addr(vram_a), .v_be(vram_be), .v_d(vram_d), .v_q(vram_q),
    .v32_req(vram32_req), .v32_ack(vram32_ack), .v32_addr(vram32_a[15:2]), .v32_q(vram32_q),
    .h_req(h_req), .h_ack(h_ack), .h_we(h_we), .h_addr(h_addr), .h_be(4'b1111), .h_d(h_d), .h_q(h_q));

// --- видео ---
md_video video(.mclk(mclk), .ce_pix(ce_pix), .hblank(hbl), .vblank(vbl), .hsync(hs),
    .r(red), .g(green), .b(blue), .h40(resolution[0]), .v30(resolution[1]), .lace(lace), .field(field), .pal(ctrl[2]), .blank(!md_run), .sync_stall(sync_stall),
    .clk_osd(mclk), .osd_wr(osd_wr), .osd_addr(osd_addr), .osd_data(osd_data), .osd_show(osd_show),
    .osd_sel(osd_sel), .osd_band(osd_band), .osd_rule(osd_rule), .osd_bar(osd_bar),
    .picture_size(picture_size),
    .hclk(hclk), .hclk5(hclk5), .hreset(hreset),
    .tmds_clk_p(O_tmds_clk_p), .tmds_clk_n(O_tmds_clk_n), .tmds_d_p(O_tmds_data_p), .tmds_d_n(O_tmds_data_n),
    .locked(video_locked), .hframes(hframes), .hres_cnt(hres_cnt), .jump_cnt(jump_cnt));

endmodule
`default_nettype wire
