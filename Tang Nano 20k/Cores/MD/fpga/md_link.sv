// SPDX-License-Identifier: GPL-3.0-or-later
// LINK ядра Mega Drive VDAC4 (03.11.2026): ведомый QSPI next_qspi, домен MCLK 54 МГц.
// Общие с ядром NES адреса (номера слов): 00 метка VDC4, 01 VERSION, 02 {boot, ошибка}, 0f проба,
// 21 размер картинки {535A, size}, 30 меню {show, sel}, 31 пауза, 34/35/36 оформление меню,
// 3c BOOT (424F4F54 — назад в слот 0), 40–42 и окно 800–9ff — мост к microSD Tang (fb_sdspi),
// 0c00–0dff — текст меню (nes_osd).
//
// Своё у Mega Drive — всё, что можно, делает ESP (MD.ELF):
//   50 W/R управление: [0] машина работает (0 — сброс, загрузка), [2] PAL, [3] экспорт (не Япония),
//      [4] пад 3 кнопки, [8] SRAM_QUIRK, [9] SRAM00_QUIRK, [10] NORAM_QUIRK, [11] FMBUSY_QUIRK,
//      [12] SCHAN_QUIRK, [13] FAST_FIFO — ESP берёт их из заголовка ROM и своей таблицы игр.
//   51 W/R размер ROM в байтах.
//   52 W/R пады {пад 2 [27:16], пад 1 [11:0]}: {Z,Y,X,Mode,Start,C,B,A,Вверх,Вниз,Влево,Вправо}, 1 —
//      нажата. Клавиатуру и Kempston в пад раскладывает ESP; горячие клавиши — тоже ESP.
//   53 R состояние {развёртка подстроена [31], машина работает [30], 14'b0, кадров VDP [15:0]}.
//   Мост SDRAM (ROM, ОЗУ, SRAM, видеопамять — ESP загружает и сохраняет; адреса — md_vdac4.sv):
//   58 W/R адрес 32-битного слова SDRAM [20:0] для пакета; 59 W {seq[31:16], запись[15], слов[6:0]
//      (1..64)}, R последний выполненный seq; 5a R {ошибка[1], занят[0]};
//   1400–143f W/R буфер пакета (64 слова). Пакет seq принимается один раз и только следующим по
//   порядку; данные буфера не трогать, пока пакет не выполнен.
//   Звук: 08 R {обороты [31:16], 6'b0, указатель записи в коде Грея [9:0]}, 09 R 1024, 0a R 54000
//   (MCLK, кГц), 0b R время YM в тактах синтеза [19:0] (MCLK/42; на паузе стоит), 1800–1bff R кольцо
//   событий md_snd_events (1024 слова).
//   Снимок F5/F6 (ядро 0.5): 5c W/R {[1] запрос прерывания 68000 уровня 7, [0] вставка $A12000 видна 68000},
//      R [2] — 68000 принял уровень 7; 5d W адрес слова вставки, R {адрес [25:16], слово [15:0]};
//      5e W слово во вставку по адресу, адрес +1; 5f W/R команда обработчику (слово $A127FE).
//   Снимок, Z80 и прочее (ядро 0.6; system.sv, правка 6 prepare.py):
//   60 W/R Z80 {[3] держать T80 в сбросе, на снятии — DIRSet, [2] NMI, [1] отпустить, [0] взвести вставку $0066 (фронт)},
//      R [4] Z80 стоит во вставке, [5] вставка ещё действует;
//   61–67 W регистры для загрузки T80 (DIR [31:0] ... [211:192]), R регистры T80 сейчас (REG);
//   68/69 W/R банки {[45:41..11] BANK_REG 7..1, [10] BANK_ROM, [9] BANK_SRAM, [8:0] BAR} (R — сейчас,
//      W — для загрузки), 6a/6b — так же таймеры YM (md_ym_stub st_q); 6c W — загрузить банки и таймеры;
//   6d W адрес отвода VDP, R {адрес [15:8], байт [7:0]} (00-1f регистры, 20/21 ADDR, 22 {PENDING, CODE,
//      ADDR[16]}). Мост: бит 14 в 59 — пакет из ОЗУ Z80 вместо SDRAM (58 — адрес байта, слово = байт).
`default_nettype none
module md_link(
    input  wire        sck, cs_n, input wire [3:0] io_in,
    output wire [3:0]  io_out, output wire io_oe,
    input  wire        clk, rst_n,
    output reg         boot_req=0,
    // меню
    output wire        osd_wr, output wire [8:0] osd_addr, output wire [31:0] osd_data,
    output reg         osd_show=0, output reg [5:0] osd_sel=6'd63,
    output reg [29:0]  osd_band=0, osd_rule=0, output reg [31:0] osd_bar=0,
    output reg [2:0]   picture_size=0,   // [1:0] — 100/95/90 %, [2] — 16:9 (ядро 0.2)
    // microSD Tang
    output wire        sd_cs_n, sd_sck, sd_mosi, input wire sd_miso,
    // машина
    output reg [15:0]  ctrl=0,
    output reg [24:0]  rom_size=0,
    output reg [23:0]  pads=0,
    output reg         pause=0,
    input  wire        frame,          // начало кадра VDP (уровень гашения кадра)
    input  wire        video_locked,   // домен HDMI
    input  wire [13:0] diag,           // 03.11.2026, плата: {PLL HDMI, PLL MCLK, сбросы развёртки [1:0], перескоки [1:0],
                                       //   кадры HDMI [7:0]} — в 53 [29:16]
    input  wire [63:0] z80_dbg,        // состояние Z80 (system.sv Z80_DBG) — 54 [31:0], 55 [63:32]; домен clk
    // снимок: вставка 68000 и прерывание уровня 7 (system.sv SNAP_*)
    output reg         snap_act=0, snap_irq=0,
    output reg [15:0]  snap_cmd=0,
    output reg [9:0]   snap_ovl_a=0,
    output reg         snap_ovl_we=0,
    output reg [15:0]  snap_ovl_d=0,
    input  wire [15:0] snap_ovl_q,
    input  wire        snap_taken,
    // снимок: Z80, ОЗУ Z80, банки, таймеры YM, VDP (ядро 0.6)
    output reg [3:0]   snap_z80_ctl=0,
    input  wire [1:0]  snap_z80_st,
    input  wire [211:0] snap_z80_reg,
    output reg [211:0] snap_z80_dir=0,
    output reg [12:0]  snap_zram_a=0,
    output reg         snap_zram_we=0,
    output reg [7:0]   snap_zram_d=0,
    input  wire [7:0]  snap_zram_q,
    input  wire [63:0] snap_sys_q, snap_ym_q,
    output reg [63:0]  snap_sys_d=0, snap_ym_d=0,
    output reg         snap_sys_we=0,
    output reg [7:0]   snap_vdp_a=0,
    input  wire [7:0]  snap_vdp_q,
    // звук
    input  wire        snd_push, input wire [31:0] snd_word,
    input  wire [19:0] snd_time,       // время YM (md_snd_events) — домен clk
    // мост SDRAM (порт h md_sdram)
    output reg         h_req=0, input wire h_ack,
    output reg         h_we=0,
    output reg [22:2]  h_addr=0,
    output reg [31:0]  h_d=0,
    input  wire [31:0] h_q
);
wire wr_stb;
wire [17:0] wr_addr;
wire [31:0] wr_data;
wire [12:0] rd_addr;
reg [31:0] rd_data;
next_qspi qspi(.sck(sck), .cs_n(cs_n), .io_in(io_in), .io_out(io_out), .io_oe(io_oe),
    .clk(clk), .rst_n(rst_n), .wr_stb(wr_stb), .wr_addr(wr_addr), .wr_data(wr_data),
    .rd_addr(rd_addr), .rd_data(rd_data));

assign osd_wr=wr_stb && wr_addr>=18'h0c00 && wr_addr<18'h0e00;
assign osd_addr=wr_addr[8:0];
assign osd_data=wr_data;

wire [31:0] sd_ctrl_rd, sd_ptr_rd, sd_stat, sd_rx_q;
fb_sdspi #(.CLK_KHZ(54000)) sdspi(.clk(clk), .rst_n(rst_n),
    .sd_cs_n(sd_cs_n), .sd_sck(sd_sck), .sd_mosi(sd_mosi), .sd_miso(sd_miso),
    .ctrl_we(wr_stb && wr_addr==18'h40), .ptr_we(wr_stb && wr_addr==18'h41),
    .cmd_we(wr_stb && wr_addr==18'h42), .wdata(wr_data),
    .ctrl_rd(sd_ctrl_rd), .ptr_rd(sd_ptr_rd), .status(sd_stat),
    .tx_we(wr_stb && wr_addr[17:9]==9'd4), .tx_waddr(wr_addr[8:0]), .tx_wdata(wr_data),
    .rd_clk(sck), .rd_addr(rd_addr[8:0]), .rd_q(sd_rx_q));

// --- регистры ---
reg [31:0] scratch=0;
reg [15:0] frames=0;
reg frame_d=0;
reg [1:0] lock_s=0;
always @(posedge clk) lock_s<={lock_s[0],video_locked};
reg [13:0] diag_s0=0, diag_s=0;          // только для глаз: счётчик из чужого домена читать дважды
always @(posedge clk) begin diag_s0<=diag; diag_s<=diag_s0; end
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        scratch<=0; boot_req<=0; osd_show<=0; osd_sel<=6'd63; osd_band<=0; osd_rule<=0; osd_bar<=0;
        picture_size<=0; ctrl<=0; rom_size<=0; pads<=0; pause<=0; frames<=0; frame_d<=0;
    end else begin
        frame_d<=frame;
        if (frame_d && !frame) frames<=frames+16'd1;
        if (wr_stb) case (wr_addr)
            18'h0f: scratch<=wr_data;
            18'h21: if (wr_data[31:16]==16'h535a) picture_size<=wr_data[2:0];
            18'h30: begin osd_show<=wr_data[0]; osd_sel<=wr_data[13:8]; end
            18'h31: pause<=wr_data[0];
            18'h34: osd_band<=wr_data[29:0];
            18'h35: osd_rule<=wr_data[29:0];
            18'h36: osd_bar<=wr_data;
            18'h3c: if (wr_data==32'h424f4f54) boot_req<=1;
            18'h50: ctrl<=wr_data[15:0];
            18'h51: rom_size<=wr_data[24:0];
            18'h52: pads<={wr_data[27:16],wr_data[11:0]};
            default: ;
        endcase
    end
end

// --- снимок: управление и вставка 68000 (system.sv, правка 5 prepare.py) ---
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        snap_act<=0; snap_irq<=0; snap_cmd<=0; snap_ovl_a<=0; snap_ovl_we<=0; snap_ovl_d<=0;
        snap_z80_ctl<=0; snap_z80_dir<=0; snap_sys_d<=0; snap_ym_d<=0; snap_sys_we<=0; snap_vdp_a<=0;
    end else begin
        snap_ovl_we<=0; snap_sys_we<=0;
        if (snap_ovl_we) snap_ovl_a<=snap_ovl_a+10'd1;
        if (wr_stb) case (wr_addr)
            18'h5c: begin snap_act<=wr_data[0]; snap_irq<=wr_data[1]; end
            18'h5d: snap_ovl_a<=wr_data[9:0];
            18'h5e: begin snap_ovl_d<=wr_data[15:0]; snap_ovl_we<=1; end
            18'h5f: snap_cmd<=wr_data[15:0];
            18'h60: snap_z80_ctl<=wr_data[3:0];
            18'h61: snap_z80_dir[31:0]<=wr_data;
            18'h62: snap_z80_dir[63:32]<=wr_data;
            18'h63: snap_z80_dir[95:64]<=wr_data;
            18'h64: snap_z80_dir[127:96]<=wr_data;
            18'h65: snap_z80_dir[159:128]<=wr_data;
            18'h66: snap_z80_dir[191:160]<=wr_data;
            18'h67: snap_z80_dir[211:192]<=wr_data[19:0];
            18'h68: snap_sys_d[31:0]<=wr_data;
            18'h69: snap_sys_d[63:32]<=wr_data;
            18'h6a: snap_ym_d[31:0]<=wr_data;
            18'h6b: snap_ym_d[63:32]<=wr_data;
            18'h6c: snap_sys_we<=1;
            18'h6d: snap_vdp_a<=wr_data[7:0];
            default: ;
        endcase
    end
end

// --- мост SDRAM: пакеты по 64 слова ---
reg [31:0] hbuf [0:63];
reg [31:0] hbuf_q=0, hbuf_host=0;
reg [20:0] h_base=0;
reg [5:0] h_pos=0;
reg [6:0] h_count=0;
reg [1:0] h_st=0;
reg h_busy=0, h_err=0, h_writing=0, h_z80=0;
reg [1:0] h_zw=0;
reg [15:0] h_seq=0, h_done=0;
wire h_got=h_st==2'd3 && (h_z80 ? h_zw==2'd0 : h_req==h_ack);
always @(posedge clk) begin
    hbuf_q<=hbuf[h_pos];
    if (wr_stb && wr_addr>=18'h1400 && wr_addr<18'h1440 && !h_busy) hbuf[wr_addr[5:0]]<=wr_data;
    else if (h_got && !h_writing) hbuf[h_pos]<=h_z80 ? {24'b0,snap_zram_q} : h_q;
end
always @(posedge sck) hbuf_host<=hbuf[rd_addr[5:0]];
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        h_base<=0; h_pos<=0; h_count<=0; h_st<=0; h_busy<=0; h_err<=0; h_writing<=0; h_seq<=0; h_done<=0;
        h_z80<=0; h_zw<=0; snap_zram_a<=0; snap_zram_we<=0; snap_zram_d<=0;
    end else begin
        snap_zram_we<=0;
        if (wr_stb && wr_addr==18'h58 && !h_busy) h_base<=wr_data[20:0];
        if (wr_stb && wr_addr==18'h59 && wr_data[31:16]!=h_done) begin
            if (!h_busy && wr_data[31:16]==h_done+16'd1 && wr_data[6:0]!=0 && wr_data[6:0]<=7'd64) begin
                h_seq<=wr_data[31:16]; h_writing<=wr_data[15]; h_z80<=wr_data[14]; h_count<=wr_data[6:0];
                h_pos<=0; h_busy<=1; h_err<=0; h_st<=2'd1;
            end else h_err<=1;
        end
        case (h_st)
            2'd1: h_st<=2'd2;                       // hbuf_q — слово h_pos
            2'd2: if (h_z80) begin                  // ОЗУ Z80: адрес — такт, слово — через такт
                snap_zram_a<=h_base[12:0]+{7'b0,h_pos}; snap_zram_d<=hbuf_q[7:0]; snap_zram_we<=h_writing;
                h_zw<=2'd2; h_st<=2'd3;
            end else begin
                h_addr<=h_base+{15'b0,h_pos}; h_d<=hbuf_q; h_we<=h_writing; h_req<=~h_req; h_st<=2'd3;
            end
            2'd3: if (h_z80 && h_zw!=2'd0) h_zw<=h_zw-2'd1;
            else if (h_got) begin
                if ({1'b0,h_pos}+7'd1==h_count) begin h_busy<=0; h_done<=h_seq; h_st<=2'd0; end
                else begin h_pos<=h_pos+6'd1; h_st<=2'd1; end
            end
            default: ;
        endcase
    end
end

// --- кольцо событий звука ---
reg [31:0] sring [0:1023];
reg [9:0] s_wp=0, s_wp_g=0;
reg [15:0] s_wraps=0, s_wraps_r=0;
reg [31:0] s_q=0;
always @(posedge clk) begin
    if (snd_push) begin
        sring[s_wp]<=snd_word; s_wp<=s_wp+10'd1;
        if (&s_wp) s_wraps<=s_wraps+16'd1;
    end
    s_wp_g<=s_wp^(s_wp>>1); s_wraps_r<=s_wraps;
end
always @(posedge sck) s_q<=sring[rd_addr[9:0]];

always @* begin
    rd_data=0;
    if (rd_addr>=13'h1800 && rd_addr<13'h1c00) rd_data=s_q;
    else if (rd_addr>=13'h1400 && rd_addr<13'h1440) rd_data=hbuf_host;
    else if (rd_addr>=13'h0800 && rd_addr<13'h0a00) rd_data=sd_rx_q;
    else case (rd_addr)
        13'h00: rd_data=32'h56444334;
        13'h01: rd_data=32'h06010008;          // Mega Drive (личность 06), ядро 0.8: HDMI 720p60/720p50 по стандарту
        13'h02: rd_data={30'b0,boot_req,1'b0};
        13'h08: rd_data={s_wraps_r,6'b0,s_wp_g};
        13'h09: rd_data=32'd1024;
        13'h0a: rd_data=32'd54000;
        13'h0b: rd_data={12'b0,snd_time};
        13'h0f: rd_data=scratch;
        13'h21: rd_data={16'h535a,13'b0,picture_size};
        13'h30: rd_data={16'h4f53,2'b0,osd_sel,6'b0,pause,osd_show};
        13'h31: rd_data={31'b0,pause};
        13'h34: rd_data={2'b0,osd_band};
        13'h35: rd_data={2'b0,osd_rule};
        13'h36: rd_data=osd_bar;
        13'h40: rd_data=sd_ctrl_rd;
        13'h41: rd_data=sd_ptr_rd;
        13'h42: rd_data=sd_stat;
        13'h50: rd_data={16'b0,ctrl};
        13'h51: rd_data={7'b0,rom_size};
        13'h52: rd_data={4'b0,pads[23:12],4'b0,pads[11:0]};
        13'h53: rd_data={lock_s[1],ctrl[0],diag_s,frames};
        13'h54: rd_data=z80_dbg[31:0];
        13'h55: rd_data=z80_dbg[63:32];
        13'h58: rd_data={11'b0,h_base};
        13'h59: rd_data={16'b0,h_done};
        13'h5a: rd_data={30'b0,h_err,h_busy};
        13'h5c: rd_data={29'b0,snap_taken,snap_irq,snap_act};
        13'h5d: rd_data={6'b0,snap_ovl_a,snap_ovl_q};
        13'h5f: rd_data={16'b0,snap_cmd};
        13'h60: rd_data={26'b0,snap_z80_st,snap_z80_ctl};
        13'h61: rd_data=snap_z80_reg[31:0];
        13'h62: rd_data=snap_z80_reg[63:32];
        13'h63: rd_data=snap_z80_reg[95:64];
        13'h64: rd_data=snap_z80_reg[127:96];
        13'h65: rd_data=snap_z80_reg[159:128];
        13'h66: rd_data=snap_z80_reg[191:160];
        13'h67: rd_data={12'b0,snap_z80_reg[211:192]};
        13'h68: rd_data=snap_sys_q[31:0];
        13'h69: rd_data=snap_sys_q[63:32];
        13'h6a: rd_data=snap_ym_q[31:0];
        13'h6b: rd_data=snap_ym_q[63:32];
        13'h6d: rd_data={16'b0,snap_vdp_a,snap_vdp_q};
        default: ;
    endcase
end
endmodule
`default_nettype wire
