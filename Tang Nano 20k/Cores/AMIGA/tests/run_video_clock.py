"""Measure the actual Amiga PLL wrapper using Gowin's GW2A simulation primitives."""
from pathlib import Path
import hashlib,json,os,subprocess,tempfile,shutil
root=Path(__file__).resolve().parents[1]
negative=os.environ.get('AMIGA_PLL_NEGATIVE')=='1'
stage=Path(tempfile.mkdtemp(prefix='amiga-video-clock-negative-' if negative else 'amiga-video-clock-'))
vendor=Path(r'C:\Gowin\Gowin_V1.9.11.03_Education_x64\IDE\simlib\gw2a\prim_sim.v')
rtl=root/'fpga/amigaclks.v'
if negative:
    references=[root/'release/0.12.1/source/fpga/amigaclks.v',root.parent/'amiga-video-pipeline/fpga/amigaclks.v']
    rtl=next((p for p in references if p.is_file()),None)
    assert rtl is not None,'Original 0.12.1 clock source is required for the negative control'
shutil.copy2(rtl,stage/'amigaclks.v')
(stage/'pll_tb.sv').write_text('''`timescale 1ns/1ps
module pll_tb;
 GSR GSR(.GSRI(1'b1));
 reg clk=0;always #18.5185185 clk=~clk;
 wire c7,c28,c85,sd,locked,tmds,pixel,vlocked;
 amigaclks dut(.clk_in(clk),.clk_7m(c7),.clk_28m(c28),.clk_85m(c85),
 .clk_sdram(sd),.locked(locked),.vidmode(1'b1),.clk_tmds(tmds),
 .clk_pixel(pixel),.video_locked(vlocked)VIDEO_KEEP);
 real rise85=0,fall85=0,rise28=0,riseSD=0,period85,period28,phaseSD,dutySD;
 integer checks=0,pixel_checks=0,fast_edges=0,last_fast_edges=0;
 real pixel_rise=0,pixel_period=0,fast_rise=0,fast_period=0;
 always @(posedge tmds)begin fast_period=$realtime-fast_rise;fast_rise=$realtime;fast_edges=fast_edges+1;end
 always @(posedge pixel)begin
   pixel_period=$realtime-pixel_rise;pixel_rise=$realtime;
   if($realtime>200000)begin
     if(pixel_period<37.00 || pixel_period>37.08 || fast_period<7.39 || fast_period>7.43)
       $fatal(1,"video clock frequency mismatch");
     if(pixel_checks>0 && fast_edges-last_fast_edges!=5)$fatal(1,"video clock ratio mismatch");
     pixel_checks=pixel_checks+1;last_fast_edges=fast_edges;
   end
 end
 always @(posedge c85) begin period85=$realtime-rise85;rise85=$realtime;end
 always @(negedge c85) fall85=$realtime;
 always @(posedge c28) begin
   period28=$realtime-rise28;rise28=$realtime;
   #0.002;
   if($realtime>200000) begin
     if($isunknown(pixel))$fatal(1,"pixel clock is X: divider was not reset");
     if(!locked || !vlocked) $fatal(1,"PLL not locked after settling");
     if($realtime-fall85>0.01) $fatal(1,"CLKOUTD3 not aligned to CLKOUT falling edge");
     if(period85<11.68 || period85>11.71 || period28<35.06 || period28>35.12)
       $fatal(1,"PLL frequency mismatch");
     checks=checks+1;
   end
 end
 always @(posedge sd) begin riseSD=$realtime;phaseSD=$realtime-rise85;end
 always @(negedge sd) dutySD=$realtime-riseSD;
 initial begin
   #300000;
   if(pixel_checks<1000)$fatal(1,"No known pixel clock edges");
   if(checks<1000 || phaseSD<8.75 || phaseSD>8.79 || dutySD<5.83 || dutySD>5.87)
     $fatal(1,"PLL phase or duty mismatch");
   $display("PASS %0d CLKOUTD3 edges aligned to CLKOUT falling edge; SDRAM clock CLKOUTP 270 degrees",checks);
   $display("pixel_edges=%0d pixel_period_ns=%0.6f serializer_period_ns=%0.6f",pixel_checks,pixel_period,fast_period);
   $display("period85_ns=%0.6f period28_ns=%0.6f sdram_phase_ns=%0.6f sdram_high_ns=%0.6f video_lock=%b",period85,period28,phaseSD,dutySD,vlocked);
   $finish;
 end
endmodule
'''.replace('VIDEO_KEEP',",.video_keep(1'b0)" if 'video_keep' in rtl.read_text(encoding='utf-8',errors='replace') else ''))  # 8.11.2026: вход video_keep с 0.12.12
tool=Path(r'E:\zx\tools\rtl\ucrt64\bin')
env=os.environ.copy();env['PATH']=str(tool)+';'+env['PATH']
def run(cmd,name):
    p=subprocess.run(list(map(str,cmd)),cwd=stage,env=env,capture_output=True,text=True,errors='replace',timeout=90)
    (stage/name).write_text(p.stdout+p.stderr)
    if p.returncode:raise RuntimeError(p.stdout+p.stderr)
    return p.stdout
run([tool/'iverilog.exe','-g2012','-s','pll_tb','-o','pll.vvp',stage/'pll_tb.sv',stage/'amigaclks.v',vendor],'compile.log')
if negative:
    proc=subprocess.run([str(tool/'vvp.exe'),str(stage/'pll.vvp')],cwd=stage,env=env,capture_output=True,text=True,errors='replace',timeout=90)
    result=proc.stdout+proc.stderr;(stage/'run.log').write_text(result)
    assert proc.returncode!=0 and 'pixel clock is X' in result,result
    (root/'tests/video-clock-negative-result.json').write_text(json.dumps({'passed':False,'expected_failure':True,'detected_unreset_divider':True,'result':result,'rtl_sha256':hashlib.sha256(rtl.read_bytes()).hexdigest()},indent=2)+'\n')
    print(result);raise SystemExit(0)
result=run([tool/'vvp.exe',stage/'pll.vvp'],'run.log')
assert 'PASS ' in result
sdc=(root/'fpga/amigaVdac4.sdc').read_text(encoding='utf-8')
assert '-edges {2 5 8} [get_pins {amigaclks/sysclk_inst/CLKOUTD3}]' in sdc
report={'passed':True,'hardware':False,'vendor_model':str(vendor),
 'vendor_model_sha256':hashlib.sha256(vendor.read_bytes()).hexdigest(),'result':result,
 'source_sha256':{p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in ['fpga/amigaclks.v','fpga/amigaVdac4.sdc','tests/run_video_clock.py']},
 'clock28_phase_ns':500/85.5,'scope':'Vendor behavioral system PLL, 27/135MHz video clocks (576p, 0.13.0), divider initialization and SDC correspondence; no routed clock skew or analog jitter measurement'}
(root/'tests/video-clock-result.json').write_text(json.dumps(report,indent=2)+'\n')
print(result)
