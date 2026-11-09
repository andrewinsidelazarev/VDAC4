// SPDX-License-Identifier: GPL-3.0-or-later
// AGA RGB888 to standard 576p50 / 480p60 (0.13.0; up to 0.12.18 — 720p50/60). Amiga keeps its own
// raster and clocks.
module amiga_video(
    input wire clk28,clk85,hclk,hclk5,
    input wire reset28,reset85,hreset,
    input wire hs,vs,lace,field,window_in,
    input wire [23:0] rgb_in,
    input wire [3:0] picture,
    input wire osd_wr,input wire [8:0] osd_addr,input wire [31:0] osd_data,
    input wire osd_show,input wire [5:0] osd_sel,
    input wire [29:0] osd_band,osd_rule,
    output wire ram_cs,ram_we,ram_double,output wire [20:0] ram_address,
    output wire [127:0] ram_din,input wire [127:0] ram_dout,input wire ram_ack,ram_read_valid,ram_single,input wire [1:0] ram_fetch_index,
    output wire [31:0] diagnostic,
    output wire steal, // clk85: видео отстаёт, CPU придержать в chip RAM
    output wire [5:0] raster_debug,
    output wire tmds_clock,output wire [2:0] tmds
);
    wire [1:0] capture_ready,consumed;
    wire [16:0] capture_address0,capture_address1;
    wire [6:0] capture_first0,capture_last0,capture_first1,capture_last1;
    wire [9:0] capture_read_address;
    wire [31:0] capture_rgb;
    wire capture_frame;
    wire [43:0] capture_info;
    wire [7:0] overruns;
    wire [3:0] black_frames;
    wire pal;
    amiga_video_capture capture(.clk(clk28),.reset(reset28),.hs(hs),.vs(vs),
        .window_in(window_in),.lace(lace),.field(field),.rgb(rgb_in),
        .read_clk(clk85),.read_address(capture_read_address),.read_rgb(capture_rgb),
        .consumed(consumed),.ready_toggle(capture_ready),
        .address0(capture_address0),.address1(capture_address1),
        .first0(capture_first0),.last0(capture_last0),.first1(capture_first1),.last1(capture_last1),
        .frame_toggle(capture_frame),.frame_info(capture_info),.overruns(overruns),.black_frames(black_frames),.pal(pal));
    wire request_toggle,request_done,cache_we;
    wire [22:0] request_info;
    wire [10:0] cache_address; // 8.11.2026: физический адрес кэша (слот*276 + слово)
    wire [31:0] cache_data;
    wire dma_idle;
    amiga_video_dma dma(.clk(clk85),.reset(reset85),.capture_ready(capture_ready),
        .capture_address0(capture_address0),.capture_address1(capture_address1),
        .capture_first0(capture_first0),.capture_last0(capture_last0),
        .capture_first1(capture_first1),.capture_last1(capture_last1),
        .capture_consumed(consumed),.capture_read_address(capture_read_address),.capture_rgb(capture_rgb),
        .request_toggle(request_toggle),.request_info(request_info),.request_done(request_done),
        .cache_we(cache_we),.cache_address(cache_address),.cache_data(cache_data),
        .ram_cs(ram_cs),.ram_we(ram_we),.ram_double(ram_double),.ram_address(ram_address),.ram_din(ram_din),
        .ram_dout(ram_dout),.ram_ack(ram_ack),.ram_read_valid(ram_read_valid),.ram_single(ram_single),.ram_fetch_index(ram_fetch_index),.idle(dma_idle),.steal(steal));
    reg [2:0] frame_s=0;
    reg frame_seen=0,published=0;
    reg [43:0] committed_info=0;
    always @(posedge clk85) begin
        frame_s<={frame_s[1:0],capture_frame};
        if(reset85)begin frame_s<=0;frame_seen<=0;published<=0;committed_info<=0;end
        else if(frame_s[2]!=frame_seen && dma_idle)begin
            committed_info<=capture_info;published<=~published;frame_seen<=frame_s[2];
        end
    end
    reg [2:0] info_s=0,pal_s=0;
    reg info_seen=0,have_frame=0;
    reg [43:0] latest_info=0;
    // 7 ноября 2026: горизонталь окна DIW (x и ширина) меняет кроп, только если совпала
    // в двух кадрах подряд. В Workbench окно изредка сдвигалось на один кадр на
    // 2 пикселя, и вся картинка прыгала по горизонтали.
    reg [19:0] seen_geom=0,stable_geom=0;
    reg [10:0] hx=0;
    reg [9:0] vy=0;
    reg pal_h=1,lace_h=0,bank_h=0,frame_valid=0;
    reg [1:0] pal_switches=0; // 8.11.2026: смены развёртки 50/60 Гц (до 3)
    // 8 ноября 2026 (0.13.0): 576p50 (CEA-861 VIC 17/18): 864 x 625, активно 720 x 576, строка — 12 тактов
    // до HSYNC, HSYNC 64, 68 после; кадр — 5 строк до VSYNC, VSYNC 5, 39 после. 480p60 (VIC 2/3): 858 x 525,
    // 720 x 480, 16 / 62 / 60; 9 / 6 / 30. Точка 27 МГц. Под нагрузкой в Workbench сигнал 720p
    // (742,5 Мбит/с на линию) пропадал при исправной развёртке в ядре; 576p — 270 Мбит/с.
    wire [10:0] ht=pal_h ? 11'd864:11'd858;
    wire [10:0] hs0=pal_h ? 11'd732:11'd736;
    wire [10:0] hs1=pal_h ? 11'd796:11'd798;
    wire [9:0] v_active=pal_h ? 10'd576:10'd480;
    wire [9:0] vs0=pal_h ? 10'd580:10'd488;    // VSYNC — с фронта HSYNC этой строки
    wire [9:0] vs1=pal_h ? 10'd585:10'd494;    // до фронта HSYNC этой строки
    wire line_end=hx==ht-1'b1;
    wire new_frame=hx==0 && vy==v_active;
    always @(posedge hclk) begin
        info_s<={info_s[1:0],published};pal_s<={pal_s[1:0],pal};
        if(hreset)begin hx<=0;vy<=0;info_s<=0;info_seen<=0;have_frame<=0;frame_valid<=0;pal_h<=1;end
        else begin
            if(line_end)begin hx<=0;vy<=vy==(pal_h ? 10'd624:10'd524) ? 10'd0:vy+1'b1;end
            else hx<=hx+1'b1;
            if(hx==0 && vy==0)pal_h<=pal_s[2];
            if(hx==0 && vy==0 && pal_h!=pal_s[2] && pal_switches!=2'd3)pal_switches<=pal_switches+1'b1;
            if(info_s[2]!=info_seen)begin latest_info<=committed_info;have_frame<=1;info_seen<=info_s[2];
                seen_geom<={committed_info[39:30],committed_info[19:10]};
                if({committed_info[39:30],committed_info[19:10]}==seen_geom)stable_geom<=seen_geom;
            end
            if(new_frame)begin frame_valid<=have_frame;bank_h<=latest_info[41];lace_h<=latest_info[40];end
        end
    end
    wire active,advance_y;
    wire [9:0] sample_x,sample_y,sample_y_next,top,height,sx0,sy0,sw,sh;
    wire [10:0] width,left,right;wire [9:0] bottom;
    wire wide_h; // кадр 16:9 в AVI InfoFrame (бит 2 регистра картинки), с начала кадра
    amiga_video_geometry geometry(.clk(hclk),.reset(hreset),.hx(hx),.vy(vy),.total_width(ht),
        .pal(pal_h),.config_in(picture),.source_x(stable_geom[19:10]),.source_y(latest_info[29:20]),
        .source_width(stable_geom[9:0]),.source_height(latest_info[9:0]),
        .advance_y(advance_y),.active(active),.sample_x(sample_x),.sample_y(sample_y),.sample_y_next(sample_y_next),
        .width(width),.left(left),.height(height),.top(top),.right(right),.bottom(bottom),.sx0(sx0),.sy0(sy0),.sw(sw),.sh(sh),
        .wide(wide_h));
    wire [23:0] picture_rgb;
    wire pixel_valid;
    wire [15:0] misses;
    amiga_video_cache cache(.clk(hclk),.reset(hreset),.hx(hx),.total_width(ht),.vy(vy),
        .sample_x(sample_x),.sample_y(sample_y),.sample_y_next(sample_y_next),.advance_y(advance_y),
        .left(left),.width(width),.top(top),.height(height),.right(right),.bottom(bottom),.source_y(sy0),.source_x(sx0),.source_width(sw),
        .active(active),.frame_valid(frame_valid),.new_frame(new_frame),.lace(lace_h),.bank(bank_h),
        .dma_clk(clk85),.cache_we(cache_we),.cache_address(cache_address),.cache_data(cache_data),
        .request_toggle(request_toggle),.request_info(request_info),.request_done(request_done),
        .rgb(picture_rgb),.pixel_valid(pixel_valid),.misses(misses));
    // Active output pixels are two clocks behind hx. There is no DE
    // during the horizontal wrap, so vy is already the corresponding row.
    wire [10:0] hx_q=hx-11'd2;wire [9:0] vy_q=vy;
    // 0.13.0: у 576p/480p синхроимпульсы отрицательные (CEA-861): в импульсе 0, вне его 1.
    reg de_s=0,hs_s=1,vs_s=1;
    reg de=0,hs_out=1,vs_out=1;
    always @(posedge hclk)begin
        if(hreset)begin de<=0;hs_out<=1;vs_out<=1;de_s<=0;hs_s<=1;vs_s<=1;end
        else begin
        de<=de_s;hs_out<=hs_s;vs_out<=vs_s;
        de_s<=hx<720 && vy<v_active;
        hs_s<=!(hx>=hs0 && hx<hs1);
        vs_s<=!(vy==vs0 ? hx>=hs0 : vy==vs1 ? hx<hs0 : vy>vs0 && vy<vs1);
        end
    end
    wire osd_box,osd_hl,osd_band_px,osd_text;
    wire [23:0] osd_text_rgb;
    // 0.13.0: окно меню 494 x 430 (точка шрифта — 1 такт x 2 строки) по середине строки 720, сверху 40 —
    // помещается и в 480 строк 480p60.
    nes_osd #(.COLS(56),.ROWS(24),.PAD(24),.SCALE_X(1),.WX0(113),.WY0(40),.FRAME_Y(1)) osd(
        .clk(clk28),.wr(osd_wr),.wr_addr(osd_addr),.wr_data(osd_data),.show(osd_show),.sel(osd_sel),
        .band_rows(osd_band),.rule_rows(osd_rule),.clk_pixel(hclk),.cx(hx_q),.cy(vy_q),
        .box(osd_box),.hl(osd_hl),.band(osd_band_px),.text(osd_text),.text_rgb(osd_text_rgb));
    wire [23:0] p=pixel_valid ? picture_rgb:24'd0;
    wire [23:0] rgb=osd_text ? osd_text_rgb : osd_hl ? 24'h2c4c9c : osd_band_px ? 24'h1e3266 :
        osd_box ? {3'b0,p[23:19],3'b0,p[15:11],3'b0,p[7:3]}:p;
    // Register RGB before TMDS so the cache/OSD and the encoder have separate
    // pixel-clock paths. DVI transport matches the original Amiga core.
    reg [23:0] tx_rgb=0;reg tx_de=0,tx_hs=1,tx_vs=1;
    always @(posedge hclk)begin tx_rgb<=rgb;tx_de<=de;tx_hs<=hs_out;tx_vs<=vs_out;end
    // 7 ноября 2026: HDMI вместо DVI. В DVI начало активной строки приёмник угадывает по смене
    // управляющих символов на данные; на плате (запись OBS, телевизор) низ кадра изредка
    // начинался на 4 такта раньше — приёмник сбивался с начала строки до кадрового импульса,
    // данные целы. В HDMI перед строкой преамбула и защитная полоса — начало однозначно;
    // плюс AVI InfoFrame: полный RGB, без обрезки. Как в ядре Mega Drive (md_video.sv).
    // Задержка от tx_rgb до TMDS: 1 регистр + 10 тактов преамбулы + 1 кодер.
    localparam TX_DELAY=12;
    wire [9:0] t0,t1,t2;
    // 0.13.0: VIC 2/3 — 480p60 4:3/16:9, 17/18 — 576p50 4:3/16:9; VSYNC активен нулём.
    hdmi_tx #(.HDMI(1'b1),.SHORT_TMDS(1'b1),.VS_HIGH(1'b0),.VIC(8'd2),.VIC_ALT(8'd17),.PIC_AR(2'b01),
        .VIC_W(8'd3),.VIC_ALT_W(8'd18)) tx(
        // 0.13.3: флаг формата AVI всегда 16:9 (VIC 18/3) — пункт ASPECT RATIO убран (пользователь: «всё равно не
        // работает» — монитор флаг не учитывает); 16:9 и было выбрано. Бит 2 регистра картинки больше не нужен.
        .pixel_clk(hclk),.reset_n(!hreset),.vic_alt(pal_h),.wide(1'b1),.r(tx_rgb[23:16]),.g(tx_rgb[15:8]),.b(tx_rgb[7:0]),
        .de(tx_de),.hs(tx_hs),.vs(tx_vs),.tmds_blue(t0),.tmds_green(t1),.tmds_red(t2));
    wire [9:0] symbols[2:0];
    assign symbols[0]=t0,symbols[1]=t1,symbols[2]=t2;
    serializer ser(.clk_pixel(hclk),.clk_pixel_x5(hclk5),.reset(hreset),
        .tmds_internal(symbols),.tmds(tmds),.tmds_clock(tmds_clock));
    assign raster_debug=vy[5:0];
    // 8 ноября 2026: {переполнения захвата[31:24], 0, смены 50/60[21:20] (до 3), pal_h[19], lace_h[18], 0,
    // frame_valid[16], чёрные строки вывода[15:12], чёрные поля Amiga[11:8], промахи кэша[7:0]}; счётчики
    // переполнений, промахов и чёрных строк — по кругу (8.11.2026). Узко: ПЛИС заполнена.
    assign diagnostic={overruns,2'b0,pal_switches,pal_h,lace_h,1'b0,frame_valid,misses[11:8],black_frames,misses[7:0]};
endmodule
