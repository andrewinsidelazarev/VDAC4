"""Measure the actual Amiga PLL wrapper using Gowin's GW2A simulation primitives."""
from pathlib import Path
import hashlib,json,os,subprocess
root=Path(__file__).resolve().parents[1]
stage=root/'tests/pll-build';stage.mkdir(exist_ok=True)
vendor=Path(r'C:\Gowin\Gowin_V1.9.11.03_Education_x64\IDE\simlib\gw2a\prim_sim.v')
rtl=root/'fpga/amigaclks.v'
(stage/'pll_tb.sv').write_text('''`timescale 1ns/1ps
module pll_tb;
 GSR GSR(.GSRI(1'b1));
 reg clk=0;always #18.5185185 clk=~clk;
 wire c7,c28,c85,sd,locked,tmds,pixel,vlocked;
 amigaclks dut(.clk_in(clk),.clk_7m(c7),.clk_28m(c28),.clk_85m(c85),
 .clk_sdram(sd),.locked(locked),.vidmode(1'b1),.clk_tmds(tmds),
 .clk_pixel(pixel),.video_locked(vlocked));
 real rise85=0,fall85=0,rise28=0,riseSD=0,period85,period28,phaseSD,dutySD;
 integer checks=0;
 always @(posedge c85) begin period85=$realtime-rise85;rise85=$realtime;end
 always @(negedge c85) fall85=$realtime;
 always @(posedge c28) begin
   period28=$realtime-rise28;rise28=$realtime;
   #0.002;
   if($realtime>200000) begin
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
   if(checks<1000 || phaseSD<8.75 || phaseSD>8.79 || dutySD<5.83 || dutySD>5.87)
     $fatal(1,"PLL phase or duty mismatch");
   $display("PASS %0d CLKOUTD3 edges aligned to CLKOUT falling edge; SDRAM clock CLKOUTP 270 degrees",checks);
   $display("period85_ns=%0.6f period28_ns=%0.6f sdram_phase_ns=%0.6f sdram_high_ns=%0.6f video_lock=%b",period85,period28,phaseSD,dutySD,vlocked);
   $finish;
 end
endmodule
''')
tool=Path(r'E:\zx\tools\rtl\ucrt64\bin')
env=os.environ.copy();env['PATH']=str(tool)+';'+env['PATH']
def run(cmd,name):
    p=subprocess.run(list(map(str,cmd)),cwd=stage,env=env,capture_output=True,text=True,timeout=90)
    (stage/name).write_text(p.stdout+p.stderr)
    if p.returncode:raise RuntimeError(p.stdout+p.stderr)
    return p.stdout
run([tool/'iverilog.exe','-g2012','-s','pll_tb','-o','pll.vvp',stage/'pll_tb.sv',rtl,vendor],'compile.log')
result=run([tool/'vvp.exe',stage/'pll.vvp'],'run.log')
assert 'PASS ' in result
sdc=(root/'fpga/amigaVdac4.sdc').read_text(encoding='utf-8')
assert '-edges {2 5 8} [get_pins {amigaclks/sysclk_inst/CLKOUTD3}]' in sdc
report={'passed':True,'hardware':False,'vendor_model':str(vendor),
 'vendor_model_sha256':hashlib.sha256(vendor.read_bytes()).hexdigest(),'result':result,
 'source_sha256':{p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in ['fpga/amigaclks.v','fpga/amigaVdac4.sdc','tests/run_pll.py']},
 'clock28_phase_ns':500/85.5,'scope':'Vendor behavioral PLL and SDC correspondence; no routed clock skew or analog jitter measurement'}
(root/'tests/pll-result.json').write_text(json.dumps(report,indent=2)+'\n')
print(result)
