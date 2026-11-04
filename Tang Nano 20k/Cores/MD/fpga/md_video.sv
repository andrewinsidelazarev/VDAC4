// SPDX-License-Identifier: GPL-3.0-or-later
// Картинка Mega Drive на HDMI 720p для VDAC4 (03.11.2026) — без кадрового буфера.
//
// VDP пишет строки в кольцо на 64 строки (BSRAM), развёртка 720p читает их. Кадр Mega Drive и кадр HDMI
// идут в ногу: так нет ни выпавших, ни повторённых кадров, а картинка запаздывает меньше чем на
// миллисекунду. SDRAM видео не занимает.
//
// Развёртка — стандарт CEA-861 (ядро 0.8, 04.11.2026; пользователь: «нужно приведение сигнала к одному из
// стандартных»): NTSC — 1280 x 720p60 (VIC 4: строка 1650 точек, кадр 750 строк), PAL — 1280 x 720p50 (VIC 19:
// строка 1980, кадр 750), обе на 74,25 МГц. Все строки и кадры одной длины. До 0.8 развёртку подгоняли под
// кадр VDP длиной последней строки (1645 x 749, последняя −50 точек; 1965 x 749, +98): карта захвата это брала,
// монитор на плате терял синхронизацию с началом игры.
//
// Синхронизация (как framebuffer_sync.sv mdtang). Mega Drive на MCLK 54 МГц (ровно 2 x 27) быстрее HDMI:
// кадр NTSC — 262 строки по 3420 тактов, 60,265 Гц против 60,000; PAL — 313 строк, 50,446 против 50,000. Поэтому
// подстраивается Mega Drive: дописав строку 0 кадра, VDP встаёт вместе со всей машиной (sync_stall: 68000, Z80,
// таймеры YM, VDP — system.sv, SYNC_STALL), пока развёртка не дойдёт до строки REL (746). Стоянка — около
// 73 мкс на кадр в NTSC и 150 мкс в PAL, игра идёт на 0,44 / 0,89 % медленнее (ближе к настоящей приставке:
// 59,92 и 49,70 Гц). Звук на ESP идёт по времени YM, оно на стоянке стоит вместе с машиной.
//
// Почему 64 строки. Строка 0 кадра VDP пишется до стоянки, строка 1 — с 746-й строки HDMI, читается с 4-й
// (100 %): запас 1 строка HDMI. Отставание чтения от записи к концу кадра: NTSC — до 26 строк, PAL 224 строки —
// до 59 (95 и 90 % — 56 и 53), PAL 240 — до 50. Кольцо — 64 строки по 320 точек, 12 бит (R4 G4 B4): 20480
// слов, 15 блоков BSRAM.
//
// Масштаб — счётчиками Брезенхэма, как в framebuffer.sv mdtang: ширина 320 (H40) или 256 (H32) — в
// окно 4:3 шириной 960/912/864, высота 224 и 240 — в 720/684/648 строк (размер 100/95/90 %, как SIZE
// TS-Config). Ядро 0.7 (04.11.2026, пользователь: «почему по вертикали не на всю высоту?»): NTSC 224 строки —
// тоже на всю высоту, как на телевизоре (960 x 720 — кадр 320 x 224 в пропорциях 4:3); до 0.7 — 672 (x3).
// Окно по центру, вокруг — чёрное. Поверх — меню nes_osd. 16:9 (ядро 0.2, как F7 у NES): ширина
// 1280/1216/1152, слева 0/32/64, высота та же.
//
// Чересстрочка (03.11.2026). У этого VDP оба поля одной длины (262/313 строк), полстроки сдвигает
// только кадровый синхроимпульс, — синхронизация та же. Режим 1 (LSM 01) — те же строки в
// обоих полях, выводится как обычный. Двойное разрешение (LSM 11: 320 x 448/480, меню игр, Sonic 2 на
// двоих): в поле 224/240 строк — чётные (поле 0) или нечётные (поле 1) строки картинки. Кадрового
// буфера нет, поэтому каждое поле показывается само, на своём месте (bob): источник — 448/480
// логических строк в том же окне, строка кольца — (логическая − поле)/2; поле 1 ниже поля 0 на
// логическую строку (1,5 строки HDMI). Кольцо пишется и читается как обычно — по 224/240 строк.
`default_nettype none
module md_video(
    // --- домен MCLK: выход VDP ---
    input  wire        mclk,
    input  wire        ce_pix, hblank, vblank, hsync,
    input  wire [3:0]  r, g, b,
    input  wire        h40, v30,
    input  wire        lace, field,    // двойное разрешение (LSM 11) и поле: 1 — нечётные строки
    input  wire        pal,            // NTSC/PAL — меняет развёртку (720p60 / 720p50)
    input  wire        blank,          // загрузка игры, сброс: чёрный экран
    output reg         sync_stall=0,   // машина стоит: VDP дописал строку 0, ждёт развёртку (домен MCLK)
    // --- меню (домен clk_osd) ---
    input  wire        clk_osd,
    input  wire        osd_wr, input wire [8:0] osd_addr, input wire [31:0] osd_data,
    input  wire        osd_show, input wire [5:0] osd_sel,
    input  wire [29:0] osd_band, input wire [29:0] osd_rule, input wire [31:0] osd_bar,
    input  wire [2:0]  picture_size,   // [1:0]: 0 — 100 %, 1 — 95 %, 2 — 90 %; [2] — 16:9
    // --- HDMI ---
    input  wire        hclk,           // 74,25 МГц
    input  wire        hclk5,          // 371,25 МГц
    input  wire        hreset,
    output wire        tmds_clk_p, tmds_clk_n,
    output wire [2:0]  tmds_d_p, tmds_d_n,
    output reg         locked=0,       // для LINK: кадры VDP идут в ногу с развёрткой (домен hclk)
    output reg  [7:0]  hframes=0,      // кадры развёртки HDMI — диагностика на плате (53 [23:16])
    output reg  [1:0]  hres_cnt=0,     // сбросы развёртки (53 [27:26]) и кадры VDP, опоздавшие к развёртке
    output reg  [1:0]  jump_cnt=0      //   (53 [25:24]: стоянка дольше 65 535 тактов MCLK, 1,2 мс)
);

// =====================================================================================
// Запись: VDP -> кольцо. Счётчики — как x/y в mdtang_top: после гашения кадра до первого
// гашения строки идут 15 лишних точек, их отсекает hsync_seen.
// =====================================================================================
reg ce_r=0, hbl_r=0, vbl_r=0, hsync_seen=0;
reg [8:0] wx=0;
reg [5:0] wl=0;                 // строка кольца
reg [14:0] wbase=0;             // wl*320
reg frame_tog=0, h40_m=0, v30_m=0, lace_m=0, field_m=0;
reg [14:0] waddr=0;
reg [11:0] wdata=0;
reg wen=0;
always @(posedge mclk) begin
    ce_r<=ce_pix; hbl_r<=hblank; vbl_r<=vblank;
    wen<=0;
    if (vblank) begin wl<=0; wbase<=0; hsync_seen<=0; end
    if (hsync) hsync_seen<=1;
    if (hsync_seen && !vblank) begin
        if (ce_pix && !ce_r && !hblank && wx<9'd320) begin
            waddr<=wbase+{6'b0,wx}; wdata<={r,g,b}; wen<=1; wx<=wx+9'd1;
        end
        if (hblank) begin
            wx<=0;
            if (!hbl_r) begin wl<=wl+6'd1; wbase<=(wl==6'd63) ? 15'd0 : wbase+15'd320; end
        end
    end
    // начало кадра: конец гашения кадра
    if (vbl_r && !vblank) begin frame_tog<=~frame_tog; h40_m<=h40; v30_m<=v30; lace_m<=lace; field_m<=field; end
end

// Синхронизация: конец строки 0 кадра (начало гашения строки после её точек) — машина встаёт; пуск — смена
// rel_t (строка REL развёртки), пришедшая после начала стоянки. Сброс машины (blank) — без стоянок.
reg rel_t=0;                    // меняется на строке REL развёртки (домен hclk)
reg [2:0] rel_s=0;
reg rel_seen=0, l0_done=0;
reg [15:0] stall_n=0;           // тактов MCLK стоянки (диагностика; кадр HDMI — 900 000 тактов)
always @(posedge mclk) begin
    rel_s<={rel_s[1:0],rel_t};
    if (vblank) l0_done<=0;
    if (blank) begin sync_stall<=0; l0_done<=0; end
    else if (!sync_stall) begin
        if (!l0_done && hsync_seen && !vblank && hblank && !hbl_r && wl==6'd0) begin
            sync_stall<=1; rel_seen<=rel_s[2]; l0_done<=1; stall_n<=0;
        end
    end else begin
        if (stall_n!=16'hFFFF) stall_n<=stall_n+16'd1;
        if (rel_s[2]!=rel_seen) sync_stall<=0;
    end
end

// Пять банков по 4096 слов, в каждом — три массива по 4 бита (R, G, B): по блоку 4K x 4 на массив,
// всего 15. Одним массивом 20480 x 12 Gowin брал 2K x 9 по два в ширину (22 блока), тремя по 4 бита —
// округлял глубину до 32K (24 блока).
wire [14:0] raddr;
reg [2:0] rbank=0;
wire [3:0] bq_r [0:4], bq_g [0:4], bq_b [0:4];
genvar k;
generate for (k=0;k<5;k=k+1) begin : bank
    reg [3:0] mr [0:4095];
    reg [3:0] mg [0:4095];
    reg [3:0] mb [0:4095];
    reg [3:0] qr=0, qg=0, qb=0;
    always @(posedge mclk) if (wen && waddr[14:12]==k) begin
        mr[waddr[11:0]]<=wdata[11:8]; mg[waddr[11:0]]<=wdata[7:4]; mb[waddr[11:0]]<=wdata[3:0];
    end
    always @(posedge hclk) begin qr<=mr[raddr[11:0]]; qg<=mg[raddr[11:0]]; qb<=mb[raddr[11:0]]; end
    assign bq_r[k]=qr, bq_g[k]=qg, bq_b[k]=qb;
end endgenerate
always @(posedge hclk) rbank<=raddr[14:12];
wire [11:0] ring_q={bq_r[rbank],bq_g[rbank],bq_b[rbank]};

// =====================================================================================
// Развёртка HDMI — CEA-861: 720p60 (VIC 4) и 720p50 (VIC 19), строки одной длины
// =====================================================================================
localparam [10:0] HA=11'd1280, HSYNC=11'd40;
localparam [9:0] VA=10'd720, VT=10'd750, VS0=10'd725, VS1=10'd730;
localparam [9:0] REL=10'd746;   // строка развёртки, с которой VDP пишет строку 1 кадра
reg [2:0] pal_s=0, tog_s=0, blank_s=0;
reg [2:0] size_s=0;
always @(posedge hclk) begin
    pal_s<={pal_s[1:0],pal}; tog_s<={tog_s[1:0],frame_tog}; blank_s<={blank_s[1:0],blank};
    size_s<=picture_size;
end
reg pal_h=0;                    // режим развёртки — на кадр, со строки 0 (смена NTSC/PAL — между кадрами)
wire [10:0] HT=pal_h ? 11'd1980 : 11'd1650;
wire [10:0] HS0=pal_h ? 11'd1720 : 11'd1390;          // 1280 + передний порог 440 / 110
reg [10:0] hx=0;
reg [9:0] vy=0;
reg [2:0] quiet=0;              // кадров HDMI без кадра VDP
reg seen=0;
wire last_line=vy==VT-10'd1;
wire line_end=hx==HT-11'd1;
wire event_h=tog_s[2]^tog_s[1];

// Сброс развёртки — только до первого захвата PLL HDMI (03.11.2026, плата): по LOCK ПЛИС сбрасывала развёртку
// на каждом его провале — приёмник терял кадр. Дальше счётчики идут сами, как у hdl-util hdmi (reset 0).
reg hrun=0, hres_d=0;
always @(posedge hclk) begin
    if (!hrun) begin hx<=0; vy<=0; pal_h<=0; locked<=0; quiet<=0; seen<=0; end
    else begin
        if (line_end) begin
            hx<=0;
            if (last_line) begin vy<=0; hframes<=hframes+8'd1; pal_h<=pal_s[2]; end
            else vy<=vy+10'd1;
        end else hx<=hx+11'd1;
        if (vy==REL && hx==11'd0) rel_t<=~rel_t;
        // кадры VDP идут — картинка в ногу (нет кадров три кадра HDMI подряд — нет)
        if (event_h) seen<=1;
        if (last_line && hx==11'd0) begin
            if (seen) begin locked<=1; quiet<=0; end
            else if (quiet!=3'd7) quiet<=quiet+3'd1;
            if (quiet>=3'd3) locked<=0;
            seen<=0;
        end
    end
end
always @(posedge hclk) begin
    if (!hreset) hrun<=1;
    hres_d<=hreset;
    if (hreset && !hres_d) hres_cnt<=hres_cnt+2'd1;
end
// опоздавший кадр VDP (стоянка дольше 65 535 тактов MCLK, 1,2 мс; обычная — 4 000–8 100) — в счётчик jump_cnt
reg late_t=0, late_d=0;
always @(posedge mclk) if (sync_stall && stall_n==16'd65535 && !late_d) begin late_t<=~late_t; late_d<=1; end
                       else if (!sync_stall) late_d<=0;
reg [2:0] late_s=0;
always @(posedge hclk) begin late_s<={late_s[1:0],late_t}; if (late_s[2]^late_s[1]) jump_cnt<=jump_cnt+2'd1; end
wire [8:0] trim=stall_n[15:7];  // длина последней стоянки, по 128 тактов MCLK (модель, hdmi.log)

// =====================================================================================
// Окно и масштаб
// =====================================================================================
reg h40_h=1, v30_h=0, lace_h=0, field_h=0;
always @(posedge hclk) if (event_h) begin h40_h<=h40_m; v30_h<=v30_m; lace_h<=lace_m; field_h<=field_m; end   // устоялись к событию
// размеры — на кадр, с начала кадра
reg [10:0] W=960, X0=160;
reg [9:0] H=720, Y0=0;
reg [8:0] SW=320, SH=224;
always @(posedge hclk) if (vy==0 && hx==0) begin
    SW<=h40_h ? 9'd320 : 9'd256;
    SH<=lace_h ? (v30_h ? 9'd480 : 9'd448) : (v30_h ? 9'd240 : 9'd224);   // логических строк
    case (size_s[1:0])
        2'd1: begin W<=size_s[2] ? 11'd1216 : 11'd912; X0<=size_s[2] ? 11'd32 : 11'd184; {H,Y0}<={10'd684,10'd18}; end
        2'd2: begin W<=size_s[2] ? 11'd1152 : 11'd864; X0<=size_s[2] ? 11'd64 : 11'd208; {H,Y0}<={10'd648,10'd36}; end
        default: begin W<=size_s[2] ? 11'd1280 : 11'd960; X0<=size_s[2] ? 11'd0 : 11'd160; {H,Y0}<={10'd720,10'd0}; end
    endcase
end

// по вертикали: строка источника sy для строки vy+1 — сразу после видимой части строки vy (hx 1280): при 16:9
// окно начинается с точки 0, первую точку строки кольцо читает ещё в конце предыдущей строки
reg [8:0] sy=0;
reg [10:0] vacc=0;
reg win_y=0;
wire [9:0] vy_next=last_line ? 10'd0 : vy+10'd1;
always @(posedge hclk) if (hx==HA) begin
    if (vy_next==Y0) begin sy<=0; vacc<=0; win_y<=1; end
    else if (win_y) begin
        if (vy_next==Y0+H) win_y<=0;
        if ({1'b0,vacc}+{3'b0,SH}>={2'b0,H}) begin vacc<=vacc+{2'b0,SH}-{1'b0,H}; sy<=sy+9'd1; end
        else vacc<=vacc+{2'b0,SH};
    end
end
// строка кольца: без чересстрочки — sy; с ней — строка своего поля (sy − поле)/2, выше первой — первая
wire [8:0] sy_f=sy-{8'b0,field_h};
wire [5:0] rl=!lace_h ? sy[5:0] : (sy<{8'b0,field_h}) ? 6'd0 : sy_f[6:1];
wire [14:0] rbase={1'b0,rl,8'b0}+{3'b0,rl,6'b0};      // rl*320

// по горизонтали: sx — точка источника для пикселя hx+1 (данные кольца приходят тактом позже); при X0 = 0
// счёт начинается за две точки до конца предыдущей строки
reg [8:0] sx=0;
reg [10:0] hacc=0;
wire [10:0] sx_start=X0==11'd0 ? HT-11'd2 : X0-11'd2;
always @(posedge hclk) begin
    if (hx==sx_start) begin sx<=0; hacc<=0; end
    else if (hacc+{2'b0,SW}>=W) begin hacc<=hacc+{2'b0,SW}-W; sx<=sx+9'd1; end
    else hacc<=hacc+{2'b0,SW};
end
assign raddr=rbase+{6'b0,sx};

// меню: сигналы — к пикселю hx (nes_osd считает на 3 точки вперёд)
wire osd_box, osd_hl, osd_band_px, osd_text; wire [23:0] osd_rgb;
nes_osd osd(.clk(clk_osd), .wr(osd_wr), .wr_addr(osd_addr), .wr_data(osd_data),
    .show(osd_show), .sel(osd_sel), .band_rows(osd_band), .rule_rows(osd_rule), .bar_cfg(osd_bar),
    .clk_pixel(hclk), .cx(hx), .cy(vy),
    .box(osd_box), .hl(osd_hl), .band(osd_band_px), .text(osd_text), .text_rgb(osd_rgb));

// =====================================================================================
// Пиксель и HDMI
// =====================================================================================
wire in_x=hx>=X0 && hx<X0+W;
wire show_pic=locked && !blank_s[2] && win_y && in_x;
wire [23:0] pic=show_pic ? {ring_q[11:8],ring_q[11:8],ring_q[7:4],ring_q[7:4],ring_q[3:0],ring_q[3:0]} : 24'h0;
reg [23:0] rgb=0;
reg de=0, hs=0, vs=0;
always @(posedge hclk) begin
    de<=hx<HA && vy<VA;
    hs<=hx>=HS0 && hx<HS0+HSYNC;
    // vsync меняется вместе с передним фронтом hsync (CEA-861, как hdl-util hdmi), а не в начале строки:
    // с фронтом в начале строки карта захвата и телевизор на плате кадр не брали (03.11.2026, проверяется)
    vs<=vy==VS0-10'd1 ? hx>=HS0 : vy==VS1-10'd1 ? hx<HS0 : vy>=VS0 && vy<VS1-10'd1;
    rgb<=osd_text ? osd_rgb : osd_hl ? 24'h2C4C9C : osd_band_px ? 24'h1E3266 :
         osd_box ? {3'b0,pic[23:19],3'b0,pic[15:11],3'b0,pic[7:3]}+24'h080C28 : pic;
end

// HDMI, а не голый DVI (03.11.2026, плата): с DVI картинка с меню мигала и пропадала, серые полосы — нет;
// все ядра платы (TS-Config, NES, Amiga) выдают HDMI. hdmi_tx (из TS-Config): преамбула и защитная полоса
// перед строкой, раз в кадр — AVI InfoFrame (RGB полного диапазона, игра, без обрезки краёв) с VIC 4 (720p60)
// или 19 (720p50). Видео и синхроимпульсы задержаны вместе на 11 тактов.
wire [9:0] tmds0, tmds1, tmds2;
hdmi_tx #(.HDMI(1'b1), .VS_HIGH(1'b1), .VIC(8'd4), .VIC_ALT(8'd19), .PIC_AR(2'b10)) tx(
    .pixel_clk(hclk), .reset_n(hrun), .vic_alt(pal_h), .r(rgb[23:16]), .g(rgb[15:8]), .b(rgb[7:0]), .de(de), .hs(hs), .vs(vs),
    .tmds_blue(tmds0), .tmds_green(tmds1), .tmds_red(tmds2));

// RESET у OSER10 — 0, как у NES: сброс из домена hclk в hclk5 не укладывался во время (восстановление)
wire [2:0] tmds_ser;
OSER10 oser0(.Q(tmds_ser[0]), .D0(tmds0[0]), .D1(tmds0[1]), .D2(tmds0[2]), .D3(tmds0[3]), .D4(tmds0[4]),
    .D5(tmds0[5]), .D6(tmds0[6]), .D7(tmds0[7]), .D8(tmds0[8]), .D9(tmds0[9]), .PCLK(hclk), .FCLK(hclk5), .RESET(1'b0));
OSER10 oser1(.Q(tmds_ser[1]), .D0(tmds1[0]), .D1(tmds1[1]), .D2(tmds1[2]), .D3(tmds1[3]), .D4(tmds1[4]),
    .D5(tmds1[5]), .D6(tmds1[6]), .D7(tmds1[7]), .D8(tmds1[8]), .D9(tmds1[9]), .PCLK(hclk), .FCLK(hclk5), .RESET(1'b0));
OSER10 oser2(.Q(tmds_ser[2]), .D0(tmds2[0]), .D1(tmds2[1]), .D2(tmds2[2]), .D3(tmds2[3]), .D4(tmds2[4]),
    .D5(tmds2[5]), .D6(tmds2[6]), .D7(tmds2[7]), .D8(tmds2[8]), .D9(tmds2[9]), .PCLK(hclk), .FCLK(hclk5), .RESET(1'b0));
ELVDS_OBUF tmds_buf [3:0] (.I({hclk,tmds_ser}), .O({tmds_clk_p,tmds_d_p}), .OB({tmds_clk_n,tmds_d_n}));

endmodule
`default_nettype wire
