// SPDX-License-Identifier: GPL-3.0-or-later
// Four RGB888 half-lines packed in three physical RAMs. Both halves of the
// following row can be prefetched before the current row finishes.
module amiga_video_cache(
    input wire clk,reset,
    input wire [10:0] hx,total_width,
    input wire [9:0] vy,sample_x,sample_y,sample_y_next,
    input wire [10:0] left,width,right,
    input wire [9:0] top,height,bottom,source_y,source_x,source_width,
    input wire active,frame_valid,new_frame,lace,bank,advance_y,
    input wire dma_clk,cache_we,
    input wire [10:0] cache_address, // физический: слот*276 + слово (считает DMA)
    input wire [31:0] cache_data,
    output reg request_toggle=0,
    output reg [22:0] request_info=0,
    input wire request_done,
    output reg [23:0] rgb,
    output wire pixel_valid,
    output wire [15:0] misses
);
    reg [7:0] missed=0;
    // 8 ноября 2026: black_lines — строки, которые кэш сам вывел чёрными: нужной строки нет, и
    // предыдущей тоже (alt_ok). Промахи (missed) — по кругу, рост виден и после загрузки.
    reg [3:0] black_lines=0;
    assign misses={4'd0,black_lines,missed};
    wire [31:0] q[0:2];
    reg [1:0] pixel_slot;
    wire [10:0] read_physical=pixel_slot_d*11'd276+word_address;
    // 8 ноября 2026: запись в BRAM — только из регистров того же фронта, как b5w_* в Next 0.37.
    // DMA выставляет слово, адрес и разрешение по спаду clk85, а BRAM пишет по фронту: прежде до
    // порта записи было полтакта (5,8 нс), адрес шёл через сумматор слот*276+адрес, выбор блока —
    // через LUT в CE. В Next ровно такой порт (bank5) в части раскладок записывал неверные биты,
    // хотя STA Gowin нарушений не показывал. Здесь это неверные точки строки вывода и слова не той
    // строки кэша. Теперь спад -> фронт — только провода до триггеров wr_*, а от wr_* до BRAM — целый
    // такт. Запись на такт позже; DMA сообщает о готовности строки ещё на такт позже (read_tail 7).
    reg wr_we=0;
    reg [10:0] wr_address=0;
    reg [31:0] wr_data=0;
    always @(posedge dma_clk) begin wr_we<=cache_we;wr_address<=cache_address;wr_data<=cache_data; end
    // During horizontal blanking prefetch the first word. A straddling first
    // pixel can then combine that word with the word read on its active clock.
    wire [9:0] query_x=active ? sample_x:source_x;
    wire half=query_x>=368;
    wire [8:0] local_x_in=half ? query_x-368:query_x;
    // Split source-coordinate conversion from packed-word/RAM addressing.
    // Matching metadata and output raster are delayed by the same pixel.
    reg [8:0] local_x=0;
    reg [1:0] pixel_slot_d=0;
    reg active_d=0;
    reg pixel_accept=0;
    // 8 ноября 2026: строка не успела прийти из SDRAM — выводится предыдущая строка источника,
    // а не чёрная (на плате — чёрные полосы через весь экран). Она в соседнем слоте кэша:
    // строки выбираются по порядку, и следующая строка вытесняет её только после текущей.
    // Промах по-прежнему считается (misses).
    always @(posedge clk)begin
        local_x<=local_x_in;pixel_slot_d<=(!pixel_ok && alt_ok) ? alt_slot:pixel_slot;active_d<=active;
        pixel_accept<=active && frame_valid && (pixel_ok || alt_ok);
    end
    wire [8:0] triple_group={local_x[8:2],1'b0}+local_x[8:2];
    wire [8:0] first_word=triple_group+(local_x[1:0]==3 ? 2:local_x[1:0]==0 ? 0:local_x[1:0]-1'b1);
    wire [8:0] last_word=triple_group+(local_x[1:0]>=2 ? 2:local_x[1:0]);
    wire [8:0] word_address=active_d ? last_word:first_word;
    genvar n;
    generate for(n=0;n<3;n=n+1) begin:line
        reg [31:0] mem[0:511];
        reg [31:0] data=0;
        always @(posedge dma_clk) if(wr_we && wr_address[10:9]==n) mem[wr_address[8:0]]<=wr_data;
        always @(posedge clk) data<=mem[read_physical[8:0]];
        assign q[n]=data;
    end endgenerate
    reg [9:0] tag_row0=0,tag_row1=0,tag_row2=0,tag_row3=0;
    wire [9:0] tag_row[0:3];
    assign tag_row[0]=tag_row0,tag_row[1]=tag_row1,tag_row[2]=tag_row2,tag_row[3]=tag_row3;
    reg [3:0] valid=0;
    reg [2:0] done_s=0;
    reg pending=0,pending_stale=0;reg [1:0] refill=3;
    reg [10:0] fetch_id=0;
    reg in_y=0;
    always @(posedge clk)in_y<=vy>=top && vy<bottom;
    wire [9:0] logical_y=in_y ? sample_y:source_y;
    reg [9:0] row=0;
    always @(posedge clk)row<=lace ? logical_y : (bank ? 10'd288:10'd0)+(logical_y>>1);
    wire final_repeat=in_y && advance_y && (lace || sample_y[0]);
    wire [10:0] consumer_id={row,1'b0}+
        ((final_repeat && hx>=right) ? 11'd2 :
         (final_repeat && hx>=left && half) ? 11'd1:11'd0);
    reg [10:0] consumer_q=0;
    // Geometry prepares the next logical row during the final two clocks.
    // Hold the coherent current-row release state across that preparation.
    always @(posedge clk)if(hx<total_width-2)consumer_q<=consumer_id;
    wire [10:0] limit_id=consumer_q+11'd4;
    wire wanted_half=fetch_id[0];
    wire [9:0] wanted_row=fetch_id[10:1];
    wire [1:0] victim=fetch_id[1:0];
    wire pixel_ok=valid[pixel_slot] && tag_row[pixel_slot]==row;
    // Предыдущая строка источника для промаха (см. выше): соседний по чётности слот.
    wire [1:0] alt_slot={~pixel_slot[1],pixel_slot[0]};
    wire alt_ok=valid[alt_slot] && tag_row[alt_slot]==row-10'd1;
    // Select the incoming row before the final blanking RAM read. The
    // noninterlace bank offset (288 rows) is even, so only logical bit 1
    // is needed. Keeping this separate from the registered scheduler row
    // also preserves the first straddling RGB888 pixel at the left edge.
    always @* pixel_slot={lace ? logical_y[0]:logical_y[1],half};
    // A half-line has exactly 23 groups of 16 pixels. Compute the
    // clipped range in group coordinates; subtracting 368 pixels is
    // subtracting 23 groups, and the low four bits never change.
    wire [9:0] crop_end=source_x+source_width;
    wire [5:0] end_group={1'b0,crop_end[9:4]}+(|crop_end[3:0]);
    wire [4:0] first_group=wanted_half ?
        (source_x>368 ? source_x[9:4]-5'd23:5'd0) :
        (source_x<368 ? source_x[8:4]:5'd0);
    wire [5:0] last_group=wanted_half ?
        (crop_end>368 ? end_group-6'd23:6'd0) :
        (crop_end<368 ? end_group:6'd23);
    wire [5:0] groups_raw=last_group-{1'b0,first_group};
    wire [4:0] group_count=groups_raw==0 || groups_raw>23 ? 5'd1:groups_raw[4:0];
    reg [1:0] bank_q=0;
    reg [1:0] phase_q=0;
    reg active_q=0;
    reg [31:0] previous_word=0;
    reg pixel_valid_q=0;
    always @* case(phase_q)
        0:rgb=q[bank_q][23:0];
        1:rgb={q[bank_q][15:0],previous_word[31:24]};
        2:rgb={q[bank_q][7:0],previous_word[31:16]};
        3:rgb=q[bank_q][31:8];
    endcase
    assign pixel_valid=pixel_valid_q;
    always @(posedge clk) begin
        done_s<={done_s[1:0],request_done};
        // Each source step is zero or one pixel (0.13.0: geometry crops the
        // source to the 576p window width). Only a phase change needs a new carry word.
        // Blanking continuously preloads the carry for a cropped first pixel.
        if(phase_q!=local_x[1:0] || !active_d || !active_q)previous_word<=q[bank_q];
        active_q<=active_d;
        phase_q<=local_x[1:0];
        bank_q<=read_physical[10:9];pixel_valid_q<=pixel_accept;
        if(reset) begin
            valid<=0;request_toggle<=0;request_info<=0;done_s<=0;
            pending<=0;pending_stale<=0;missed<=0;black_lines<=0;pixel_valid_q<=0;refill<=3;fetch_id<=0;
        end else begin
            if(new_frame)begin valid<=0;refill<=3;if(pending)pending_stale<=1;end
            else if(refill)begin refill<=refill-1'b1;if(refill==1)fetch_id<={row,1'b0};end
            if(pending && done_s[2]==request_toggle) begin
                pending<=0;
                if(!pending_stale && !new_frame) begin
                    case(request_info[12:11])
                        0:tag_row0<=request_info[10:1];1:tag_row1<=request_info[10:1];
                        2:tag_row2<=request_info[10:1];3:tag_row3<=request_info[10:1];
                    endcase
                    valid[request_info[12:11]]<=1;fetch_id<=fetch_id+1'b1;
                end
            end else if(!pending && !new_frame && !refill && frame_valid && hx<total_width-2) begin
                if(fetch_id<consumer_q)fetch_id<=consumer_q;
                else if(fetch_id<limit_id && fetch_id<1152)begin
                    request_info<={first_group,group_count,victim,wanted_row,wanted_half};request_toggle<=~request_toggle;
                    pending<=1;pending_stale<=0;valid[victim]<=0;
                end
            end
            if(active && frame_valid && !pixel_ok && (hx==left || (half && sample_x==368))) begin
                missed<=missed+1'b1;
                if(!alt_ok) black_lines<=black_lines+1'b1;
            end
        end
    end
endmodule
