module video_system_top(
 input clk28,clk85,hclk,sd_clk,reset,hs,vs,window_in,lace,field,
 input [23:0] rgb_in,input [3:0] picture,input [1:0] traffic,
 output [3:0] latency,output [9:0] t0,t1,t2,output [31:0] cpu_reads,chip_reads,frames_published,
 output [31:0] video_reads,video_writes,refreshes,chip_bad,cpu_held,
 output ready,output [10:0] hx,output [9:0] vy,
 output [23:0] rgb,output de,output [31:0] diagnostic,
 output [43:0] info,output [9:0] sw,sh,sx0,sy0,
 output [20:0] last_ram_addr,output ram_cs,ram_we,output req,done,output [12:0] req_info,
 output [31:0] probe_data,input [20:0] probe_addr);
 wire [31:0] dq;wire [10:0] addr;wire [3:0] dqm;wire [1:0] ba;
 wire cke,cs,we,ras,cas,ack,read_valid,ram_double,single;wire [1:0] fetch_index;wire [20:0] va;wire [127:0] din,dout;
 sdram #(.DATA_WIDTH(32),.RAS_WIDTH(11),.CAS_WIDTH(8),.VIDEO_PORT(1),.VIDEO_STREAM(1),.CHIP48_BURST(1),
 .RAM_CLOCK_SPEED(85500000),.SYNC_CLOCK_SPEED(7125000),.SYNC_DELAY(3)) memory(
 .clk(clk85),.reset_n(!reset),.ready(ready),.sd_data(dq),.sd_addr(addr),.sd_dqm(dqm),.sd_ba(ba),
 .sd_cke(cke),.sd_cs(cs),.sd_we(we),.sd_ras(ras),.sd_cas(cas),.sync(sync),.refresh(1'b0),
 .din(chip_write ? chip_a[15:0]:16'd0),.addr(bus ? chip_addr:22'h002000),.ds(2'd0),.cs(traffic!=0 && ready && chip_cs),.we(chip_write),.dout(chip_dout),.dout48(),
 .p2_cs(traffic!=0 && ready && cpu_wait==0),.p2_we(1'b0),.p2_addr(23'h201000),.p2_din(16'd0),.p2_ds(2'd0),.p2_ack(cpu_ack),.p2_dout(),.p2_dout48(),
 .video_cs(ram_cs),.video_we(ram_we),.video_double(ram_double),.video_urgent(video.steal),.video_addr(va),.video_din(din),.video_dout(dout),.video_ack(ack),.video_read_valid(read_valid),.video_single(single),.video_fetch_index(fetch_index));
 reg [31:0] cr=0,pr=0,fp=0;reg published_seen=0;
 reg [31:0] vr=0,vw=0,rf=0;
 assign cpu_reads=cr,chip_reads=pr,frames_published=fp;
 assign video_reads=vr,video_writes=vw,refreshes=rf;
 assign latency=video.TX_DELAY;
 assign t0=video.t0,t1=video.t1,t2=video.t2;
 reg [2:0] slot_count=0;
 // traffic=2/3 — шина chip RAM как у minimig: CCK = два синхроимпульса SDRAM.
 // DMA Agnus только в нечётном (hpos[0]=1), CPU только в чётном (~cck);
 // каждый — один синхроимпульс. 2: игра — DMA в 6 из 10 CCK (5 плоскостей
 // низкого разрешения, спрайты, копер), CPU с кодом в chip RAM в каждом CCK;
 // 3: Workbench — DMA в 4 из 10 CCK, CPU в 3 из 10.
 // Первый CCK каждой четвёрки с обращениями пишет слово A значением A[15:0],
 // следующие три читают его; в конце CCK данные сверяются (chip_bad).
 reg cck_half=0;reg [3:0] cck_n=0;reg [21:0] chip_a=22'h002000;
 wire bus=traffic[1];
 wire dma_cck=cck_n<(traffic==2 ? 4'd6:4'd4),cpu_cck=traffic==2 || cck_n<3;
 wire chip_write=bus && chip_a[1:0]==0;
 wire [21:0] chip_addr={chip_a[21:2],2'b00};
 // Пока видео отстаёт (steal), CPU не получает подтверждения (nrdy моста):
 // его обращение переносится на следующий CCK, чётный слот свободен.
 reg cpu_hold=0;reg [31:0] held=0;assign cpu_held=held;
 wire cpu_go=cpu_cck && !cpu_hold,chip_any=dma_cck || cpu_go;
 wire chip_cs=traffic==1 ? slot_count[2:0]==0 : bus && (cck_half ? dma_cck:cpu_go);
 wire [15:0] chip_dout;
 reg [31:0] chip_bad_r=0;assign chip_bad=chip_bad_r;
 reg [7:0] cpu_wait=0;reg cpu_seen=0;wire cpu_ack;
 always @(posedge clk85) begin
   if(ready && memory.state==memory.STATE_CAS)begin
    if(memory.sdram_port==memory.PORT_VIDEO)begin
     if(memory.ram_we)vw<=vw+1;else vr<=vr+1;
    end
    if(memory.sdram_port==memory.PORT_REFRESH)rf<=rf+1;
   end
   if(ready && memory.state==memory.STATE_CAS && memory.sdram_port==memory.PORT_1)pr<=pr+1;
   if(video.published!=published_seen)begin published_seen<=video.published;fp<=fp+1;end
   if(sync_count==11)slot_count<=slot_count+1'b1;
   if(sync_count==11 && ready)begin
    cck_half<=~cck_half;
    if(cck_half)begin
     cck_n<=cck_n==9 ? 4'd0:cck_n+1'b1;
     if(chip_any)chip_a<=chip_a+1'b1;
     cpu_hold<=bus && video.steal;if(bus && cpu_cck && cpu_hold)held<=held+1;
     if(bus && ready && chip_any && !chip_write && chip_dout!=chip_addr[15:0])begin
      chip_bad_r<=chip_bad_r+1;
      if(chip_bad_r<8)$display("chip read %h got %h expected %h, CCK %0d",chip_addr,chip_dout,chip_addr[15:0],cck_n);
     end
    end
   end
   if(cpu_ack!=cpu_seen)begin cpu_seen<=cpu_ack;cpu_wait<=24;cr<=cr+1;end
   else if(cpu_wait!=0)cpu_wait<=cpu_wait-1'b1;
 end
 reg [3:0] sync_count=0;always @(posedge clk85) sync_count<=sync_count==11 ? 0:sync_count+1'b1;
 wire sync=sync_count<3;
 sdr_chip_model chip(.clk(sd_clk),.dq(dq),.addr(addr),.dqm(dqm),.ba(ba),.cs_n(cs),.we_n(we),.ras_n(ras),.cas_n(cas));
 amiga_video video(.clk28(clk28),.clk85(clk85),.hclk(hclk),.hclk5(hclk),
 .reset28(reset||!ready),.reset85(reset||!ready),.hreset(reset),
 .hs(hs),.vs(vs),.window_in(window_in),.lace(lace),.field(field),.rgb_in(rgb_in),.picture(picture),
 .osd_wr(1'b0),.osd_addr(9'd0),.osd_data(32'd0),.osd_show(1'b0),.osd_sel(6'd63),.osd_band(30'd0),.osd_rule(30'd0),
 .ram_cs(ram_cs),.ram_we(ram_we),.ram_double(ram_double),.ram_address(va),.ram_din(din),.ram_dout(dout),.ram_ack(ack),.ram_read_valid(read_valid),.ram_single(single),.ram_fetch_index(fetch_index),
 .diagnostic(diagnostic),.tmds_clock(),.tmds());
 assign hx=video.hx_q,vy=video.vy_q,rgb=video.rgb,de=video.de,info=video.latest_info;
 assign sw=video.sw,sh=video.sh,sx0=video.sx0,sy0=video.sy0;
 assign req=video.request_toggle,done=video.request_done,req_info=video.request_info;
 assign last_ram_addr=va,probe_data=chip.mem[probe_addr];
 endmodule
 module serializer #(parameter NUM_CHANNELS=3,VIDEO_RATE=32000000)(
 input clk_pixel,clk_pixel_x5,reset,input [9:0] tmds_internal[NUM_CHANNELS-1:0],output [2:0] tmds,output tmds_clock);
 assign tmds=0,tmds_clock=clk_pixel;
 endmodule
