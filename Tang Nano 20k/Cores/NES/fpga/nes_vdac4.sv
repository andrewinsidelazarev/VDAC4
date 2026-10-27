// NESTang VDAC4 board adaptation. GPL-3.0-or-later.
module nes_vdac4
(
    // Master input clock
    input         clk_27m,

    // Buttons (active low on Tang Nano 20K)
    input   [1:0]   btn,

    // LINK вЂ” ESP32-S3 VDAC4 (QSPI, РІРµРґСѓС‰РёР№ вЂ” ESP): РєР»Р°РІРёР°С‚СѓСЂР°, РјС‹С€СЊ, РґР¶РѕР№СЃС‚РёРє, Р·РІСѓРє, UART
    input  wire        link_sck,     // FPGA77 (GCLKT_1)
    input  wire        link_cs_n,    // FPGA18
    inout  wire [3:0]  link_io,      // FPGA71, 72, 55, 31
    output tri         link_irq_n,   // FPGA85: РЅРµ РёСЃРїРѕР»СЊР·СѓРµС‚СЃСЏ

    // BL616: С‚РѕР»СЊРєРѕ Р·Р°РїСЂРѕСЃ РІРѕР·РІСЂР°С‚Р° РІ СЃР»РѕС‚ 0 (cp_spi СЏРґРµСЂ VDAC4)
    input  wire        cp_sck,       // FPGA13
    input  wire        cp_cs_n,      // FPGA86
    input  wire        cp_mosi,      // FPGA76
    output wire        cp_miso,      // FPGA75
    output wire        cp_irq_n,     // FPGA69

    // Р’С‹С…РѕРґС‹ РїР»Р°С‚С‹ VDAC4 вЂ” РІ РїРѕРєРѕРµ, РєР°Рє РёС… РґРµСЂР¶РёС‚ СЏРґСЂРѕ VDAC4 Р±РµР· СЂР°Р±РѕС‚С‹
    output wire        mux_sel,      // FPGA56: РІС‹Р±РѕСЂ РїРѕР»РѕРІРёРЅС‹ С€РёРЅС‹ Evo
    output wire        pa_en,        // FPGA51: СѓСЃРёР»РёС‚РµР»СЊ Tang вЂ” РІС‹РєР»СЋС‡РµРЅ
    output wire        ft_int_n,     // FPGA74 в†’ Evo: INT_N FT812 РЅРµР°РєС‚РёРІРµРЅ
    output wire        ft_miso,      // FPGA79 в†’ Evo: MISO FT812 вЂ” 0

    // Р’С…РѕРґС‹ РїР»Р°С‚С‹ VDAC4 РѕС‚ Evo, РєРѕС‚РѕСЂС‹Рµ СЏРґСЂСѓ Next РЅРµ РЅСѓР¶РЅС‹: РѕРїРёСЃР°РЅС‹, С‡С‚РѕР±С‹ РёС… РїРѕРґС‚СЏР¶РєРё Р±С‹Р»Рё РєР°Рє Сѓ
    // СЏРґРµСЂ VDAC4 (ts_fb_hdmi_top.cst), Р° РЅРµ РїРѕРґС‚СЏР¶РєР° РІРІРµСЂС… Gowin РїРѕ СѓРјРѕР»С‡Р°РЅРёСЋ. FPGA19/20 вЂ” РѕР±С‰РёРµ
    // MOSI Рё С‚Р°РєС‚ SPI Evo, РёС… Р¶Рµ СЃР»СѓС€Р°РµС‚ ESP (GPIO11/12): Сѓ TS-Config С‚Р°РєС‚ РїРѕРґС‚СЏРЅСѓС‚ РІРЅРёР·.
    input  wire [7:0]  mux_d,        // FPGA27вЂ¦30, 41, 42, 48, 54: С€РёРЅР° Evo
    input  wire        hsync_in,     // FPGA15
    input  wire        vsync_in,     // FPGA16
    input  wire        pclk_in,      // FPGA80
    input  wire        m_sel,        // FPGA73: 1 вЂ” Evo РІ СЂРµР¶РёРјРµ FT812 (РїР»Р°РіРёРЅ), 0 РїРѕСЃР»Рµ РІР·РІРѕРґР° вЂ” СЃР±СЂРѕСЃ Evo
    input  wire        ft_sck,       // FPGA20: С‚Р°РєС‚ SPI Evo (Рё ESP)
    input  wire        ft_cs_n,      // FPGA17
    input  wire        ft_mosi,      // FPGA19: MOSI SPI Evo (Рё ESP)

    // SDRAM interface
    output          O_sdram_clk,
    output          O_sdram_cke,
    output          O_sdram_cs_n,
    output          O_sdram_cas_n,
    output          O_sdram_ras_n,
    output          O_sdram_wen_n,
    output  [10:0]  O_sdram_addr,
    output  [1:0]   O_sdram_ba,
    output  [3:0]   O_sdram_dqm,
    inout   [31:0]  IO_sdram_dq,

    // Physical SD card interface (microSD СЃР°РјРѕР№ Tang: NextZXOS)
    output        SD_SCK,
    output        SD_MOSI,
    input         SD_MISO,
    output        SD_CS,

    output            O_tmds_clk_p    ,
    output            O_tmds_clk_n    ,
    output     [2:0]  O_tmds_data_p   ,//{r,g,b}
    output     [2:0]  O_tmds_data_n

);

wire clk, fclk, hclk, hclk5;
wire pll_lock, hdmi_lock;
gowin_pll_nes pll_nes(.clkin(clk_27m), .clkoutd3(clk), .clkout(fclk),
    .clkoutp(O_sdram_clk), .lock(pll_lock));
gowin_pll_hdmi pll_hdmi(.clkin(clk_27m), .clkout(hclk5), .lock(hdmi_lock));
reg [2:0] reset_sync = 0;
always @(posedge clk) reset_sync <= {reset_sync[1:0], pll_lock};
wire sys_resetn = reset_sync[2];
CLKDIV #(.DIV_MODE("5")) div5(.CLKOUT(hclk), .HCLKIN(hclk5), .RESETN(hdmi_lock), .CALIB(1'b0));

assign mux_sel=0, pa_en=0, ft_int_n=1, ft_miso=0;
// microSD Tang — мост fb_sdspi в nes_link: ESP (FatFs) читает и пишет карту во время игры
// (меню выбора образа, снимки); картридж по-прежнему приходит от ESP через LINK.
assign link_irq_n=1'bz;
wire osd_wr, osd_show, nes_pause, nes_paused;
wire [8:0] osd_addr;
wire [31:0] osd_data;
wire [5:0] osd_sel;
wire [29:0] osd_band, osd_rule;                 // 0.10: оформление окна меню (LINK d0, d4, d8)
wire [31:0] osd_bar;
wire [3:0] link_out;
wire link_oe, boot_req, evo_reset;
wire aspect_requested, aspect_restore, aspect_value;
wire [1:0] picture_size;                        // 0.9: размер картинки 100 / 95 / 90 % (LINK 84)
assign link_io=link_oe ? link_out : 4'bzzzz;
next_evo_reset evo(.clk(clk), .m_sel(m_sel), .req(evo_reset));
next_cp cp(.sck(cp_sck), .cs_n(cp_cs_n), .mosi(cp_mosi), .miso(cp_miso),
    .irq_n(cp_irq_n), .clk(clk), .boot(boot_req | evo_reset));

wire [127:0] keys;
wire [7:0] joystick0;
wire loading, loader_do_valid, loader_done, loader_fail, sdram_busy;
wire [7:0] loader_do;
wire [15:0] sample;
wire save_hold, save_read, save_write, save_full;
wire [21:0] save_offset;
wire [9:0] ss_addr; wire [31:0] ss_wdata, ss_rdata; wire ss_we;
wire [7:0] save_dout, save_din;
wire [17:0] bram_addr;
wire [7:0] bram_dout;
wire bram_write;
reg [7:0] eeprom[0:255];
wire [7:0] bram_din=eeprom[bram_addr[7:0]];
always @(posedge clk) begin
    if (save_hold && save_write && save_offset[18]) eeprom[save_offset[7:0]]<=save_dout;
    else if (!save_hold && bram_write) eeprom[bram_addr[7:0]]<=bram_dout;
end
assign save_din=(save_hold && save_offset[18]) ? eeprom[save_offset[7:0]] : memory_din_cpu;
nes_link link(.sck(link_sck), .cs_n(link_cs_n), .io_in(link_io),
    .io_out(link_out), .io_oe(link_oe), .clk(clk), .rst_n(sys_resetn),
    .keyboard(keys), .joystick0(joystick0), .sample(sample), .boot_req(boot_req),
    .aspect_requested(aspect_requested), .aspect_restore(aspect_restore), .aspect_value(aspect_value),
    .picture_size(picture_size),
    .osd_wr(osd_wr), .osd_addr(osd_addr), .osd_data(osd_data), .osd_show(osd_show), .osd_sel(osd_sel),
    .osd_band(osd_band), .osd_rule(osd_rule), .osd_bar(osd_bar),
    .pause(nes_pause), .paused(nes_paused),
    .sd_cs_n(SD_CS), .sd_sck(SD_SCK), .sd_mosi(SD_MOSI), .sd_miso(SD_MISO),
    .loading(loading), .rom_data(loader_do), .rom_valid(loader_do_valid),
    .loader_done(loader_done), .loader_fail(loader_fail), .sdram_busy(sdram_busy),
    .save_hold(save_hold), .save_offset(save_offset), .save_read(save_read),
    .save_write(save_write), .save_dout(save_dout), .save_din(save_din), .save_full(save_full),
    .ss_addr(ss_addr), .ss_wdata(ss_wdata), .ss_we(ss_we), .ss_rdata(ss_rdata));

// NES serial order: A B Select Start Up Down Left Right. HID arrows, Z/X, Space/Enter.
// Kempston #1F целиком (как в R-Type, TSLib Input.Kempston): 0 вправо, 1 влево, 2 вниз, 3 вверх,
// 4 огонь (B Sega) — A NES, 6 A Sega — B NES, 5 C — Select, 7 START — Start.
wire [7:0] joy1 = {keys[79]|joystick0[0], keys[80]|joystick0[1],
    keys[81]|joystick0[2], keys[82]|joystick0[3], keys[40]|joystick0[7], keys[44]|joystick0[5],
    keys[29]|joystick0[6], keys[27]|joystick0[4]};
wire [2:0] joypad_out;
wire [1:0] joypad_clock;
reg [7:0] joypad_bits=8'hff;
reg [1:0] last_joypad_clock=0;
always @(posedge clk) begin
    if (ss_we && ss_addr==10'h00B) {last_joypad_clock,joypad_bits}<=ss_wdata[9:0];
    else begin
    if (joypad_out[0]) joypad_bits<=joy1;
    else if (!joypad_clock[0] && last_joypad_clock[0]) joypad_bits<={1'b1,joypad_bits[7:1]};
    last_joypad_clock<=joypad_clock;
    end
end
// Снимок платы: 0x00B — джойстик, 0x00C — байты SDRAM, которые видит NES ({PPU, CPU}). Мост
// снимка (save_full) затирает выходной регистр SDRAM; записанное в 0x00C NES видит вместо
// него до своего следующего чтения этого порта — как будто моста не было.
reg [7:0] din_cpu_ovr=0, din_ppu_ovr=0;
reg ovr_cpu=0, ovr_ppu=0, rd_cpu_d=0, rd_ppu_d=0;
wire [7:0] nes_din_cpu=ovr_cpu ? din_cpu_ovr : memory_din_cpu;
wire [7:0] nes_din_ppu=ovr_ppu ? din_ppu_ovr : memory_din_ppu;
always @(posedge clk) begin
    rd_cpu_d<=memory_read_cpu; rd_ppu_d<=memory_read_ppu;
    if (ss_we && ss_addr==10'h00C) begin
        din_cpu_ovr<=ss_wdata[7:0]; din_ppu_ovr<=ss_wdata[15:8]; ovr_cpu<=1; ovr_ppu<=1;
    end else begin
        if (reset_nes || memory_read_cpu && !rd_cpu_d) ovr_cpu<=0;
        if (reset_nes || memory_read_ppu && !rd_ppu_d) ovr_ppu<=0;
    end
end
wire [31:0] ss_rdata_nes;
assign ss_rdata=ss_rdata_nes | (ss_addr==10'h00B ? {22'b0,last_joypad_clock,joypad_bits} :
    ss_addr==10'h00C ? {16'b0,nes_din_ppu,nes_din_cpu} : 32'b0);
wire [4:0] joypad1_data={4'b0,joypad_bits[0]}, joypad2_data=5'b00001;

wire [21:0] memory_addr_cpu, memory_addr_ppu, loader_addr;
wire memory_read_cpu, memory_read_ppu, memory_write_cpu, memory_write_ppu;
wire [7:0] memory_din_cpu, memory_din_ppu, memory_dout_cpu, memory_dout_ppu;
wire [7:0] loader_write_data;
wire loader_write;
wire [63:0] loader_flags;
reg [63:0] mapper_flags=0;
reg loading_r=0, reset_nes=1, clkref=0;
wire [1:0] system_type=0;
wire [2:0] nes_ce;
wire [5:0] color;
wire [8:0] cycle, scanline;
wire int_audio=1;
wire ext_audio=(mapper_flags[7:0]==19)||(mapper_flags[7:0]==24)||(mapper_flags[7:0]==26);
wire loader_reset=loading && !loading_r;
reg f4_prev=0;
reg [7:0] soft_reset=0;
always @(posedge clk) begin
    loading_r<=loading;
    clkref<=~clkref;
    f4_prev<=keys[61];
    if (keys[61] && !f4_prev) soft_reset<=255;
    else if (soft_reset!=0) soft_reset<=soft_reset-1'b1;
    if (!loading && loading_r) begin reset_nes<=0; clkref<=1; end
    else if (loading) reset_nes<=1;
    if (!sys_resetn) reset_nes<=1;
    if (loader_done && !loader_fail) mapper_flags<=loader_flags;
end
NES nes(
    .clk(clk), .reset_nes(reset_nes | (soft_reset!=0) | save_hold), .cold_reset(1'b0),
    .pausecore(nes_pause), .corepaused(nes_paused), .clkref(clkref),
    .ss_addr(ss_addr), .ss_wdata(ss_wdata), .ss_we(ss_we), .ss_rdata(ss_rdata_nes),
    .sys_type(system_type), .nes_div(nes_ce),
    .mapper_flags(mapper_flags),
    .sample(sample), .color(color),
    .joypad_out(joypad_out), .joypad_clock(joypad_clock), 
    .joypad1_data(joypad1_data), .joypad2_data(joypad2_data),

    .fds_busy(1'b0), .fds_eject(1'b0), .diskside_req(), .diskside(2'b0),        // disk system
    .audio_channels(5'b11111),  // enable all channels
    
    .cpumem_addr(memory_addr_cpu),
    .cpumem_read(memory_read_cpu),
    .cpumem_din(nes_din_cpu),
    .cpumem_write(memory_write_cpu),
    .cpumem_dout(memory_dout_cpu),
    .ppumem_addr(memory_addr_ppu),
    .ppumem_read(memory_read_ppu),
    .ppumem_write(memory_write_ppu),
    .ppumem_din(nes_din_ppu),
    .ppumem_dout(memory_dout_ppu),

    .bram_addr(bram_addr), .bram_din(bram_din), .bram_dout(bram_dout), .bram_write(bram_write), .bram_override(),

    .cycle(cycle), .scanline(scanline),
    .int_audio(int_audio),    // VRC6
    .ext_audio(ext_audio),

    .apu_ce(), .gg(1'b0), .gg_code(129'b0), .gg_avail(), .gg_reset(1'b0), .emphasis(), .save_written()
);


reg loader_write_mem=0, loader_write_r=0;
reg [7:0] loader_write_data_mem=0;
reg [21:0] loader_addr_mem=0;
always @(posedge clk) begin
    loader_write_r<=loader_write;
    loader_write_mem<=loader_write || loader_write_r;
    if (loader_write) begin loader_addr_mem<=loader_addr; loader_write_data_mem<=loader_write_data; end
end
sdram_nes sdram(.clk(fclk), .clkref(clkref), .resetn(sys_resetn), .busy(sdram_busy),
    .SDRAM_DQ(IO_sdram_dq), .SDRAM_A(O_sdram_addr), .SDRAM_BA(O_sdram_ba),
    .SDRAM_nCS(O_sdram_cs_n), .SDRAM_nWE(O_sdram_wen_n), .SDRAM_nRAS(O_sdram_ras_n),
    .SDRAM_nCAS(O_sdram_cas_n), .SDRAM_CKE(O_sdram_cke), .SDRAM_DQM(O_sdram_dqm),
    // save_full: снимок на паузе — порт B у моста, запросы NES на обоих портах сняты (после
    // моста SDRAM видит их фронт заново и перечитывает тот же адрес)
    .addrA(memory_addr_ppu), .weA(memory_write_ppu && !save_hold && !save_full), .dinA(memory_dout_ppu),
    .oeA(memory_read_ppu && !save_hold && !save_full), .doutA(memory_din_ppu),
    .addrB(save_hold ? {4'b1111,save_offset[17:0]} : save_full ? save_offset : loading ? loader_addr_mem : memory_addr_cpu),
    .weB(save_hold ? (save_write && !save_offset[18]) : save_full ? save_write : loading ? loader_write_mem : memory_write_cpu),
    .dinB(save_hold || save_full ? save_dout : loading ? loader_write_data_mem : memory_dout_cpu),
    .oeB(save_hold ? (save_read && !save_offset[18]) : save_full ? save_read : !loading && memory_read_cpu), .doutB(memory_din_cpu),
    .rv_addr(20'b0), .rv_din(16'b0), .rv_ds(2'b0), .rv_dout(), .rv_req(1'b0), .rv_req_ack(), .rv_we(1'b0));
GameLoader loader(.clk(clk), .reset(!sys_resetn || loader_reset), .downloading(loading),
    .filetype(8'h02), .is_bios(1'b0), .invert_mirroring(1'b0),
    .indata(loader_do), .indata_clk(loader_do_valid), .mem_addr(loader_addr),
    .mem_data(loader_write_data), .mem_write(loader_write), .bios_download(),
    .mapper_flags(loader_flags), .busy(), .done(loader_done), .error(loader_fail), .rom_loaded());
nes2hdmi_vdac4 hdmi(.clk(clk), .resetn(sys_resetn), .color(color), .cycle(cycle), .scanline(scanline),
    .sample(sample >> 1), .f6(keys[64]), .overlay(1'b0),   // пропорции — F7 (HID 0x40)
    .osd_wr(osd_wr), .osd_addr(osd_addr), .osd_data(osd_data), .osd_show(osd_show), .osd_sel(osd_sel), .overlay_x(), .overlay_y(), .overlay_color(15'b0),
    .osd_band(osd_band), .osd_rule(osd_rule), .osd_bar(osd_bar),
    .aspect_requested(aspect_requested), .aspect_restore(aspect_restore), .aspect_value(aspect_value),
    .picture_size(picture_size),
    .clk_pixel(hclk), .clk_5x_pixel(hclk5), .tmds_clk_n(O_tmds_clk_n), .tmds_clk_p(O_tmds_clk_p),
    .tmds_d_n(O_tmds_data_n), .tmds_d_p(O_tmds_data_p));
endmodule
