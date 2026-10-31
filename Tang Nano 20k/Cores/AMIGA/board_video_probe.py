"""Temporarily substitute a solid RGB color; keep the guest running, then restore.

Only writes byte register 0x1a0 after verifying Amiga and the VPR1 capability.
No reset, SD/cache upload, firmware flash, or CPU-state writes are performed.
"""
from pathlib import Path
import argparse, datetime, json, re, socket, time

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('address')
ap.add_argument('--source')
ap.add_argument('--color', default='202020', help='Six hexadecimal RGB digits')
ap.add_argument('--seconds', type=int, default=20)
ap.add_argument('--off', action='store_true', help='Restore the normal image immediately')
args = ap.parse_args()
if not re.fullmatch('[0-9a-fA-F]{6}', args.color) or not 1 <= args.seconds <= 45:
    ap.error('Use six RGB hex digits and a duration from 1 to 45 seconds')
out = Path(__file__).resolve().parent / 'diagnostics/video-probe'
out.mkdir(parents=True, exist_ok=True)
stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-%H%M%SZ')
report = {'utc': stamp, 'address': args.address, 'color': args.color,
          'seconds': args.seconds, 'writes': [], 'restored': False}
path = out / (stamp + '.json')

class Console:
    def __enter__(self):
        self.sock = socket.create_connection((args.address, 23), timeout=10,
                    source_address=(args.source, 0) if args.source else None)
        self.sock.settimeout(.25)
        self.buffer = bytearray()
        until = time.monotonic() + 1
        while time.monotonic() < until:
            try:
                if not self.sock.recv(8192): raise ConnectionError('Console disconnected')
            except socket.timeout: pass
        return self

    def __exit__(self, *exc): self.sock.close()

    def request(self, command, pattern):
        self.sock.sendall((command + '\n').encode())
        until = time.monotonic() + 10
        while time.monotonic() < until:
            while b'\n' in self.buffer:
                line, _, tail = self.buffer.partition(b'\n')
                self.buffer[:] = tail
                match = re.fullmatch(pattern, line.rstrip(b'\r'))
                if match: return match
            try:
                chunk = self.sock.recv(8192)
                if not chunk: raise ConnectionError('Console disconnected')
                self.buffer.extend(chunk)
            except socket.timeout: pass
        raise TimeoutError(command)

    def read(self, address):
        m = self.request(f'tang rd {address:x}',
                        f'tang {address:02X} = ([0-9A-Fa-f]{{8}})'.encode())
        return int(m[1], 16)

    def identity(self):
        if [self.read(a) for a in (0, 4, 0x1a4)] != [0x56444334, 0x05010001, 0x56505231]:
            raise RuntimeError('Requires a running Amiga with VPR1 output diagnostic')

    def write_probe(self, value):
        self.identity()  # F12 could have switched cores during the observation.
        m = self.request(f'tang wr 1a0 {value:08x}',
                         rb'tang 1A0 <- ([0-9A-Fa-f]{8}), reads ([0-9A-Fa-f]{8})')
        if int(m[1],16) != value or int(m[2],16) != value:
            raise RuntimeError('Probe write/readback mismatch (guest may be in reset)')
        report['writes'].append(f'{value:08X}')

    def state(self):
        return {'top_status': self.read(0x64), 'sync_counters': self.read(0x68),
                'cpu_steps': self.read(0x74), 'probe': self.read(0x1a0)}

needs_restore = False
try:
    with Console() as console:
        console.identity()
        report['before'] = console.state()
        if args.off:
            console.write_probe(0)
            report['restored'] = True
        else:
            if report['before']['probe'] != 0:
                raise RuntimeError('Probe already active; use --off first')
            try:
                # Mark restoration necessary before issuing the command, even
                # if the board receives it but its reply is lost.
                needs_restore = True
                console.write_probe(0xa5000000 | int(args.color,16))
                print(f'Solid RGB {args.color} for {args.seconds} s; guest continues running.', flush=True)
                time.sleep(args.seconds)
                report['during'] = console.state()
            finally:
                console.write_probe(0)
                needs_restore = False
                report['restored'] = True
        report['after'] = console.state()
        print('Normal image restored; ' + str(path), flush=True)
except BaseException as exc:
    report['error'] = str(exc)
    if needs_restore:
        try:
            with Console() as retry:
                retry.identity()
                retry.write_probe(0)
                report['restored'] = True
        except Exception as restore_error:
            report['restore_error'] = str(restore_error)
            print('Restore not confirmed. Use --off after reconnection, or reset Amiga.', flush=True)
    raise
finally:
    path.write_text(json.dumps(report, indent=2) + '\n')
