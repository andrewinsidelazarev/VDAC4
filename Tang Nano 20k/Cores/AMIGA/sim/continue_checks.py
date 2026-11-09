"""Продолжить текущие проверки без ручного запуска следующего прогона.

Не прошивает плату. После уже запущенного smoke запускает ROM+ADF на том же
снимке RTL. Полученные кадры требуют просмотра; сам факт чтения ADF не PASS.
"""
from pathlib import Path
import argparse,datetime,json,subprocess,time
from build import HERE,PROJECT,sha
from run import DEFAULT_ROM,DEFAULT_ADF


def write(path,value):
    tmp=path.with_suffix('.tmp');tmp.write_text(json.dumps(value,indent=2),encoding='utf-8');tmp.replace(path)


def read(path):
    try:return json.loads(path.read_text(encoding='utf-8'))
    except (OSError,ValueError):return {}


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--after',type=Path,required=True)
    ap.add_argument('--seconds',type=float,default=6);a=ap.parse_args()
    if not 0<a.seconds<=10:raise ValueError('Допустимо до 10 с виртуального времени')
    manifest=read(a.after/'manifest.json')
    if not manifest:raise RuntimeError('Нет манифеста ожидаемого smoke')
    snapshot=manifest['source_sha256']
    def unchanged():
        return all((PROJECT/n).is_file() and sha(PROJECT/n)==h for n,h in snapshot.items()
                   if not n.startswith('tool/'))
    tag=datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    out=HERE/'runs'/('kickstart-testkit-fixed-'+tag);out.mkdir()
    status_file=out/'continuation.json'
    state={'state':'waiting_for_smoke','after':str(a.after),'output':str(out),
           'seconds':a.seconds,'hardware_access':False,'TestKit_menu_confirmed':False}
    write(HERE/'runs/continuation-latest.json',state)
    deadline=time.monotonic()+12*3600
    while not (a.after/'result.json').exists():
        if not unchanged():raise RuntimeError('Исходники изменились после сборки; нужен новый smoke')
        if (a.after/'error.txt').exists():raise RuntimeError('Smoke сообщил ошибку')
        if time.monotonic()>deadline:raise RuntimeError('Истекло время ожидания smoke')
        state['smoke_status']=read(a.after/'status.json');write(status_file,state);time.sleep(5)
    if not read(a.after/'result.json').get('passed'):raise RuntimeError('Smoke не прошёл')
    if not unchanged():raise RuntimeError('Исходники изменились после smoke')
    stage=Path(manifest['stage']);write(out/'manifest.json',manifest)
    for name in ['clocks.json','memory.json','startup.json']:
        if (a.after/name).exists():(out/name).write_bytes((a.after/name).read_bytes())
    args=[str(stage/'amiga_sim.exe'),'--rom',str(DEFAULT_ROM),'--adf',str(DEFAULT_ADF),
          '--fast-rom','--interactive','--output',str(out)]
    state.update(state='running_Kickstart_TestKit',stage=str(stage),
                 media={n:{'path':str(p),'sha256':sha(p)} for n,p in [('rom',DEFAULT_ROM),('adf',DEFAULT_ADF)]})
    write(status_file,state);write(HERE/'runs/continuation-latest.json',state)
    with (out/'engine.log').open('w',encoding='utf-8') as log:
        child=subprocess.Popen(args,cwd=stage,stdin=subprocess.PIPE,stdout=log,stderr=subprocess.STDOUT,
                               text=True,creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            while child.poll() is None:
                s=read(out/'status.json');state['status']=s;state['pid']=child.pid;write(status_file,state)
                reason='target_reached' if s.get('sim_seconds',0)>=a.seconds else ''
                if not unchanged():reason='sources_changed'
                if time.monotonic()>deadline:reason='wall_time_limit'
                if reason:
                    child.stdin.write('regs\nstop\n');child.stdin.flush();child.wait(timeout=120)
                    state['stop_reason']=reason;break
                time.sleep(5)
        finally:
            if child.poll() is None:
                child.stdin.write('stop\n');child.stdin.flush()
                try:child.wait(timeout=120)
                except subprocess.TimeoutExpired:child.terminate();child.wait(timeout=10)
        write(out/'exit.json',{'code':child.returncode})
    s=read(out/'status.json');v=read(out/'vectors.json')
    checks={'engine':child.returncode==0 and not (out/'error.txt').exists(),
            'CPU':s.get('cpu_steps',0)>100 and len(v)==4,
            'TMDS':s.get('hdmi_mismatches',1)==0 and s.get('hdmi_matches',0)>1000 and s.get('hdmi_frames',0)>0}
    result={'transport_and_CPU_passed':all(checks.values()),'checks':checks,'status':s,
            'TestKit_menu_confirmed':False,'requires_frame_and_trace_review':True,
            'stop_reason':state.get('stop_reason'),'source_snapshot_unchanged':unchanged()}
    write(out/'result.json',result);state.update(state='finished_requires_review',result=result)
    write(status_file,state);write(HERE/'runs/continuation-latest.json',state)


if __name__=='__main__':
    try:main()
    except Exception as error:
        failure={'error':str(error),'time':datetime.datetime.now().isoformat(),'state':'failed'}
        write(HERE/'runs/continuation-error.json',failure)
        state=read(HERE/'runs/continuation-latest.json');state.update(failure)
        write(HERE/'runs/continuation-latest.json',state)
        if state.get('output'):write(Path(state['output'])/'continuation.json',state)
        raise
