"""Exercise queued host commands against the real simulator process."""
import json,subprocess,time
from build import HERE,build
stage=build('current');out=HERE/'runs'/('controls-'+str(time.time_ns()));out.mkdir(parents=True)
log=(out/'engine.log').open('w')
p=subprocess.Popen([str(stage/'amiga_sim.exe'),'--rom',str(HERE/'fixtures/diagnostic.rom'),'--fast-rom','--interactive','--output',str(out)],cwd=stage,stdin=subprocess.PIPE,stdout=log,stderr=subprocess.STDOUT,text=True)
def status():
    try:return json.loads((out/'status.json').read_text())
    except (OSError,json.JSONDecodeError):return {}
def until(predicate):
    deadline=time.monotonic()+20
    while time.monotonic()<deadline:
        if predicate():return
        if p.poll() is not None:raise RuntimeError((out/'engine.log').read_text())
        time.sleep(.1)
    raise TimeoutError('Host control test timed out')
try:
    until(lambda:status().get('state')=='running')
    p.stdin.write('regs\ntrace\nrun\n');p.stdin.flush()
    until(lambda:(out/'registers.json').exists() and (out/'wave.vcd').exists())
    until(lambda:status().get('sim_seconds',0)>.001)
    p.stdin.write('pause\nregs\n');p.stdin.flush();until(lambda:status().get('paused'))
    p.stdin.write('stop\n');p.stdin.flush();assert p.wait(10)==0
    regs=json.loads((out/'registers.json').read_text());assert regs['0x0']==0x56444334
    timestamps=[int(l[1:]) for l in (out/'wave.vcd').read_text().splitlines() if l.startswith('#')]
    duration=timestamps[-1]-timestamps[0]
    assert 9990000<=duration<=10010000 # first dump is at the next RTL event
    result={'passed':True,'batched_commands':True,'pause':True,'stop_without_extra_input':True,'VCD_window_ps':duration,'LINK_ID':hex(regs['0x0'])}
    (out/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2));print(out)
finally:
    if p.poll() is None:p.terminate();p.wait(5)
    log.close()
