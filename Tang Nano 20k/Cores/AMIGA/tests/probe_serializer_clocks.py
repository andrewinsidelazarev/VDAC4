"""Measure OSER10 parallel-to-fast capture spacing with the real vendor PLL model."""
from pathlib import Path
import json,os,subprocess
root=Path(__file__).resolve().parents[1];stage=root/'tests/serializer-clock-probe';stage.mkdir(exist_ok=True)
vendor=Path(r'C:\Gowin\Gowin_V1.9.11.03_Education_x64\IDE\simlib\gw2a\prim_sim.v')
(stage/'probe.sv').write_text('''`timescale 1ns/1ps
module probe;
 reg osc=0;always #18.5185185 osc=~osc;
 reg gsr=0;GSR GSR(.GSRI(gsr));
 wire c7,c28,c85,sd,lock,fast,divpixel,vlock;
 amigaclks pll(.clk_in(osc),.clk_7m(c7),.clk_28m(c28),.clk_85m(c85),.clk_sdram(sd),
 .locked(lock),.vidmode(1'b1),.clk_tmds(fast),.clk_pixel(divpixel),.video_locked(vlock));
 reg [9:0] seq=0;always @(posedge c28)seq<=seq+10'd61;
 wire [9:0] symbols[2:0];assign symbols[0]=seq;assign symbols[1]=~seq;assign symbols[2]=seq^10'h2aa;
 wire [2:0] normal_q,inverted_q;wire normal_clock,inverted_clock;
 serializer normal(.clk_pixel(c28),.clk_pixel_x5(fast),.reset(1'b0),.tmds_internal(symbols),.tmds(normal_q),.tmds_clock(normal_clock));
 serializer inverted(.clk_pixel(~c28),.clk_pixel_x5(fast),.reset(1'b0),.tmds_internal(symbols),.tmds(inverted_q),.tmds_clock(inverted_clock));
 real last_normal=0,last_inverted=0,min_normal=100,min_inverted=100,age_normal,age_inverted;
 integer samples=0;reg measuring=0,load_normal,load_inverted;
 always @(posedge c28)last_normal=$realtime;
 always @(negedge c28)last_inverted=$realtime;
 always @(posedge fast)begin
   load_normal=normal.channel[0].ser.d_up0;load_inverted=inverted.channel[0].ser.d_up0;
   #0.002;
   if(measuring && gsr && load_normal)begin
     age_normal=$realtime-0.002-last_normal;
     if(age_normal<min_normal)min_normal=age_normal;
     samples=samples+1;
   end
   if(measuring && gsr && load_inverted)begin
     age_inverted=$realtime-0.002-last_inverted;
     if(age_inverted<min_inverted)min_inverted=age_inverted;
   end
 end
 initial begin
   #200000;if(!lock||!vlock)$fatal(1,"PLL not locked");
   for(integer phase=0;phase<5;phase=phase+1)begin
     measuring=0;gsr=0;@(posedge c28);@(posedge c28);
     #(0.5+phase*7.017544)gsr=1;
     repeat(20)@(posedge c28);
     min_normal=100;min_inverted=100;samples=0;measuring=1;
     repeat(100)@(posedge c28);
     measuring=0;
     $display("release_phase=%0d samples=%0d normal_age_ns=%0.3f inverted_age_ns=%0.3f",phase,samples,min_normal,min_inverted);
   end
   $finish;
 end
endmodule
''')
tool=Path(r'E:\zx\tools\rtl\ucrt64\bin');env=os.environ.copy();env['PATH']=str(tool)+';'+env['PATH']
logs=[]
for cmd in [[tool/'iverilog.exe','-g2012','-s','probe','-o','probe.vvp',stage/'probe.sv',root/'fpga/amigaclks.v',root/'fpga/serializer.sv',vendor],[tool/'vvp.exe',stage/'probe.vvp']]:
    p=subprocess.run(list(map(str,cmd)),cwd=stage,env=env,capture_output=True,text=True,timeout=120)
    logs.append(p.stdout+p.stderr)
    (stage/'run.log').write_text('\n'.join(logs))
    if p.returncode:raise RuntimeError(logs[-1])
print(logs[-1])
