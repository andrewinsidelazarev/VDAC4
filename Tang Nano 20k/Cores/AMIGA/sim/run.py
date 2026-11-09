"""Local simulator launcher. No connection to the VDAC4 board is made."""
from pathlib import Path
from http.server import ThreadingHTTPServer,BaseHTTPRequestHandler
import argparse,datetime,hashlib,io,json,os,subprocess,threading,time,webbrowser
from build import HERE,PROJECT,build,sha

DEFAULT_ROM=PROJECT/'ROM/kickstart-3.1-a500_a600_a2000.rom'
DEFAULT_ADF=PROJECT/'disks/AmigaTestKit-1.21/AmigaTestKit-1.21/AmigaTestKit.adf'
class Session:
    def __init__(self):
        self.process=None;self.output=None;self.phase='Готов к запуску';self.failure='';self.busy=False;self.lock=threading.Lock()
        self.defaults={'profile':'stable','rom':str(DEFAULT_ROM),'adf':str(DEFAULT_ADF) if DEFAULT_ADF.exists() else '', 'hdf':'','hdf1':'','fast_rom':True}
        try:
            previous=json.loads((HERE/'runs/last_session.json').read_text())
            p=Path(previous['output']).resolve()
            if p.is_relative_to((HERE/'runs').resolve()) and (p/'status.json').is_file():
                self.output=p;self.defaults=previous['defaults'];self.phase='Сохранённый сеанс'
        except (OSError,KeyError,ValueError):pass
    def start(self,config):
        with self.lock:
            if self.busy or (self.process and self.process.poll() is None):raise ValueError('Симулятор уже запущен. Сначала нажмите «Остановить».')
            if config.get('profile') not in ['stable','current']:raise ValueError('Неверный профиль')
            for name in ['rom','adf','hdf','hdf1']:
                if config.get(name) and not Path(config[name]).is_file():raise ValueError('Файл не найден: '+config[name])
            if not config.get('rom'):raise ValueError('Укажите ROM')
            self.busy=True;self.failure='';self.phase='Сборка модели из исходников';self.defaults=config
            self.output=HERE/'runs'/datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
            self.output.mkdir(parents=True,exist_ok=True)
            (HERE/'runs/last_session.json').write_text(json.dumps({'output':str(self.output),'defaults':config}))
        def work():
            try:
                from check_clocks import check
                self.phase='Проверка ФАПЧ и сериализатора (4 состояния)'
                gate=check(config['profile']);(self.output/'clocks.json').write_text(json.dumps(gate,indent=2))
                if not gate['passed']:raise RuntimeError(gate['output'])
                from check_memory import check as check_memory
                self.phase='Проверка памяти в двух симуляторах'
                memory=check_memory(config['profile']);(self.output/'memory.json').write_text(json.dumps(memory,indent=2))
                if not memory['passed']:raise RuntimeError('Не совпали фазы / данные SDRAM между Icarus и Verilator')
                if config['profile']=='current':
                    from check_sdram_startup import check as check_startup
                    self.phase='Проверка инициализации W9864G2GH'
                    startup=check_startup('current');(self.output/'startup.json').write_text(json.dumps(startup,indent=2))
                    if not startup['passed']:raise RuntimeError('Нарушена последовательность запуска SDRAM')
                self.phase='Сборка модели из исходников'
                stage=build(config['profile']);manifest=json.loads((stage/'build.json').read_text())
                manifest['media']={n:{'path':config[n],'bytes':Path(config[n]).stat().st_size,'sha256':sha(config[n])} for n in ['rom','adf','hdf','hdf1'] if config.get(n)}
                manifest['rom_route']='preinitialized SDRAM (QSPI loader bypassed)' if config.get('fast_rom',True) else 'real QSPI loader'
                (self.output/'manifest.json').write_text(json.dumps(manifest,indent=2))
                args=[str(stage/'amiga_sim.exe'),'--output',str(self.output),'--rom',config['rom'],'--interactive']
                if config.get('fast_rom',True):args.append('--fast-rom')
                for n in ['adf','hdf','hdf1']:
                    if config.get(n):args += ['--'+n,config[n]]
                self.phase='Быстрая инициализация ROM' if config.get('fast_rom',True) else 'Загрузка ROM через QSPI'
                with (self.output/'engine.log').open('w',encoding='utf-8') as log:
                    self.process=subprocess.Popen(args,cwd=stage,stdin=subprocess.PIPE,stdout=log,stderr=subprocess.STDOUT,text=True,creationflags=subprocess.CREATE_NO_WINDOW)
                    self.busy=False;code=self.process.wait()
                (self.output/'exit.json').write_text(json.dumps({'code':code}))
                self.phase='Остановлен';self.failure=(self.output/'error.txt').read_text() if (self.output/'error.txt').exists() else (f'Код завершения: {code}' if code else '')
            except Exception as e:self.failure=str(e);self.phase='Ошибка';self.busy=False
        threading.Thread(target=work,daemon=True).start()
    def command(self,line):
        if not self.process or self.process.poll() is not None:raise ValueError('Симулятор ещё не запущен')
        words=line.split();cmd=words[0] if words else ''
        counts={'run':0,'pause':0,'stop':0,'frame':0,'reset':0,'key':2,'mouse':3,'video':1,'osd':1,'trace':0,'regs':0}
        if cmd not in counts or len(words)!=counts[cmd]+1:raise ValueError('Неверная команда')
        for w in words[1:]:int(w)
        self.process.stdin.write(line+'\n');self.process.stdin.flush()
    def status(self):
        result={'application':'Amiga RTL Simulator','phase':self.phase,'error':self.failure,'building':self.busy,'alive':bool(self.process and self.process.poll() is None),'output':str(self.output or ''),'defaults':self.defaults}
        if self.output:
            try:result.update(json.loads((self.output/'status.json').read_text()))
            except (FileNotFoundError,json.JSONDecodeError):pass
        return result

def serve(port=8088,open_browser=True):
    session=Session()
    class Handler(BaseHTTPRequestHandler):
        def log_message(self,*args):pass
        def send(self,data,kind='application/json',code=200):
            if not isinstance(data,bytes):data=data.encode('utf-8')
            self.send_response(code);self.send_header('Content-Type',kind);self.send_header('Cache-Control','no-store');self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
        def do_GET(self):
            route=self.path.split('?')[0]
            if route=='/':return self.send((HERE/'index.html').read_bytes(),'text/html; charset=utf-8')
            if route=='/status':return self.send(json.dumps(session.status(),ensure_ascii=False))
            if route in ['/native.png','/hdmi.png'] and session.output:
                try:
                    from PIL import Image
                    with Image.open(session.output/(route[1:-4]+'.ppm')) as img:
                        data=io.BytesIO();img.save(data,format='PNG')
                    return self.send(data.getvalue(),'image/png')
                except (FileNotFoundError,OSError):pass
            if route in ['/cpu.csv','/boot.csv','/engine.log','/manifest.json','/wave.vcd','/registers.json'] and session.output:
                p=session.output/route[1:]
                if p.exists():return self.send(p.read_bytes(),'text/plain; charset=utf-8')
            return self.send('Not found','text/plain',404)
        def do_POST(self):
            # Only loopback clients; same-origin POST, no cross-origin control.
            if self.headers.get('Origin') not in [None,f'http://127.0.0.1:{port}',f'http://localhost:{port}']:return self.send('{}',code=403)
            try:
                length=int(self.headers.get('Content-Length','0'))
                if length>16384:raise ValueError('Request too large')
                data=json.loads(self.rfile.read(length))
                if self.path=='/start':session.start(data)
                elif self.path=='/command':session.command(data['command'])
                else:raise ValueError('Unknown endpoint')
                self.send('{}')
            except Exception as e:self.send(json.dumps({'error':str(e)},ensure_ascii=False),code=400)
    url=f'http://127.0.0.1:{port}'
    from urllib.request import urlopen
    try:existing=json.load(urlopen(url+'/status',timeout=1))
    except (OSError,ValueError):existing={}
    if existing.get('application')=='Amiga RTL Simulator':
        print('Simulator already running: '+url,flush=True)
        if open_browser:webbrowser.open(url)
        return
    class LocalServer(ThreadingHTTPServer):
        allow_reuse_address=False
        allow_reuse_port=False
        def server_bind(self):
            if os.name=='nt':
                import socket
                self.socket.setsockopt(socket.SOL_SOCKET,socket.SO_EXCLUSIVEADDRUSE,1)
            super().server_bind()
    try:server=LocalServer(('127.0.0.1',port),Handler)
    except OSError:
        if json.load(urlopen(url+'/status',timeout=2)).get('application')!='Amiga RTL Simulator':raise
        print('Simulator already running: '+url,flush=True)
        if open_browser:webbrowser.open(url)
        return
    print('Amiga RTL Simulator: '+url,flush=True)
    if open_browser:webbrowser.open(url)
    try:server.serve_forever()
    except KeyboardInterrupt:pass
    finally:
        if session.process and session.process.poll() is None:
            try:session.command('stop');session.process.wait(5)
            except Exception:session.process.terminate()
        server.server_close()

if __name__=='__main__':
    parser=argparse.ArgumentParser();sub=parser.add_subparsers(dest='action')
    gui=sub.add_parser('serve');gui.add_argument('--port',type=int,default=8088);gui.add_argument('--no-browser',action='store_true')
    cli=sub.add_parser('run');cli.add_argument('--profile',choices=['stable','current'],default='stable');cli.add_argument('--rom',default=str(DEFAULT_ROM));cli.add_argument('--adf');cli.add_argument('--hdf');cli.add_argument('--hdf1');cli.add_argument('--seconds',type=float,default=.1);cli.add_argument('--output',type=Path);cli.add_argument('--fast-rom',action='store_true')
    a=parser.parse_args()
    if a.action in [None,'serve']:serve(getattr(a,'port',8088),not getattr(a,'no_browser',False))
    else:
        from check_clocks import check
        gate=check(a.profile)
        if not gate['passed']:raise SystemExit(1)
        from check_memory import check as check_memory
        memory=check_memory(a.profile)
        if not memory['passed']:raise SystemExit(1)
        if a.profile=='current':
            from check_sdram_startup import check as check_startup
            startup=check_startup('current')
            if not startup['passed']:raise SystemExit('Нарушена последовательность запуска SDRAM')
        stage=build(a.profile);out=a.output or HERE/'runs'/datetime.datetime.now().strftime('%Y%m%d-%H%M%S');out.mkdir(parents=True,exist_ok=True)
        (out/'manifest.json').write_bytes((stage/'build.json').read_bytes());(out/'clocks.json').write_text(json.dumps(gate,indent=2));(out/'memory.json').write_text(json.dumps(memory,indent=2))
        if a.profile=='current':(out/'startup.json').write_text(json.dumps(startup,indent=2))
        args=[str(stage/'amiga_sim.exe'),'--rom',a.rom,'--seconds',str(a.seconds),'--output',str(out)]
        if a.fast_rom:args.append('--fast-rom')
        for n in ['adf','hdf','hdf1']:
            if getattr(a,n):args+=['--'+n,getattr(a,n)]
        with (out/'engine.log').open('w',encoding='utf-8') as log:
            process=subprocess.Popen(args,cwd=stage,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
            for line in process.stdout:
                print(line,end='',flush=True);log.write(line);log.flush()
            code=process.wait()
        (out/'exit.json').write_text(json.dumps({'code':code}))
        raise SystemExit(code)
