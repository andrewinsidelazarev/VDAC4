"""Archive and check Gowin's actual routed report, not its process exit code."""
from pathlib import Path
import json, re, shutil

def fpga_report(stage: Path, out: Path):
    pnr=stage/'impl/pnr'
    for name in ('amigaVdac4.tr','amigaVdac4.rpt.txt','amigaVdac4.pin.html'):
        shutil.copyfile(pnr/name,out/name)
    timing=(pnr/'amigaVdac4.tr').read_text()
    result={}
    for kind in ('Setup','Hold'):
        m=re.search(r'<Numbers of '+kind+r' Violated Endpoints>:(\d+)',timing)
        if not m: raise ValueError('Missing '+kind+' timing summary')
        result[kind.lower()+'_violated_endpoints']=int(m[1])
    result['worst_reported_slack_ns']={}
    for i,kind in enumerate(('Setup','Hold','Recovery','Removal'),1):
        section=timing.split(f'3.1.{i} {kind} Paths Table')[-1].split('\n3.',1)[0]
        values=[float(x) for x in re.findall(r'^\s+\d+\s+(-?\d+\.\d+)\s',section,re.M)]
        if not values: raise ValueError('Missing '+kind+' slack table')
        result['worst_reported_slack_ns'][kind.lower()]=min(values)
    pulse=timing.split('3.2 Minimum Pulse Width Table')[-1].split('\n3.',1)[0]
    values=[float(x) for x in re.findall(r'^\s+\d+\s+(-?\d+\.\d+)\s',pulse,re.M)]
    if not values: raise ValueError('Missing minimum pulse width table')
    result['worst_reported_slack_ns']['pulse_width']=min(values)
    sdc=(stage/'fpga/amigaVdac4.sdc').read_text(encoding='utf-8')
    if '-edges {2 5 8}' in sdc:
        clocks={m[1]:{'period_ns':float(m[2]),'rise_ns':float(m[3]),'fall_ns':float(m[4])}
                for m in re.finditer(r'^\s+\d+\s+(clk85|clk28)\s+Generated\s+([\d.]+)\s+[\d.]+MHz\s+([\d.]+)\s+([\d.]+)',timing,re.M)}
        if set(clocks)!={'clk85','clk28'}:raise ValueError('Missing system PLL clocks in routed summary')
        phase=clocks['clk85']['period_ns'] if 'source [get_pins {amigaclks/sysclk_inst/CLKOUTP}] -edges {2 5 8}' in sdc else clocks['clk85']['period_ns']/2
        if abs(clocks['clk28']['rise_ns']-phase)>0.005:
            raise ValueError('Routed clk28 does not model the configured CLKOUTD3 source phase')
        result['system_clocks']=clocks
    resource=(pnr/'amigaVdac4.rpt.txt').read_text()
    ioff=re.search(r'I/O Register as FF\s*\|\s*(\d+)/',resource)
    if ioff:result['io_registers']=int(ioff[1])
    controller=stage/'fpga/sdram.sv'
    if controller.is_file() and 'dq_out <= addr_0' in controller.read_text(encoding='utf-8'):
        # This wrapper has 20 command/address I/O FFs plus 32 IREG + 32 OREG.
        if not ioff or int(ioff[1])<84:raise ValueError('SDRAM input/output registers did not both pack into I/O')
    result['passed']=(not result['setup_violated_endpoints'] and not result['hold_violated_endpoints']
                      and min(result['worst_reported_slack_ns'].values())>=0)
    result['scope']='Routed STA under amigaVdac4.sdc; asynchronous LINK/CP excluded; no board or SDRAM signal-integrity validation.'
    (out/'timing.json').write_text(json.dumps(result,indent=2)+'\n')
    return result
