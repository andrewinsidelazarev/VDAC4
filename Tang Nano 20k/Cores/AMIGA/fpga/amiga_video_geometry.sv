// SPDX-License-Identifier: GPL-3.0-or-later
// 576p50 / 480p60 output geometry (0.13.0, 8.11.2026; before: 720p). Source coordinates are
// pixels, including interlaced logical rows. The display window fills the 720-dot line; 4:3 or
// 16:9 is told to the display by the AVI InfoFrame (wide), the dots are the same.
// All mode/window changes take effect together in vertical blanking.
module amiga_video_geometry(
    input wire clk, reset,pal,
    input wire [10:0] hx,
    input wire [9:0] vy,
    input wire [10:0] total_width,
    input wire [3:0] config_in, // size 0/95/90%, wide (16:9 in AVI), native border
    input wire [9:0] source_x, source_width,
    input wire [9:0] source_y, source_height,
    output reg active=0,
    output wire [9:0] sample_x, sample_y, sample_y_next,
    output reg advance_y=0,
    output reg [10:0] width=720, left=0,
    output reg [9:0] height=576, top=0,
    output reg [10:0] right=720, output reg [9:0] bottom=576,
    output reg [9:0] sx0=48, sy0=32, sw=640, sh=512,
    output reg wide=0
);
    // 576p50: 864 x 625, active 720 x 576; 480p60: 858 x 525, active 720 x 480.
    wire [9:0] active_lines=pal ? 10'd576:10'd480;
    wire [9:0] last_line=pal ? 10'd624:10'd524;
    reg [3:0] config_s0=0, config_s1=0;
    // 8 ноября 2026 (0.13.0): размеры 95 и 90 % — от кадра 720 x 576 (480), по центру, чётные.
    wire [10:0] w_new=config_s1[1:0]==1 ? 11'd684 : config_s1[1:0]==2 ? 11'd648 : 11'd720;
    wire [9:0] h_new=config_s1[1:0]==1 ? (pal ? 10'd548:10'd456) :
                     config_s1[1:0]==2 ? (pal ? 10'd520:10'd432) : active_lines;
    // Шаг источника на точку вывода — 0 или 1 (кэш выводит по одной точке источника). Окно DIW шире
    // или выше кадра (оверскан 736 точек, 90 %) — середина окна размером с кадр, края отрезаны.
    wire [10:0] src_w={1'b0,source_width},src_h={1'b0,source_height};
    wire [10:0] crop_x=(src_w-w_new)>>1,crop_y=(src_h-{1'b0,h_new})>>1;
    // 0.13.0: смена 50/60 (pal) меняет число строк кадра (576/480). pal меняется в начале кадра (hx 0,
    // vy 0, здесь виден с hx 1) — геометрия пересчитывается тут же, а не в гашении: иначе первый кадр
    // нового режима идёт со старым нижним краем, и кэш на new_frame начинает предвыборку не с той строки.
    reg pal_q=1;
    wire latch_now=hx==0 && vy==active_lines || hx==1 && vy==0 && pal!=pal_q;
    always @(posedge clk) begin
        config_s0<=config_in; config_s1<=config_s0;
        if(hx==1 && vy==0) pal_q<=pal;
        if(reset) begin
            width<=720;left<=0;height<=576;top<=0;wide<=0;
            sx0<=48;sy0<=32;sw<=640;sh<=512;
        end else if(latch_now) begin
            width<=w_new;left<=(11'd720-w_new)>>1;
            height<=h_new;top<=(active_lines-h_new)>>1;
            wide<=config_s1[2];
            // Bounds come from clamped capture coordinates; only reject an
            // absent/tiny DIW. No image-content guessing is involved.
            if(source_width>=160 && source_height>=100) begin
                if(src_w>w_new) begin sx0<=source_x+crop_x[9:0];sw<=w_new[9:0];end
                else begin sx0<=source_x;sw<=source_width;end
                if(src_h>{1'b0,h_new}) begin sy0<=source_y+crop_y[9:0];sh<=h_new;end
                else begin sy0<=source_y;sh<=source_height;end
            end else begin sx0<=48;sy0<=pal?32:40;sw<=640;sh<=pal?512:400;end
        end
    end
    reg latched=0;
    always @(posedge clk)begin latched<=latch_now; if(latched)begin right<=left+width;bottom<=top+height;end end
    wire [10:0] next_x=hx==total_width-1'b1 ? 11'd0 : hx+1'b1;
    wire [9:0] next_y=hx==total_width-1'b1 ? (vy==last_line ? 10'd0 : vy+1'b1) : vy;
    reg [9:0] xi=0, yi=0;
    reg [10:0] xa=0;
    reg [9:0] ya=0;
    wire [11:0] xs={1'b0,xa}+sw;
    wire [10:0] ys={1'b0,ya}+sh;
    reg row_tick=0;
    always @(posedge clk)if(reset)row_tick<=0;else row_tick<=hx==total_width-3;
    always @(posedge clk) begin
        advance_y<=ys>=height;
        if(reset) begin xi<=0;yi<=0;xa<=0;ya<=0;end
        else begin
            if(next_x==left) begin xi<=sx0;xa<=0;end
            else if(next_x>left && next_x<right) begin
                if(xs>=width) begin xi<=xi+1'b1;xa<=xs-width;end
                else xa<=xs;
            end
            if(row_tick) begin
                if(next_y==last_line) begin yi<=sy0;ya<=0;end
                else if(next_y>=top && next_y<bottom) begin
                    if(ys>=height) begin yi<=yi+1'b1;ya<=ys-height;end
                    else ya<=ys;
                end
            end
        end
    end
    // 0.13.0: и не ниже активных строк кадра. Смена 576p -> 480p (pal) — с начала кадра, а нижний край
    // окна (bottom 576) обновится только в его гашении: прежде строки 480..524 первого кадра 480p
    // считались активными — кэш отмечал промахи на невидимых строках (стенд: 160 промахов при старте NTSC).
    always @(posedge clk)if(reset)active<=0;else active<=next_x>=left && next_x<right && next_y>=top && next_y<bottom &&
        next_y<active_lines;
    assign sample_x=xi;
    assign sample_y=yi;
    assign sample_y_next=ys>=height ? yi+10'd1 : yi;
endmodule
