// SPDX-License-Identifier: GPL-3.0-or-later
// Two 368-pixel half-line buffers, retaining all RGB888 bits. The DMA reader
// has the following half-line interval to store a completed half in SDRAM.
// 7 ноября 2026: кадр публикуется и при переполнении. Пропущенная половина
// строки остаётся от прошлого кадра того же банка; прежде весь кадр
// отбрасывался, и под нагрузкой на экране навсегда оставался последний
// целый кадр (на плате — чёрный экран загрузки). Счётчик overruns остаётся.
module amiga_video_capture(
 input wire clk,reset,hs,vs,window_in,lace,field,
 input wire fit, // 12.11.2026 (0.13.11): не растягивать по высоте (бит 3 регистра картинки, такт тот же)
 input wire [23:0] rgb,
 input wire read_clk,input wire [9:0] read_address,output reg [31:0] read_rgb,
 input wire [1:0] consumed,
 output reg [1:0] ready_toggle=0,
 output reg [16:0] address0=0,address1=0,
 output reg [6:0] first0=0,last0=68,first1=0,last1=68, // блоки половины строки для записи
 output reg frame_toggle=0,output reg [43:0] frame_info=0,
 output reg [7:0] overruns=0,
 output reg [3:0] black_frames=0, // 8.11.2026: поля с окном DIW, где все точки окна чёрные (по кругу)
 output reg pal=1,
 // 9.11.2026 (0.13.7): захват этого кадра ушёл ниже верха окна (вывод может брать этот буфер, см. amiga_video.sv)
 output reg cap_ahead=0,
 output wire cap_bank
);
 reg [31:0] pixels[0:1023];
 reg [23:0] pixel_carry=0;
 // 8 ноября 2026: адрес чтения ({write_half,write_burst} DMA и номер слова контроллера SDRAM)
 // выставляется по спаду clk85, а BRAM читает по фронту. Прежде он шёл в порт BRAM прямо, за
 // полтакта — та же схема, что ломала bank5 в Next (STA чистый, на плате неверные слова в части
 // раскладок); здесь это неверные слова в кадровом буфере. Теперь адрес сначала ложится в триггер
 // фронта (только провода), а BRAM читает из него через целый такт. Контроллер просит каждое слово
 // на такт раньше (sdram.sv, video_fetch_index), и слово попадает на DQ в прежнем такте.
 // syn_preserve: синтез не должен перенести триггер внутрь BSRAM (режим pipeline) — тогда порт
 // адреса снова питался бы со спада.
 reg [9:0] read_address_q /* synthesis syn_preserve=1 */;
 always @(posedge read_clk) begin read_address_q<=read_address; read_rgb<=pixels[read_address_q]; end
 reg [10:0] cx=0;reg [9:0] cy=0;reg hd=1,vd=1;
 reg [1:0] used0=0,used1=0;
 reg bank=0,lace_r=0,field_r=0,skip_half=0,got=0,any_window=0,pal_seen=1,lit=0;
 // 9 ноября 2026 (0.13.7): окно DIW есть на этой строке (win_line) / было на предыдущей (win_prev). Строки пишутся
 // не только по окну прошлого кадра (in_rows), но и по окну этой строки: Nexus 7 двигает окно по вертикали каждый
 // кадр (шар: DIWSTRT $2B81 -> $4381; финальные титры), и строки, вошедшие в окно больше чем на 2 строки дальше
 // прошлого края, не записывались — вывод показывал их из прошлых кадров («уехал верхний/нижний сегмент»).
 reg win_line=0,win_prev=0;
 reg [16:0] row_addr=0;
 assign cap_bank=bank;
 reg [9:0] xmin=736,xmax=0,ymin=576,ymax=0;
 // 8 ноября 2026: в SDRAM пишется только окно DIW прошлого кадра (±1 блок, ±2 строки), а не всё
 // поле 736×288 с бордюрами: вывод всё равно показывает только окно. Workbench 640×256 — запись
 // на 23 % меньше. Полосы SDRAM видео не хватало: при работе блиттера захват пропускал половины
 // строк (куски старого изображения), чтение опаздывало. Узкое или пустое окно — пишется всё.
 // Если окно выросло, края первого кадра после этого — от прежних кадров.
 reg [9:0] wx0=0,wx1=735,wy0=0,wy1=575;
 wire [9:0] ybase=pal ? 10'd23:10'd20;
 wire [9:0] yend=pal ? 10'd311:10'd260;
 wire sample=cx>=330 && cx<1802 && cy>=ybase && cy<yend && !cx[0];
 // 12 ноября 2026 (0.13.11): STRETCH HEIGHT (бит 3 регистра картинки). ON (по умолчанию) — окно DIW, растянутое на весь
 // кадр, как до 0.13.10. OFF (fit) — по вертикали весь растр, как у оригинальной Amiga с монитором и у исходного NanoMig:
 // 288 строк PAL с 23-й после VSYNC (NTSC 240 с 20-й), каждая строка ровно дважды; картинка стоит там, куда её ставит
 // программа, без центрирования. По горизонтали — окно DIW в обоих режимах (растяжение 16:9 задумано).
 reg fit_r=0;
 wire [9:0] dh=ymax-ymin+10'd2;                                   // высота окна DIW, логические строки
 wire [9:0] field_h=pal ? 10'd576:10'd480;
 wire [9:0] x=(cx-330)>>1;
 wire [9:0] y=(cy-ybase)<<1;
 // Половина строки — точки x_lo..x_lo+367; точка p занимает байты 3p..3p+2, блок = 16 байт.
 wire [9:0] x_lo=x>=368 ? 10'd368:10'd0;
 wire in_rows=y+10'd4>=wy0 && y<=wy1+10'd4;
 wire overlap=wx0<=x_lo+10'd367 && wx1>=x_lo;
 wire [8:0] lx0=wx0>x_lo ? wx0-x_lo:9'd0;
 wire [8:0] lx1=wx1<x_lo+10'd367 ? wx1-x_lo:9'd367;
 wire [10:0] b0={2'd0,lx0}+{1'd0,lx0,1'b0};
 wire [10:0] b1={2'd0,lx1}+{1'd0,lx1,1'b0}+11'd2;
 wire [6:0] blk0=b0[10:4]==0 ? 7'd0:b0[10:4]-1'b1;
 wire [6:0] blk1=b1[10:4]>=7'd68 ? 7'd68:b1[10:4]+1'b1;
 wire half=x>=368;
 wire [8:0] local_x=half ? x-368 : x;
 wire [8:0] word_group={local_x[8:2],1'b0}+local_x[8:2];
 wire [8:0] word_index=word_group+(local_x[1:0]-1'b1);
 always @(posedge clk) begin
   used0<=consumed;used1<=used0;hd<=hs;vd<=vs;
   if(reset) begin
     cx<=0;cy<=0;hd<=1;vd<=1;ready_toggle<=0;used0<=0;used1<=0;
     frame_toggle<=0;overruns<=0;frame_info<=0;pal<=1;pal_seen<=1;bank<=0;black_frames<=0;lit<=0;
     wx0<=0;wx1<=735;wy0<=0;wy1<=575;
     got<=0;skip_half<=0;any_window<=0;
     xmin<=736;xmax<=0;ymin<=576;ymax<=0;fit_r<=0;
   end else begin
     // 7 ноября 2026: счётчики останавливаются, а не идут по кругу. При инициализации
     // graphics.library кадрового импульса нет ~0,16 с (TOD CIA-A за 46 мс не тикает,
     // как и в 0.11). Прежде cy через 1024 строки обнулялся сам, захват снова проходил
     // окно выборки, а row_addr (сброс только по VSYNC) рос дальше кадра — до копии
     // Kickstart в SDRAM (блок 98304 = 0x780000) и закрашивал её цветом экрана. Kickstart
     // падал на первом промахе кэша (симулятор: F8C500 на 1,1653 с, вместо кода 0x1111).
     // Без VSYNC cy стоит на 512 (окно выборки кончается на 311): между двумя кадровыми
     // импульсами не больше 288 строк, все внутри кадрового буфера.
     cx<=cx+1'b1;
     if(hd && !hs) begin cx<=0;if(!cy[9]) cy<=cy+1'b1;win_prev<=win_line;win_line<=0;end
     // 7 ноября 2026: в длинном поле interlace (field=0) Agnus роняет VSYNC на HSSTRT
     // строки VSSTRT+1 — вместе с HSYNC, в коротком — посреди строки VSSTRT. Прежде
     // cy обнулялся без этой строки, и строки длинного поля ложились на 2 строки кадра
     // выше: гребёнка на шрифтах и курсоре, ymin полей разный — картинка прыгала через
     // кадр. Теперь в длинном поле счёт начинается с 1, у обоих полей cy = V - VSSTRT.
     // 7 ноября 2026: 50/60 Гц меняются только после двух кадров подряд правильной длины
     // (PAL 300..330 строк, NTSC 250..270). Прежде одно поле неправильной длины (видимо,
     // запись VPOSW в interlace) переключало вывод в 60 Гц — телевизор терял сигнал.
     if(vd && !vs) begin
       cy<={9'd0,lace && !field};
       if(cy>=300 && cy<=330) begin if(pal_seen)pal<=1; pal_seen<=1; end
       else if(cy>=250 && cy<=270) begin if(!pal_seen)pal<=0; pal_seen<=0; end
       bank<=~bank;lace_r<=lace;field_r<=field;cap_ahead<=0;
       row_addr<=(lace ? (field ? 17'd138:17'd0) : (!bank ? 17'd39744:17'd0));
       // 12.11.2026 (0.13.11): рамка 0.13.10 (стандартный экран на месте DIW $2C81) снята. STRETCH HEIGHT — см. fit_r выше;
       // берётся с начала кадра.
       got<=0;any_window<=0;lit<=0;xmin<=736;xmax<=0;ymin<=576;ymax<=0;fit_r<=fit;
     end
     if(sample) begin
       got<=1;
       // 16 логических строк (8 строк Amiga, ~0,5 мс) запаса на запись половин строк в SDRAM и предвыборку вывода
       if(!lace_r && y>=wy0+10'd16) cap_ahead<=1;
       if(local_x==0) begin
         skip_half<=ready_toggle[half]!=used1[half];
         // 8.11.2026: по кругу — рост виден и после загрузки (прежде стоял на 255 с запуска)
         if(ready_toggle[half]!=used1[half]) overruns<=overruns+1'b1;
       end
       if((local_x==0 ? ready_toggle[half]==used1[half] : !skip_half))
         begin
           pixel_carry<=rgb;
           case(local_x[1:0])
             1:pixels[{half,word_index}]<={rgb[7:0],pixel_carry};
             2:pixels[{half,word_index}]<={rgb[15:0],pixel_carry[23:8]};
             3:pixels[{half,word_index}]<={rgb,pixel_carry[23:16]};
             default:;
           endcase
         end
       if(local_x==367 && !skip_half && (in_rows || win_line || win_prev || window_in) && overlap) begin
         ready_toggle[half]<=~ready_toggle[half];
         if(half) begin address1<=row_addr+17'd69;first1<=blk0;last1<=blk1;end
         else begin address0<=row_addr;first0<=blk0;last0<=blk1;end
       end
       if(x==735) row_addr<=row_addr+(lace_r ? 17'd276:17'd138);
       if(window_in) begin
         any_window<=1;win_line<=1;if(|rgb)lit<=1;
         if(x<xmin)xmin<=x;if(x>xmax)xmax<=x;
         if(!any_window)ymin<=y;ymax<=y; // y внутри кадра только растёт
       end
     end
     if(cx==0 && cy==yend && got) begin
       // 11.11.2026 (0.13.9): бит 42 — поле этого кадра (field_r): вывод в режиме «последнее поле» берёт его строки
       frame_info<={1'b0,field_r,bank,lace_r,any_window?xmin:10'd0,(any_window && !fit_r)?ymin:10'd0,
         any_window?xmax-xmin+10'd1:10'd736,(any_window && !fit_r)?dh:field_h};
       frame_toggle<=~frame_toggle;
       if(any_window && !lit) black_frames<=black_frames+1'b1; // по кругу: важен рост
       if(any_window && xmax-xmin>=10'd159 && ymax-ymin>=10'd98) begin
         // OFF: пишутся все строки растра (бордюр тоже), иначе на нём остались бы старые кадры
         wx0<=xmin;wx1<=xmax;wy0<=fit_r?10'd0:ymin;wy1<=fit_r?field_h-10'd1:ymax+10'd1;
       end else begin wx0<=0;wx1<=735;wy0<=0;wy1<=575;end
     end
   end
 end
endmodule
