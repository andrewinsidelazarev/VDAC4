//============================================================================
//  ZX Spectrum Next on VDAC4 (Tang Nano 20K on the VDAC4 board of ZX Evolution)
//  Derived from NextNano ZXNext_tang.sv. Changes for VDAC4 (docs/next-core.md in the VDAC4
//  project): no USB FPGA companion, LEDs, I2S DAC or m0s pins — on VDAC4 those pins carry
//  Evo, ESP and LINK nets. Keyboard (USB HID bitmap), mouse and joystick come from the ESP
//  over LINK (next_link); sound and the ESP UART of the Next go to the ESP; return to slot 0
//  (TS-Config) — through the BL616 (next_cp).
//============================================================================
//============================================================================
//  ZX Spectrum Next 
//  Simplified version for Tang Nano 20K
//  Based on MiSTer port by Alexey Melnikov
//
//  This program is free software; you can redistribute it and/or modify it
//  under the terms of the GNU General Public License as published by the Free
//  Software Foundation; either version 2 of the License, or (at your option)
//  any later version.
//
//============================================================================

module ZXNext_vdac4
(
    // Master input clock
    input         clk_27m,

    // Buttons (active low on Tang Nano 20K)
    input   [1:0]   btn,

    // LINK — ESP32-S3 VDAC4 (QSPI, ведущий — ESP): клавиатура, мышь, джойстик, звук, UART
    input  wire        link_sck,     // FPGA77 (GCLKT_1)
    input  wire        link_cs_n,    // FPGA18
    inout  wire [3:0]  link_io,      // FPGA71, 72, 55, 31
    output tri         link_irq_n,   // FPGA85: не используется

    // BL616: только запрос возврата в слот 0 (cp_spi ядер VDAC4)
    input  wire        cp_sck,       // FPGA13
    input  wire        cp_cs_n,      // FPGA86
    input  wire        cp_mosi,      // FPGA76
    output wire        cp_miso,      // FPGA75
    output wire        cp_irq_n,     // FPGA69

    // Выходы платы VDAC4 — в покое, как их держит ядро VDAC4 без работы
    output wire        mux_sel,      // FPGA56: выбор половины шины Evo
    output wire        pa_en,        // FPGA51: усилитель Tang — выключен
    output wire        ft_int_n,     // FPGA74 → Evo: INT_N FT812 неактивен
    output wire        ft_miso,      // FPGA79 → Evo: MISO FT812 — 0

    // Входы платы VDAC4 от Evo, которые ядру Next не нужны: описаны, чтобы их подтяжки были как у
    // ядер VDAC4 (ts_fb_hdmi_top.cst), а не подтяжка вверх Gowin по умолчанию. FPGA19/20 — общие
    // MOSI и такт SPI Evo, их же слушает ESP (GPIO11/12): у TS-Config такт подтянут вниз.
    input  wire [7:0]  mux_d,        // FPGA27…30, 41, 42, 48, 54: шина Evo
    input  wire        hsync_in,     // FPGA15
    input  wire        vsync_in,     // FPGA16
    input  wire        pclk_in,      // FPGA80
    input  wire        m_sel,        // FPGA73: 1 — Evo в режиме FT812 (плагин), 0 после взвода — сброс Evo
    input  wire        ft_sck,       // FPGA20: такт SPI Evo (и ESP)
    input  wire        ft_cs_n,      // FPGA17
    input  wire        ft_mosi,      // FPGA19: MOSI SPI Evo (и ESP)

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

    // Physical SD card interface (microSD самой Tang: NextZXOS)
    output        SD_SCK,
    output        SD_MOSI,
    input         SD_MISO,
    output        SD_CS,

    output            O_tmds_clk_p    ,
    output            O_tmds_clk_n    ,
    output     [2:0]  O_tmds_data_p   ,//{r,g,b}
    output     [2:0]  O_tmds_data_n

);

    wire        UART_RX;            // мост LINK → UART ESP Next
    wire        UART_TX;            // UART ESP Next → мост LINK

wire sdram_ready;

//============================================================================
// Clock generation
//============================================================================

wire clk_sys, CLK_14, CLK_7, CLK_140, CLK_140_P /*synthesis syn_keep=1*/;


// Video output (directly from core)
    wire  [2:0] RGB_R;
    wire  [2:0] RGB_G;
    wire  [2:0] RGB_B;
    wire        HSYNC_n;
    wire        VSYNC_n;
    wire        HBLANK_n;
    wire        VBLANK_n;

    // Joystick interface (directly from core)
    // active high = X Z Y START A C B U D L R
    wire UART_TX_orig;           
    // reg  [11:0] joy_left = 0; // 12'b111111111111;    // Zmień z [10:0]
    reg  [11:0] joy_right = 0; // 12'b111111111111;   // Zmień z [10:0]
    wire i2c_sda_i = 1'b1;       // Zmień z reg

  // assign led[0] = SD_CS;
  // assign led[1] = SD_MOSI;
  // assign led[2] = SD_MISO;
  // assign led[3] = 1'b1;
  // assign led[4] = 1'b1;
  // assign led[5] = VBlank_n;

    // Audio output (directly from core)
    wire [11:0] AUDIO_L;
    wire [11:0] AUDIO_R;

    // PS/2 keyboard interface (directly from core)
    reg  [10:0] ps2_key = 11'b00000000000;

wire [7:0] companion_dx, companion_dy;    // имена NextNano; источник — next_link (LINK от ESP)
wire       companion_strobe;
wire [2:0] link_mouse_btns;
wire [1:0] companion_btns = link_mouse_btns[1:0];

    // PS/2 mouse interface (directly from core)
    reg  [24:0] ps2_mouse;
    reg   [7:0] ps2_mouse_ext;

reg companion_strobe_prev;
wire packet_arrived = (companion_strobe ^ companion_strobe_prev);

reg signed [7:0] dx_held, dy_held;
reg              ps2_mouse_strobe;

always @(posedge clk_sys) begin
    companion_strobe_prev <= companion_strobe;
    if (next_reset) begin
        dx_held <= 0;
        dy_held <= 0;
        ps2_mouse_strobe <= 0;
    end else if (packet_arrived) begin
        dx_held <= companion_dx;
        // Evo уже передаёт ось Kempston: положительное Y — вверх.
        // Инверсия старого USB HID здесь меняла вверх и вниз местами.
        dy_held <= companion_dy;
        ps2_mouse_strobe <= ~ps2_mouse_strobe;
    end
end

assign ps2_mouse = {
    ps2_mouse_strobe,
    dy_held,
    dx_held,
    5'b00000,
    link_mouse_btns[2],  // M
    companion_btns[1],   // R
    companion_btns[0]    // L
};
assign ps2_mouse_ext = 8'b00000000;


    // I2C interface (directly from core, for RTC)
    wire        i2c_scl_o;
    wire        i2c_sda_o;

    // Directly from core
    wire  [1:0] cpu_speed;
    wire speed_restore;
    wire [1:0] speed_saved_in;
    wire [2:0] speed_saved_out;




wire pll_locked /*synthesis syn_keep=1*/;
wire sdram_pll_locked /*synthesis syn_keep=1*/;

Gowin_rPLL pll_inst (
    .clkin(clk_27m),
    .clkout(CLK_140),       // 140MHz - 0 phase
    .clkoutp(CLK_140_P),    // 140MHz - 180 phase for SDRAM
    .clkoutd(clk_sys),
    //  .clkoutd(),
    .lock(pll_locked)
);

wire clk_hdmi_pixel;    // 27 MHz
wire clk_hdmi_5x;       // 135 MHz
wire hdmi_pll_lock;

clk_hdmi_gowin hdmi_rpll
(
    .clkin_27(clk_sys),      // Tang Nano 20K HDMI crystal 27 MHz
    .clk_pixel_x5(clk_hdmi_5x),  // 135 MHz
    .clk_pixel(clk_hdmi_pixel),    // 27 MHz (dzielone z clk_pixel_x5 ÷5)
    .pll_locked(hdmi_pll_lock)
);

// CLKDIV #(
//   .DIV_MODE("4"),
//   .GSREN("false")
// ) CLKDIV_I1 (
//   .CLKOUT (clk_sys),
//   .HCLKIN (CLK_140),
//   .RESETN (pll_locked), 
//   .CALIB  (1'b1)
// );



//============================================================================
// Reset generation
//============================================================================

// wire hw_reset = btn[0];  // Przycisk resetu (active-low na Tang Nano)
wire hw_reset = 1'b0;

wire button0 = btn[0];
wire button1 = btn[1];

reg [25:0] reset_counter = 0;
wire reset_done = reset_counter[25];

always @(posedge clk_sys or negedge pll_locked) begin
    if (!pll_locked)
        reset_counter <= 0;
    else if (sdram_ready && !reset_done)   // <-- CHANGED: wait for SDRAM
        reset_counter <= reset_counter + 1;
end

wire reset = !reset_done || hw_reset;

reg [15:0] extra_delay = 0;
wire next_reset = reset || (extra_delay != 4096);

wire machine_reset;

always @(posedge clk_sys) begin
    if (reset)
        extra_delay <= 0;
    else if (extra_delay != 4096)
        extra_delay <= extra_delay + 1;
end

CLKDIV #(
  .DIV_MODE("2"),
  .GSREN("false")
) CLKDIV_I2 (
  .CLKOUT (CLK_14),
  .HCLKIN (clk_sys),
  .RESETN (pll_locked), 
  .CALIB  (1'b0)
);

CLKDIV #(
  .DIV_MODE("4"),
  .GSREN("false")
) CLKDIV_I3 (
  .CLKOUT (CLK_7),
  .HCLKIN (clk_sys),
  .RESETN (pll_locked), 
  .CALIB  (1'b0)
);

// CLKDIV2 #(
//   .GSREN("false")
// ) CLKDIV_I3 (
//   .CLKOUT (CLK_7),
//   .HCLKIN (CLK_14),
//   .RESETN (pll_locked)
// );



//============================================================================
// Default configuration values (replacing status bits from HPS)
//============================================================================

// Default settings - adjust as needed
localparam CFG_CPU_SPEED_SW  = 1'b0;       // CPU speed switch: 0 = normal
localparam CFG_JOYSTICK_SWAP = 1'b0;       // Joystick swap: 0 = no swap
// localparam CFG_MACHINE_ID    = 8'hDA;      // Machine ID: DA = ZX Next MISTER
// localparam CFG_MACHINE_ID    = 8'b00001010; //NEXT
localparam CFG_MACHINE_ID    = 8'b00001000; //emu
localparam CFG_BOARD_ISSUE   = 4'h0;       // Board issue: 0 = issue 4
localparam CFG_CENTER_IMAGE  = 1'b0;       // Center image: 1 = yes

// 0000 1000 	EMULATORS
// 0000 1010 	ZX Spectrum Next
// 1111 1010 	ZX Spectrum Next Anti-brick
// 1001 1010 	ZX Spectrum Next Core on UnAmiga Reloaded
// 1010 1010 	ZX Spectrum Next Core on UnAmiga
// 1011 1010 	ZX Spectrum Next Core on SiDi
// 1100 1010 	ZX Spectrum Next Core on MIST
// 1101 1010 	ZX Spectrum Next Core on MiSTer
// 1110 1010 	ZX Spectrum Next Core on ZX-DOS 

//============================================================================
// RAM interface signals
//============================================================================

wire [20:0] RAM_A_ADDR;
wire        RAM_A_REQ;
wire        RAM_A_REQ_LEVEL;
wire        RAM_A_CYCLE;
wire        RAM_A_RD_n;
wire  [7:0] RAM_A_DI;
wire  [7:0] RAM_A_DO;

wire        RAM_A_WAIT;

wire [20:0] RAM_B_ADDR;
wire        RAM_B_REQ;
wire        RAM_B_SLOT;
wire  [7:0] RAM_B_DO;

//============================================================================
// SDRAM controller
//============================================================================

ram u_sdram (
    .clk_ram(clk_sys),
    .clk(CLK_140),
    .clk_sdram(CLK_140_P),
    .init(~pll_locked),
    .sdram_ready(sdram_ready), 
    //.uart_tx(UART_TX),
    
    .O_sdram_clk(O_sdram_clk),
    .O_sdram_cke(O_sdram_cke),
    .O_sdram_cs_n(O_sdram_cs_n),
    .O_sdram_cas_n(O_sdram_cas_n),
    .O_sdram_ras_n(O_sdram_ras_n),
    .O_sdram_wen_n(O_sdram_wen_n),
    .O_sdram_addr(O_sdram_addr),
    .O_sdram_ba(O_sdram_ba),
    .O_sdram_dqm(O_sdram_dqm),
    .IO_sdram_dq(IO_sdram_dq),
    
    .RAM_A_ADDR(RAM_A_ADDR),
    .RAM_A_REQ(RAM_A_REQ),
    .RAM_A_REQ_LEVEL(RAM_A_REQ_LEVEL),
    .RAM_A_CYCLE(RAM_A_CYCLE),
    .RAM_A_RD_n(RAM_A_RD_n),
    .RAM_A_DI(RAM_A_DI),
    .RAM_A_DO(RAM_A_DO),
    .RAM_A_WAIT(RAM_A_WAIT),
    
    .RAM_B_ADDR(RAM_B_ADDR),
    .RAM_B_REQ(RAM_B_REQ),
    .RAM_B_SLOT(RAM_B_SLOT),
    .RAM_B_DO(RAM_B_DO),
    .port_reset(machine_reset)
);


//============================================================================
// Physical SD card signals (directly from core)
//============================================================================

wire sdss0;
wire sdss1;
wire sdclk;
wire sdmosi;
wire sdmiso = SD_MISO;

// assign SD_CS   = sdss0;
// assign SD_SCK  = sdclk;
// assign SD_MOSI = sdmosi;

// assign SD_CS   = reset ? 1'b1 : sdss0;  // deselect w resecie
// assign SD_SCK  = reset ? 1'b0 : sdclk;    // brak zegara w resecie
// assign SD_MOSI = reset ? 1'b0 : sdmosi;   // spokojna linia danych

// assign SD_CS   = sdss0;  // deselect w resecie
// assign SD_SCK  = sdclk;    // brak zegara w resecie
// assign SD_MOSI = sdmosi;   // spokojna linia danych

assign SD_CS   = next_reset ? 1'b1 : sdss0;
assign SD_SCK  = next_reset ? 1'b0 : sdclk;
assign SD_MOSI = next_reset ? 1'b0 : sdmosi;

//============================================================================
// Video signals
//============================================================================

wire [2:0] rgb_r;
wire [2:0] rgb_g;
wire [2:0] rgb_b;
wire HBlank_n, VBlank_n;
wire HSync_n, VSync_n;
wire ntsc;

assign RGB_R    = rgb_r;
assign RGB_G    = rgb_g;
assign RGB_B    = rgb_b;
assign HSYNC_n  = HSync_n;
assign VSYNC_n  = VSync_n;
assign HBLANK_n = HBlank_n;
assign VBLANK_n = VBlank_n;

//============================================================================
// Audio signals
//============================================================================

wire [11:0] aud_l, aud_r;

assign AUDIO_L = aud_l;
assign AUDIO_R = aud_r;

//============================================================================
// ZX Next core instantiation
//============================================================================


// assign led = dbg_leds;



// reg [5:0] led_reg;
// assign led = led_reg;

// always @(posedge clk_sys) begin
//     if (reset)
//         led_reg <= 6'b111111;
//     else if (RAM_A_ADDR == 21'h0004B00F && RAM_A_DO == 8'h90)
//         led_reg[0] <= 1'b0;
// end






reg cpu_speed_set_enable = 1'b1;

// always @(posedge clk_sys) begin
//     if (button0 == 1 && button1 == 1)
//         cpu_speed_set_enable <= 1'b0;
// end

wire [127:0] usb_hid_keyboard;

wire hdmi_pixel_en;
wire hdmi_lock;

wire cpu_cen_14;
wire cpu_cep_14;
wire cpu_cen;
wire cpu_cep;

wire [12:0] zxn_audio_R;
wire [12:0] zxn_audio_L;

reg [11:0] joy_left;
wire [7:0] joystick0;

always @(posedge clk_sys) begin
    if (reset)
        joy_left <= 0;
    else 
    begin
        joy_left <= {
            4'b0000,          // 
            joystick0[7],    // START
            joystick0[4],    // A (Fire1)
            joystick0[6],    // C (Fire3)
            joystick0[5],    // B (Fire2)
            joystick0[3],    // UP
            joystick0[2],    // DOWN
            joystick0[1],    // LEFT
            joystick0[0]     // RIGHT
        };
    end
end



zxnext_top zxnext_top
(
    .g_machine_id  (CFG_MACHINE_ID),
    .g_board_issue (CFG_BOARD_ISSUE),
    
    .CLK_28        (clk_sys),
    .CLK_14        (CLK_14),
    .CLK_7         (CLK_7),
    .cpu_cen(cpu_cen),
    .cpu_cep(cpu_cep),
    .cpu_cen_14(cpu_cen_14),
    .cpu_cep_14(cpu_cep_14),

    // .cpu_speed_set_enable (cpu_speed_set_enable),

    .SW_RESET      (next_reset),
    .HW_RESET      (next_reset),

    .CPU_SPEED_SW  (CFG_CPU_SPEED_SW),
    .CPU_SPEED     (cpu_speed),
    .SPEED_RESTORE (speed_restore),
    .SPEED_SAVED_IN (speed_saved_in),
    .SPEED_SAVED_OUT (speed_saved_out),
    .CPU_WAIT      (RAM_A_WAIT),

    .RAM_A_ADDR    (RAM_A_ADDR),
    .RAM_A_REQ     (RAM_A_REQ),
    .RAM_A_REQ_LEVEL (RAM_A_REQ_LEVEL),
    .RAM_A_CYCLE     (RAM_A_CYCLE),
    .RAM_A_RD_n    (RAM_A_RD_n),
    .RAM_A_DO      (RAM_A_DI),
    .RAM_A_DI      (RAM_A_DO),
    .RAM_B_ADDR    (RAM_B_ADDR),
    .RAM_B_REQ     (RAM_B_REQ),
    .RAM_B_SLOT    (RAM_B_SLOT),
    .RAM_B_DI      (RAM_B_DO),

    .ps2_key       (ps2_key),
    .ps2_mouse     (ps2_mouse),
    .ps2_mouse_ext (ps2_mouse_ext),

    .usb_hid_keyboard (usb_hid_keyboard),

    .sd_cs0_n_o    (sdss0),
    .sd_cs1_n_o    (sdss1),
    .sd_sclk_o     (sdclk),
    .sd_mosi_o     (sdmosi),
    .sd_miso_i     (sdmiso),

    .audio_L       (aud_l),
    .audio_R       (aud_r),

    .o_HDMI_PIXEL   (hdmi_pixel_en),
    .o_HDMI_LOCK    (hdmi_lock),
    .o_zxn_audio_L  (zxn_audio_L),
    .o_zxn_audio_R  (zxn_audio_R),

    .ear_port_i    (1'b0),  // No tape input

    .joy_left      (joy_left),
    .joy_right     (joy_right),

    .uart_rx_i     (UART_RX),
    .uart_tx_o     (UART_TX),
    .dbg_leds (),

    .i2c_scl_o     (i2c_scl_o),
    .i2c_sda_o     (i2c_sda_o),
    .i2c_sda_i     (i2c_sda_i),

    .RGB           ({rgb_r, rgb_g, rgb_b}),
    .RGB_VS_n      (VSync_n),
    .RGB_HS_n      (HSync_n),
    .RGB_VB_n      (VBlank_n),
    .RGB_HB_n      (HBlank_n),
    .RGB_NTSC      (ntsc),
    .center        (CFG_CENTER_IMAGE),
    .cpu_di_dbg (),
    .button0(button0),
    .button1(button1),
    .reset(machine_reset)
);

//============================================================================
// LINK (ESP) и BL616 — вместо USB FPGA companion
//============================================================================

wire [3:0] link_io_out;
wire       link_io_oe;
wire       boot_req;

next_link #(.VERSION(16'h0012)) next_link_inst (
    .sck          (link_sck),
    .cs_n         (link_cs_n),
    .io_in        (link_io),
    .io_out       (link_io_out),
    .io_oe        (link_io_oe),

    .clk          (clk_sys),
    .rst_n        (pll_locked),
    .keyboard     (usb_hid_keyboard),
    .mouse_dx     (companion_dx),
    .mouse_dy     (companion_dy),
    .mouse_btns   (link_mouse_btns),
    .mouse_strobe (companion_strobe),
    .joystick0    (joystick0),
    .audio_l      (zxn_audio_L),
    .audio_r      (zxn_audio_R),
    .uart_tx      (UART_TX),
    .uart_rx      (UART_RX),
    .speed_restore (speed_restore),
    .speed_saved_in (speed_saved_out),
    .speed_saved_out (speed_saved_in),
    .boot_req     (boot_req)
);

assign link_io    = link_io_oe ? link_io_out : 4'hz;
assign link_irq_n = 1'bz;

// Сброс Evo (M_SEL снят после взвода) — назад в слот 0 через BL616, как BOOT от ESP
wire evo_reset_req;
next_evo_reset next_evo_reset_inst (
    .clk   (clk_sys),
    .m_sel (m_sel),
    .req   (evo_reset_req)
);

next_cp next_cp_inst (
    .sck   (cp_sck),
    .cs_n  (cp_cs_n),
    .mosi  (cp_mosi),
    .miso  (cp_miso),
    .irq_n (cp_irq_n),
    .clk   (clk_sys),
    .boot  (boot_req | evo_reset_req)
);

// входы Evo — ради подтяжек (выводы описаны в nextVdac4.cst; M_SEL — ещё и сброс Evo, выше): держать их буферы
wire evo_unused /* synthesis syn_keep=1 */ = ^{mux_d, hsync_in, vsync_in, pclk_in, ft_sck, ft_cs_n, ft_mosi};

assign mux_sel  = 1'b0;
assign pa_en    = 1'b0;
assign ft_int_n = 1'b1;
assign ft_miso  = 1'b0;

// I2S к ЦАП Tang не выводится: на VDAC4 эти выводы — шина Evo и LINK; звук Next — ESP (next_link)


// scandoubler scandoubler (
//     .clk_sys  (clk_sys),        // CLK_28
//     .reset    (next_reset),// next_reset
//     .hs_in    (~HSync_n),       // active HIGH!
//     .vs_in    (~VSync_n),       // active HIGH!
//     .r_in     (rgb_r),
//     .g_in     (rgb_g),
//     .b_in     (rgb_b),
//     .hblank_n (HBlank_n),
//     .vblank_n (VBlank_n),
//     .o_r      (lcd_r),
//     .o_g      (lcd_g),
//     .o_b      (lcd_b),
//     .o_hs     (lcd_hs),
//     .o_vs     (lcd_vs),
//     .o_de     (lcd_de)
// );
// assign lcd_dclk = clk_sys;


// Video signals (CLK_HDMI domain, from hdmi_frame)
wire        toHDMI_blank;
wire        toHDMI_hsync;
wire        toHDMI_vsync;
wire [8:0]  toHDMI_rgb;

// TMDS parallel outputs (CLK_HDMI domain, from hdmi)
wire [9:0]  hdmi_red;
wire [9:0]  hdmi_green;
wire [9:0]  hdmi_blue;

// HDMI configuration (576p 50Hz - CEA-861 mode 17/18)
// Values from original zxnext_top_issue5.vhd
wire [9:0] hdmi_hactive    = 10'd144;   // min_hactive
wire [9:0] hdmi_hsync_beg  = 10'd12;    // min_hsync
wire [9:0] hdmi_hsync_end  = 10'd75;    // max_hsync
wire [9:0] hdmi_hlast      = 10'd863;   // max_hc
wire [9:0] hdmi_vactive    = 10'd49;    // min_vactive
wire [9:0] hdmi_vsync_beg  = 10'd5;     // min_vsync
wire [9:0] hdmi_vsync_end  = 10'd9;     // max_vsync
wire [9:0] hdmi_vlast      = 10'd624;   // max_vc

// Scanlines disabled, audio always enabled
wire [1:0] hdmi_scanlines  = 2'b00;
wire       hdmi_audio_en   = 1'b1;



// hdmi_frame: re-times pre-scandoubler RGB (CLK_14) to 720x576p (CLK_HDMI)
// hdmi_frame hdmi_frame_inst (
//     .i_reset_async_n    (1'b1), //(hdmi_pll_lock),

//     .i_scanlines        (hdmi_scanlines),

//     .i_CLK_RGB          (CLK_14),
//     .i_CLK_RGB_EN       (hdmi_pixel_en),
//     .i_rgb_sync         (hdmi_lock),
//     .i_rgb              ({rgb_r, rgb_g, rgb_b}),               // 9-bit from zxnext_top

//     .i_CLK_HDMI         (clk_hdmi_pixel),

//     .o_blank            (toHDMI_blank),
//     .o_vsync_n          (toHDMI_vsync),
//     .o_hsync_n          (toHDMI_hsync),
//     .o_rgb              (toHDMI_rgb),

//     .i_HACTIVE          (hdmi_hactive),
//     .i_HSYNC_BEG        (hdmi_hsync_beg),
//     .i_HSYNC_END        (hdmi_hsync_end),
//     .i_HLAST            (hdmi_hlast),
//     .i_VACTIVE          (hdmi_vactive),
//     .i_VSYNC_BEG        (hdmi_vsync_beg),
//     .i_VSYNC_END        (hdmi_vsync_end),
//     .i_VLAST            (hdmi_vlast)
// );

hdmi_framer_gowin hdmi_framer_inst (
    // Write side (clk_sys domain) - same signals as scandoubler gets
    .clk_sys        (clk_sys),
    .reset          (next_reset),
    .hs_in          (~HSync_n),       // active HIGH (HSync_n is active-low from zxnext_top)
    .vs_in          (~VSync_n),       // active HIGH
    .hblank_n       (HBlank_n),       // already active-high "not blanking"
    .vblank_n       (VBlank_n),
    .rgb_in         ({rgb_r, rgb_g, rgb_b}),
    .mode_60 (ntsc),

    // Read side (clk_hdmi_pixel domain)
    .clk_hdmi_pixel (clk_hdmi_pixel),

    // HDMI output signals (to hdmi.vhd)
    .o_blank        (toHDMI_blank),
    .o_hsync        (toHDMI_hsync),
    .o_vsync        (toHDMI_vsync),
    .o_rgb          (toHDMI_rgb)
);

// hdmi: TMDS encoder + audio packet generator (Alexey Spirkov)
// RGB 3:3:3 expanded to 8:8:8 via pattern replication (as in original)
hdmi #(
    .FREQ   (28125000),
    .FS     (48000),
    .CTS    (28125),
    .N      (6144)
) hdmi_inst (
    .I_CLK_PIXEL        (clk_hdmi_pixel),

    .I_R                ({toHDMI_rgb[8:6], toHDMI_rgb[8:6], toHDMI_rgb[8:7]}),
    .I_G                ({toHDMI_rgb[5:3], toHDMI_rgb[5:3], toHDMI_rgb[5:4]}),
    .I_B                ({toHDMI_rgb[2:0], toHDMI_rgb[2:0], toHDMI_rgb[2:1]}),
    .I_BLANK            (toHDMI_blank),
    .I_HSYNC            (toHDMI_hsync),
    .I_VSYNC            (toHDMI_vsync),
    .I_576P_N           (1'b1),          // 0 = 50Hz (576p), 1 = 60Hz (480p)

    .I_AUDIO_ENABLE     (1'b0),    // VDAC4: звук Next — только на ЦАП ESP (LINK), не в HDMI
    .I_AUDIO_PCM_L      ({zxn_audio_L, 2'b000}),  // 13-bit -> 16-bit signed
    .I_AUDIO_PCM_R      ({zxn_audio_R, 2'b000}),

    .O_RED              (hdmi_red),
    .O_GREEN            (hdmi_green),
    .O_BLUE             (hdmi_blue)
);

hdmi_out_gowin hdmi_out_inst (
    .clk_pixel          (clk_hdmi_pixel),
    .clk_pixel_x5       (clk_hdmi_5x),
    .reset              (next_reset), // (~hdmi_pll_lock),

    .tmds_red           (hdmi_red),
    .tmds_green         (hdmi_green),
    .tmds_blue          (hdmi_blue),

    .tmds_clk_p         (O_tmds_clk_p),
    .tmds_clk_n         (O_tmds_clk_n),
    .tmds_data_p        (O_tmds_data_p),
    .tmds_data_n        (O_tmds_data_n)
);



// assign led[0] = ~req_during_reset;  // LED ON = były requesty podczas resetu
// assign led[1] = next_reset;          // LED ON = system w resecie

//assign led[0] = ~cpu_speed_set_enable;

// assign led[0] = ~dbg_leds[0];
// assign led[1] = ~dbg_leds[1];
 // assign led[0] =  ~RAM_B_REQ;
 // assign led[1] =  ~RAM_A_REQ;

// assign led[0] = ~SD_CS;
// assign led[1] = ~SD_SCK;
// светодиодов нет: на VDAC4 выводы 15–20 — HS/VS Evo и линии FT/LINK от ESP
endmodule
