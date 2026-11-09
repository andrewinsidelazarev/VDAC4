"""Проверка внешних команд SDRAM по требованиям W9864G2GH."""
from pathlib import Path
import argparse,hashlib,json,re,shutil,subprocess
from build import HERE,RTL_TOOLS,environment,sha
from check_memory import check as memory_check

def check(profile='current'):
    memory=memory_check(profile)
    if not memory['passed']:raise RuntimeError('Сначала должна пройти проверка данных/фазы SDRAM')
    source=Path(memory['stage'])
    inputs={n:sha(source/n) for n in ['sdram_sim.sv','sdram_chip.sv','amigaclks.v','gowin_models.v','tb.sv']}
    inputs.update(test=sha(__file__),profile=profile,
                  iverilog=sha(RTL_TOOLS/'bin/iverilog.exe'),vvp=sha(RTL_TOOLS/'bin/vvp.exe'))
    key=hashlib.sha256(json.dumps(inputs,sort_keys=True).encode()).hexdigest()[:12]
    stage=Path(r'E:\zx\cache\tmp\amiga-simulator')/('startup-'+key);stage.mkdir(parents=True,exist_ok=True)
    if (stage/'result.json').exists():return json.loads((stage/'result.json').read_text())
    for n in ['sdram_sim.sv','sdram_chip.sv','amigaclks.v','gowin_models.v']:shutil.copy2(source/n,stage/n)
    text=(source/'tb.sv').read_text();text=text[:text.index('always @(posedge sd_clock)')]
    text=text.replace(".cs(1'b1)",".cs(probe_access)")
    text=text.replace('module tb;','module tb;\nreg probe_access=0;',1)
    text+='''
time enabled_at=0,precharge_at=0,mode_at=0,previous_refresh=0;
time first_refresh=0,last_initial_refresh=0,first_access=0;
time last_clock=0,clock_period=0;
time minimum_rfc=64'h7fffffffffffffff,maximum_gap=0;
integer initial_refreshes=0,operating_refreshes=0,bad_dqm=0;
reg all_banks=0;
always @(posedge clk85)probe_access<=ready && first_access==0;
always @(posedge cke)enabled_at=$time;
always @(posedge chip_clock)begin
 clock_period=$time-last_clock;last_clock=$time;
 if(cke && enabled_at!=0 && precharge_at==0 && dqm_pin!==4'b1111)bad_dqm=bad_dqm+1;
 if(cke && !cs_pin)begin
  case({ras_pin,cas_pin,we_pin})
   3'b010:if(precharge_at==0)begin precharge_at=$time;all_banks=addr_pin[10];end
   3'b001:begin
    if(first_refresh==0)first_refresh=$time;
    if(previous_refresh!=0 && $time-previous_refresh<minimum_rfc)minimum_rfc=$time-previous_refresh;
    if(mode_at==0)begin initial_refreshes=initial_refreshes+1;last_initial_refresh=$time;end
    else begin
     if(operating_refreshes!=0 && $time-previous_refresh>maximum_gap)maximum_gap=$time-previous_refresh;
     operating_refreshes=operating_refreshes+1;
    end
    previous_refresh=$time;
   end
   3'b000:mode_at=$time;
   3'b011:if(first_access==0)first_access=$time;
   default:;
  endcase
 end
end
initial begin #100000 g=1;#1000000000;
 $display("SPEC %0d %0d %0d %0d %0d %0d %0d %0d",enabled_at,precharge_at,mode_at,initial_refreshes,operating_refreshes,bad_dqm,minimum_rfc,maximum_gap);
 $display("INIT_GAPS %0d %0d %0d %0d",first_refresh-precharge_at,mode_at-last_initial_refresh,first_access-mode_at,clock_period);
 $display("BANKS %0d",all_banks);$finish;
end
endmodule
'''
    (stage/'tb.sv').write_text(text)
    def run(args):
        p=subprocess.run(list(map(str,args)),cwd=stage,env=environment(),capture_output=True,text=True,timeout=60)
        if p.returncode:raise RuntimeError(p.stdout+p.stderr)
        return p.stdout+p.stderr
    run([RTL_TOOLS/'bin/iverilog.exe','-g2012','-s','tb','-o','startup.vvp','tb.sv','sdram_sim.sv','sdram_chip.sv','amigaclks.v','gowin_models.v'])
    output=run([RTL_TOOLS/'bin/vvp.exe','startup.vvp']);(stage/'run.log').write_text(output)
    values=list(map(int,re.search(r'^SPEC (.+)$',output,re.M)[1].split()))
    enabled,precharge,mode,initial,operating,bad_dqm,min_rfc,max_gap=values
    precharge_gap,mode_gap,access_gap,period=map(int,re.search(r'^INIT_GAPS (.+)$',output,re.M)[1].split())
    checks={'pause_200us':precharge-enabled>=200000000,'precharge_all_banks':'BANKS 1' in output,
            'eight_initial_refreshes':initial==8,'DQM_high_during_pause':bad_dqm==0,
            'tRP_15ns':precharge_gap>=15000,'last_refresh_to_MRS_60ns':mode_gap>=60000,
            'MRS_to_ACTIVE_two_clocks':access_gap>=2*period,
            'tRFC_60ns':min_rfc>=60000,'refresh_4096_within_64ms':operating>10 and max_gap*4096<=64000000000}
    result={'passed':all(checks.values()),'profile':profile,'checks':checks,'pause_ns':(precharge-enabled)/1000,
            'initial_refreshes':initial,'bad_DQM_samples':bad_dqm,'minimum_tRFC_ns':min_rfc/1000,
            'maximum_refresh_gap_ns':max_gap/1000,'stage':str(stage),'controller_sha256':sha(source/'sdram_sim.sv'),
            'initial_gaps_ns':{'PRECHARGE_to_REFRESH':precharge_gap/1000,'REFRESH_to_MRS':mode_gap/1000,'MRS_to_ACTIVE':access_gap/1000},
            'inputs':inputs,
            'specification':'Winbond W9864G2GH, Power Up and Initialization / Features',
            'specification_url':'https://datasheet.octopart.com/W9864G2GH-7-Winbond-datasheet-181530013.pdf'}
    (stage/'result.json').write_text(json.dumps(result,indent=2));return result

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--profile',choices=['stable','current'],default='current');ap.add_argument('--expect-reject',action='store_true');a=ap.parse_args()
    result=check(a.profile);print(json.dumps(result,indent=2));raise SystemExit(0 if (not result['passed'] if a.expect_reject else result['passed']) else 1)
