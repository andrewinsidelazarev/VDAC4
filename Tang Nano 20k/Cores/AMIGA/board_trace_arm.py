"""Arm a later 512-step trace and optionally warm-reset an identified Amiga.

Only writes the trace start register and, with --reset, the documented guest
reset command. Does not upload firmware, touch SD, or reload the core.
"""
from pathlib import Path
import argparse
import datetime
import json
import re
import socket
import time

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('address')
ap.add_argument('--source')
ap.add_argument('--start', type=lambda s: int(s, 0), required=True)
ap.add_argument('--reset', action='store_true')
args = ap.parse_args()
if not 0 <= args.start <= 0xffffffff:
    ap.error('Start must fit 32 bits')
out = Path(__file__).resolve().parent / 'diagnostics'
out.mkdir(exist_ok=True)
stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-%H%M%SZ')
path = out / (stamp + '.trace-arm.json')
report = {'utc': stamp, 'trace_start': args.start, 'writes': []}

def save():
    path.write_text(json.dumps(report, indent=2) + '\n')

try:
    with socket.create_connection((args.address, 23), timeout=10,
            source_address=(args.source, 0) if args.source else None) as sock:
        sock.settimeout(.25)
        buffer = bytearray()
        deadline = time.monotonic() + 1
        while time.monotonic() < deadline:
            try:
                if not sock.recv(8192): raise ConnectionError('Disconnected')
            except socket.timeout: pass

        def request(command, pattern):
            sock.sendall((command + '\n').encode())
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                while b'\n' in buffer:
                    line, _, tail = buffer.partition(b'\n')
                    buffer[:] = tail
                    match = re.fullmatch(pattern, line.rstrip(b'\r'))
                    if match: return match
                try:
                    data = sock.recv(8192)
                    if not data: raise ConnectionError('Disconnected')
                    buffer.extend(data)
                except socket.timeout: pass
            raise TimeoutError(command)

        def read(address):
            return int(request(f'tang rd {address:x}',
                f'tang {address:02X} = ([0-9A-Fa-f]{{8}})'.encode())[1], 16)

        def identity():
            values = [read(a) for a in (0, 4, 0x60)]
            if values[:2] != [0x56444334, 0x05010001] or values[2] not in (0x414d4403, 0x414d4404, 0x414d4405):
                raise RuntimeError(f'Not an Amiga with trace support: {values}')
            if read(0x44) != 4:
                raise RuntimeError('Kickstart is not ready')

        def write(address, value, readback=True):
            identity()
            entry = {'address': f'{address:02X}', 'value': f'{value:08X}', 'confirmed': False}
            report['writes'].append(entry)
            save()  # Preserve a record even if the reply is lost.
            result = request(f'tang wr {address:x} {value:x}',
                f'tang {address:02X} <- ([0-9A-Fa-f]{{8}}), reads ([0-9A-Fa-f]{{8}})'.encode())
            if int(result[1], 16) != value or (readback and int(result[2], 16) != value):
                raise RuntimeError('Write/readback mismatch')
            entry.update(confirmed=True, readback=result[2].decode())
            save()

        identity()
        report['before'] = {f'{a:02X}': f'{read(a):08X}' for a in (0x64, 0x70, 0x74, 0x80, 0x84, 0xf8, 0x180, 0x184, 0x18c)}
        write(0xf8, args.start)
        if args.reset:
            write(0x44, 3, readback=False)
            time.sleep(2)
        identity()
        report['after'] = {f'{a:02X}': f'{read(a):08X}' for a in (0x64, 0x70, 0x74, 0x80, 0x84, 0xf8, 0x180, 0x184, 0x18c)}
except BaseException as exc:
    report['error'] = str(exc)
    raise
finally:
    save()
    print(json.dumps(report, indent=2))
    print(path)
