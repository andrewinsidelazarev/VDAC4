"""Bounded read-only diagnostics for the project's VDAC4 board."""
import argparse, json, re, socket, time
from pathlib import Path
parser=argparse.ArgumentParser()
parser.add_argument('address'); parser.add_argument('commands',nargs='*')
parser.add_argument('--output',type=Path)
parser.add_argument('--source',help='Optional local LAN IPv4 address on multi-interface Windows hosts')
parser.add_argument('--connect-timeout',type=float,default=10)
args=parser.parse_args()
address=args.address
commands=args.commands or ['ver','tang sd ls CORES','tang sd ls AMIGA']
source=(args.source,0) if args.source else None
report=[]
def emit(entry):
    report.append(entry)
    print(json.dumps(entry),flush=True)
    if args.output: args.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
allowed={'ver','tang','tang sd ls CORES','tang sd ls AMIGA','tang core list'}
assert all(c in allowed or re.fullmatch(r'tang rd [0-9a-fA-F]{1,6}',c) for c in commands)
try:
    with socket.create_connection((address,8268),timeout=args.connect_timeout,source_address=source) as sock:
        sock.settimeout(4);banner=bytearray()
        while not banner.endswith(b'\n') and len(banner)<256:
            part=sock.recv(1)
            if not part: break
            banner.extend(part)
        banner=banner.decode().strip()
        assert banner.startswith('ZIFI-TANG/1 vdac4-'),banner
    emit({'address':address,'banner':banner})
except OSError as e:
    emit({'address':address,'update_service':str(e)})
with socket.create_connection((address,23),timeout=args.connect_timeout,source_address=source) as sock:
    sock.settimeout(.3)
    # Discard historical log; request only this task's relevant diagnostics.
    end=time.monotonic()+1
    while time.monotonic()<end:
        try: sock.recv(8192)
        except socket.timeout: pass
    for command in commands:
        reg_match=re.fullmatch(r'tang rd ([0-9a-fA-F]+)',command)
        expected=(re.compile(rb'(?m)^tang '+f'{int(reg_match[1],16)&~3:02X}'.encode()+rb' = ([0-9A-Fa-f]{8})\r?$')
                  if reg_match else None)
        sock.sendall((command+'\n').encode());data=bytearray()
        end=time.monotonic()+20;last=time.monotonic()
        while time.monotonic()<end:
            try:
                chunk=sock.recv(8192)
                if not chunk: break
                data.extend(chunk);last=time.monotonic()
                if expected and expected.search(data): break
            except socket.timeout:
                if not expected and data and time.monotonic()-last>1: break
        entry={'command':command,'reply':data.decode('utf-8','replace')}
        if expected:
            match=expected.search(data)
            entry['value']=int(match[1],16) if match else None
        emit(entry)
