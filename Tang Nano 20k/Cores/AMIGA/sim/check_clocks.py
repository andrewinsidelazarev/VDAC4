"""Four-state Gowin clock/serializer test; do not rely on Verilator's two states."""
from pathlib import Path
import argparse,json,shutil,subprocess
from build import HERE,PROJECT,RTL_TOOLS,vendor_models,sha,environment

def check(profile='current',negative=False):
    source=HERE/'reference/0.11' if profile=='stable' else PROJECT
    stage=Path(r'E:\zx\cache\tmp\amiga-simulator')/('clocks-'+profile+('-negative' if negative else ''))
    stage.mkdir(parents=True,exist_ok=True)
    vendor=vendor_models(stage)
    clocks=(source/'fpga/amigaclks.v').read_text()
    if negative:
        clocks=clocks.replace('.RESETN(video_locked)',".RESETN(1'b1)")
    (stage/'amigaclks.v').write_text(clocks)
    shutil.copy2(source/'fpga/serializer.sv',stage/'serializer.sv')
    modern='amiga_video video(' in (source/'fpga/amiga_vdac4.sv').read_text()
    # 0.11 uses clk28 directly and does not use the CLKDIV output.
    pixel='pixel_div' if modern else 'clk28'
    # 8.11.2026 (0.13.0): точек за 100 мкс — из делителей rPLL видео (27 x FBDIV / IDIV / 5 — CLKDIV):
    # 720p (0.12.x) 7425, 576p/480p (0.13.0) 2700; ±1,5 %. 0.11 — clk28 (2850).
    if modern:
        import re
        fb=int(re.search(r'tmdsclk_inst\.FBDIV_SEL\s*=\s*(\d+)',clocks).group(1))+1
        idiv=int(re.search(r'tmdsclk_inst\.IDIV_SEL\s*=\s*(\d+)',clocks).group(1))+1
        expected=27e6*fb/idiv/5*100e-6
    else:
        expected=2850
    p_lo,p_hi=int(expected*0.985),int(expected*1.015)
    (stage/'tb.sv').write_text('''`timescale 1ns/1ps
module tb;
reg osc=0,gsr_n=0;always #18.518518 osc=~osc;
GSR GSR(.GSRI(gsr_n));
wire clk28,clk85,sd,sl,vl,fc,pixel_div;
amigaclks dut(.clk_in(osc),.clk_7m(),.clk_28m(clk28),.clk_85m(clk85),.clk_sdram(sd),.locked(sl),.vidmode(1'b1),.clk_tmds(fc),.clk_pixel(pixel_div),.video_locked(vl)'''+(",.video_keep(1'b0)" if 'video_keep' in clocks else '')+''');
wire pixel='''+pixel+''';
wire [9:0] words[2:0];assign words[0]=10'h354;assign words[1]=10'h354;assign words[2]=10'h354;
wire [2:0] pins;wire cp;
serializer ser(.clk_pixel(pixel),.clk_pixel_x5(fc),.reset(1'b0),.tmds_internal(words),.tmds(pins),.tmds_clock(cp));
integer p=0,f=0,s=0,x=0;reg measuring=0;
always @(posedge pixel)if(measuring)p=p+1;
always @(fc)if(measuring)begin f=f+1;#0.001;if(^pins===1'bx)x=x+1;end
always @(posedge clk85)if(measuring)s=s+1;
initial begin
 #100 gsr_n=1;
 #500000;
 if(sl!==1'b1 || vl!==1'b1)$fatal(1,"PLL did not lock");
 if(pixel!==1'b0 && pixel!==1'b1)$fatal(1,"Used pixel clock is X: CLKDIV reset missing");
 measuring=1;#100000;measuring=0;
 if(p<'''+str(p_lo)+''' || p>'''+str(p_hi)+''' || s<8400 || s>8700)$fatal(1,"Clock rates incorrect p=%0d s=%0d",p,s);
 if(f<p*10-20 || f>p*10+20)$fatal(1,"Serializer/pixel ratio incorrect f=%0d p=%0d",f,p);
 if(x!=0)$fatal(1,"Unknown TMDS pin samples: %0d",x);
 $display("PASS clocks: pixel=%0d fclk_edges=%0d sys=%0d X=%0d",p,f,s,x);$finish;
end
endmodule
''')
    def run(args):
        return subprocess.run(list(map(str,args)),cwd=stage,env=environment(),capture_output=True,text=True,timeout=60)
    p=run([RTL_TOOLS/'bin/iverilog.exe','-g2012','-s','tb','-o','clocks.vvp','tb.sv','amigaclks.v','serializer.sv','gowin_models.v'])
    if p.returncode:raise RuntimeError(p.stdout+p.stderr)
    p=run([RTL_TOOLS/'bin/vvp.exe','clocks.vvp'])
    result={'profile':profile,'negative_control':negative,'passed':p.returncode==0,'output':p.stdout+p.stderr,
            'clocks_sha256':sha(stage/'amigaclks.v'),'serializer_sha256':sha(stage/'serializer.sv'),'vendor':vendor}
    (stage/'result.json').write_text(json.dumps(result,indent=2))
    print(result['output']);return result

if __name__=='__main__':
    a=argparse.ArgumentParser();a.add_argument('--profile',choices=['stable','current'],default='current');a.add_argument('--negative',action='store_true');args=a.parse_args()
    r=check(args.profile,args.negative)
    raise SystemExit(0 if (not r['passed'] if args.negative else r['passed']) else 1)
