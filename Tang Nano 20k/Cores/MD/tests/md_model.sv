// SPDX-License-Identifier: GPL-3.0-or-later
// Модель Mega Drive VDAC4 для Verilator (03.11.2026): system (как в md_vdac4.sv), контроллер SDRAM
// md_sdram и модель встроенной SDRAM. ROM, уже разложенный словами SDRAM, читается из rom.hex.
// Видео и события звука снимает md_model.cpp прямо с выходов system.
module md_model(
    input  wire        clk,            // MCLK 54 МГц
    input  wire        clk_z80,        // 27 МГц, фронты — на фронтах clk
    input  wire        rst_n,
    input  wire [15:0] ctrl,
    input  wire [24:0] rom_size,
    input  wire [23:0] pads,
    input  wire        pause,
    input  wire [3:0]  mem_delay,      // только IDEAL_MEM: подтверждение MBUS через столько тактов
    input  wire [2:0]  picture_size,   // WITH_VIDEO: размер и 16:9, как регистр LINK 21
    output wire [3:0]  red, green, blue,
    output wire        hs, vs, hbl, vbl, ce_pix,
    output wire [1:0]  resolution,
    output wire        lace, field,    // чересстрочка VDP (LSM 11) и поле
    output wire        snd_push,
    output wire [31:0] snd_word,
    output wire        vram_req, vram32_req, mem_req, mem_ack, sd_ready,
    output wire        m68k_as_n, m68k_clken, m68k_rom,         // для замера циклов шины 68000
    // WITH_VIDEO: md_video — кадр HDMI до TMDS (такт hclk 74,25 МГц)
    input  wire        hclk,
    output wire        h_de, h_hs, h_vs, h_locked,
    output wire [23:0] h_rgb,
    output wire [8:0]  h_trim,
    output wire [1:0]  h_jumps,         // перескоки развёртки к кадру VDP (по модулю 4)
    output wire [63:0] z80_dbg,         // состояние Z80 (как LINK 54/55)
    input  wire        snap_act, snap_irq,   // снимок: как LINK 5C–5F (md_model.cpp сам — за ESP)
    input  wire [15:0] snap_cmd,
    input  wire [9:0]  snap_ovl_a,
    input  wire        snap_ovl_we,
    input  wire [15:0] snap_ovl_d,
    output wire [15:0] snap_ovl_q,
    output wire        snap_taken,
    input  wire [3:0]  snap_z80_ctl,       // снимок, ядро 0.6: как LINK 60–6D
    output wire [1:0]  snap_z80_st,
    output wire [211:0] snap_z80_reg,
    input  wire [211:0] snap_z80_dir,
    input  wire [12:0] snap_zram_a,
    input  wire        snap_zram_we,
    input  wire [7:0]  snap_zram_d,
    output wire [7:0]  snap_zram_q,
    output wire [63:0] snap_sys_q, snap_ym_q,
    input  wire [63:0] snap_sys_d, snap_ym_d,
    input  wire        snap_sys_we,
    input  wire [7:0]  snap_vdp_a,
    output wire [7:0]  snap_vdp_q
);
wire md_run=rst_n && ctrl[0];
wire sync_stall;                    // WITH_VIDEO: стоянка синхронизации кадра (md_video), иначе 0
wire [24:1] mem_addr; wire [15:0] mem_data, mem_wdata; wire [1:0] mem_be; wire mem_we;
wire vram_ack, vram_we, vram32_ack; wire [1:0] vram_be;
wire [15:1] vram_a, vram32_a; wire [15:0] vram_d, vram_q; wire [31:0] vram32_q;
system megadrive(
    .MCLK(clk), .CLK_Z80(clk_z80), .RESET_N(md_run && sd_ready),
    .LPF_MODE(2'b00), .ENABLE_FM(1'b1), .ENABLE_PSG(1'b1), .DAC_LDATA(), .DAC_RDATA(),
    .VRAM_REQ(vram_req), .VRAM_ACK(vram_ack), .VRAM_WE(vram_we), .VRAM_BE(vram_be), .VRAM_A(vram_a),
    .VRAM_D(vram_d), .VRAM_Q(vram_q), .VRAM32_REQ(vram32_req), .VRAM32_ACK(vram32_ack),
    .VRAM32_A(vram32_a), .VRAM32_Q(vram32_q),
    .SND_PUSH(snd_push), .SND_WORD(snd_word), .SND_TIME(), .Z80_DBG(z80_dbg),
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
wire [22:1] a_addr=!mem_addr[23] ? mem_addr[22:1] :
                   mem_addr[17] ? {6'b101111,mem_addr[16:1]} : {7'b1011101,mem_addr[15:1]};
`ifdef IDEAL_MEM
// сравнение: память без задержки — подтверждение на следующем такте, тот же массив слов SDRAM
reg [31:0] mem [0:2097151];
reg a_ack_r=0, v_ack_r=0, v32_ack_r=0; reg [3:0] a_wait=0; reg [15:0] a_q_r=0, v_q_r=0; reg [31:0] v32_q_r=0;
assign mem_ack=a_ack_r, vram_ack=v_ack_r, vram32_ack=v32_ack_r;
assign mem_data=a_q_r, vram_q=v_q_r, vram32_q=v32_q_r, sd_ready=1'b1;
wire [20:0] aw=a_addr[22:2], vw={6'b110000,vram_a[15:2]+15'd0}, v32w={6'b110000,vram32_a[15:2]+15'd0};
always @(posedge clk) begin
    if (mem_req==a_ack_r) a_wait<=0;
    else if (a_wait+4'd1<mem_delay) a_wait<=a_wait+4'd1;
    else begin
        a_ack_r<=mem_req;
        if (mem_we) begin
            if (a_addr[1]) begin if (mem_be[1]) mem[aw][31:24]<=mem_wdata[15:8]; if (mem_be[0]) mem[aw][23:16]<=mem_wdata[7:0]; end
            else begin if (mem_be[1]) mem[aw][15:8]<=mem_wdata[15:8]; if (mem_be[0]) mem[aw][7:0]<=mem_wdata[7:0]; end
        end else a_q_r<=a_addr[1] ? mem[aw][31:16] : mem[aw][15:0];
    end
    if (vram_req!=v_ack_r) begin
        v_ack_r<=vram_req;
        if (vram_we) begin
            if (vram_a[1]) begin if (vram_be[1]) mem[vw][31:24]<=vram_d[15:8]; if (vram_be[0]) mem[vw][23:16]<=vram_d[7:0]; end
            else begin if (vram_be[1]) mem[vw][15:8]<=vram_d[15:8]; if (vram_be[0]) mem[vw][7:0]<=vram_d[7:0]; end
        end else v_q_r<=vram_a[1] ? mem[vw][31:16] : mem[vw][15:0];
    end
    if (vram32_req!=v32_ack_r) begin v32_ack_r<=vram32_req; v32_q_r<=mem[v32w]; end
end
initial $readmemh("rom.hex", mem);
`else
wire [31:0] dq; wire [10:0] sa; wire [1:0] ba; wire [3:0] dqm; wire cs_n, ras_n, cas_n, we_n;
md_sdram #(.REFRESH_CYCLES(400)) sdram(.clk(clk), .init_n(rst_n), .ready(sd_ready),
    .sd_dq(dq), .sd_a(sa), .sd_ba(ba), .sd_dqm(dqm), .sd_cs_n(cs_n), .sd_ras_n(ras_n),
    .sd_cas_n(cas_n), .sd_we_n(we_n), .sd_cke(),
    .a_req(mem_req), .a_ack(mem_ack), .a_we(mem_we), .a_addr(a_addr), .a_be(mem_be), .a_d(mem_wdata), .a_q(mem_data),
    .v_req(vram_req), .v_ack(vram_ack), .v_we(vram_we), .v_addr(vram_a), .v_be(vram_be), .v_d(vram_d), .v_q(vram_q),
    .v32_req(vram32_req), .v32_ack(vram32_ack), .v32_addr(vram32_a[15:2]), .v32_q(vram32_q),
    .h_req(1'b0), .h_ack(), .h_we(1'b0), .h_addr(21'b0), .h_be(4'b0), .h_d(32'b0), .h_q());
// микросхема — по спаду: такт SDRAM сдвинут (CLKOUTP 135°)
sdr_chip_model chip(.clk(~clk), .dq(dq), .addr(sa), .dqm(dqm), .ba(ba),
    .cs_n(cs_n), .ras_n(ras_n), .cas_n(cas_n), .we_n(we_n));
initial $readmemh("rom.hex", chip.mem);
`endif
`ifdef WITH_VIDEO
md_video video(.mclk(clk), .ce_pix(ce_pix), .hblank(hbl), .vblank(vbl), .hsync(hs),
    .r(red), .g(green), .b(blue), .h40(resolution[0]), .v30(resolution[1]), .lace(lace), .field(field), .pal(ctrl[2]), .blank(!md_run), .sync_stall(sync_stall),
    .clk_osd(clk), .osd_wr(1'b0), .osd_addr(9'b0), .osd_data(32'b0), .osd_show(1'b0), .osd_sel(6'd63),
    .osd_band(30'b0), .osd_rule(30'b0), .osd_bar(32'b0), .picture_size(picture_size),
    .hclk(hclk), .hclk5(1'b0), .hreset(!rst_n),
    .tmds_clk_p(), .tmds_clk_n(), .tmds_d_p(), .tmds_d_n(), .locked(h_locked));
assign h_de=video.de, h_hs=video.hs, h_vs=video.vs, h_rgb=video.rgb, h_trim=video.trim, h_jumps=video.jump_cnt;
`else
assign h_de=0, h_hs=0, h_vs=0, h_locked=0, h_rgb=0, h_trim=0, h_jumps=0, sync_stall=0;
`endif
assign m68k_as_n=megadrive.M68K_AS_N;
assign m68k_clken=megadrive.M68K_CLKENp;
assign m68k_rom=megadrive.M68K_A[23:22]==2'b00 || megadrive.M68K_A[23:21]==3'b111;   // ROM или ОЗУ
endmodule
