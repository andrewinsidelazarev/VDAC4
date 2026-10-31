"""Read debug3's frozen trace. Writes only its indexed observation register."""
from pathlib import Path
import argparse,datetime,json,re,socket,time
ap=argparse.ArgumentParser();ap.add_argument('address');ap.add_argument('--source')
ap.add_argument('--count',type=int,default=512);args=ap.parse_args()
assert 1<=args.count<=512
out=Path(__file__).resolve().parent/'diagnostics';out.mkdir(exist_ok=True)
stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-%H%M%SZ')
path=out/(stamp+'.trace.json')
report={'utc':stamp,'address':args.address,'entries':[],'complete':False}
def save():path.write_text(json.dumps(report,indent=2)+'\n')
with socket.create_connection((args.address,23),timeout=10,source_address=(args.source,0) if args.source else None) as sock:
    sock.settimeout(.25);buffer=bytearray()
    deadline=time.monotonic()+1
    while time.monotonic()<deadline:
        try:sock.recv(8192)
        except socket.timeout:pass
    def request(command,pattern):
        sock.sendall((command+'\n').encode());deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            while b'\n' in buffer:
                line,_,tail=buffer.partition(b'\n');buffer[:]=tail
                match=re.fullmatch(pattern,line.rstrip(b'\r'))
                if match:return match
            try:
                part=sock.recv(8192)
                if not part:raise RuntimeError('Console disconnected')
                buffer.extend(part)
            except socket.timeout:pass
        raise TimeoutError(command)
    def read(address):
        return int(request(f'tang rd {address:x}',f'tang {address:02X} = ([0-9A-Fa-f]{{8}})'.encode())[1],16)
    assert read(0)==0x56444334 and read(4)==0x05010001,'Not Amiga'
    report['build_tag']=f'{read(0x60):08X}'
    assert report['build_tag'] in ('414D4403','414D4404','414D4405'),'No extended trace in this build'
    report['trace_start']=read(0xf8)
    for i in range(args.count):
        m=request(f'tang wr f4 {i:x}',rb'tang F4 <- ([0-9A-Fa-f]{8}), reads ([0-9A-Fa-f]{8})')
        assert int(m[1],16)==i and int(m[2],16)==i,'Selector write/readback mismatch'
        words=[read(a) for a in [0xe0,0xe4,0xe8,0xec]]
        assert words[3]&(1<<19) and ((words[3]>>10)&511)==i,'Invalid or stale trace selection'
        assert (words[3]&1023)>=args.count,'Trace not filled'
        report['entries'].append(words)
        if i%64==63:save();print(f'{i+1}/{args.count} entries',flush=True)
    report['complete']=True;save()
print(path)
