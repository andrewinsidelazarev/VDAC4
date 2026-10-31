"""Exercise the real diagnostic client against a fake console; never use LAN."""
from pathlib import Path
from unittest.mock import patch
import contextlib, hashlib, io, json, socket, sys, tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'board_video_probe.py').read_text()

class Board:
    def __init__(self, case):
        self.case = case
        self.amiga = case != 'wrong-core'
        self.value = 0
        self.writes = []
        self.lost_reply = False
        self.clock = 0

    def now(self):
        self.clock += .125
        return self.clock

    def sleep(self, seconds):
        assert seconds == 20
        if self.case == 'f12-during-probe': self.amiga = False

    def connect(self, *args, **kwargs): return Connection(self)

class Connection:
    def __init__(self, board):
        self.board = board
        self.buffer = b''
        self.broken = False

    def settimeout(self, value): pass
    def close(self): pass

    def sendall(self, data):
        if self.broken: raise ConnectionError('Simulated disconnect')
        parts = data.decode().strip().split()
        assert parts[0] == 'tang'
        address = int(parts[2], 16)
        b = self.board
        if parts[1] == 'rd':
            registers = {0:0x56444334, 4:0x05010001 if b.amiga else 0x01010120,
                         0x1a4:0x56505231 if b.case != 'wrong-capability' else 0,
                         0x1a0:b.value, 0x64:0x1c, 0x68:100, 0x74:200}
            value = registers[address]
            self.buffer += f'tang {address:02X} = {value:08X}\r\n'.encode()
        else:
            assert parts[1] == 'wr' and address == 0x1a0 and b.amiga
            b.value = int(parts[3], 16)
            b.writes.append(b.value)
            if b.case == 'lost-write-reply' and not b.lost_reply:
                b.lost_reply = self.broken = True
            else:
                self.buffer += f'tang 1A0 <- {b.value:08X}, reads {b.value:08X}\r\n'.encode()

    def recv(self, size):
        if self.broken: raise ConnectionError('Simulated lost write reply')
        if not self.buffer: raise socket.timeout()
        chunk, self.buffer = self.buffer[:size], self.buffer[size:]
        return chunk

results = []
for case in ['normal', 'off', 'wrong-core', 'wrong-capability', 'lost-write-reply', 'f12-during-probe']:
    b = Board(case)
    if case == 'off': b.value = 0xa5abcdef
    argv = ['board_video_probe.py', 'fake-board'] + (['--off'] if case == 'off' else [])
    with tempfile.TemporaryDirectory(prefix='amiga-probe-test-', dir=root/'tests') as temp:
        assert Path(temp).resolve().is_relative_to((root/'tests').resolve())
        fake_file = Path(temp) / 'board_video_probe.py'
        failed = False
        with patch.object(sys, 'argv', argv), patch('socket.create_connection', b.connect), \
             patch('time.monotonic', b.now), patch('time.sleep', b.sleep), contextlib.redirect_stdout(io.StringIO()):
            try:
                exec(compile(source, str(fake_file), 'exec'), {'__file__':str(fake_file), '__name__':'__main__'})
            except (RuntimeError, ConnectionError): failed = True
        report = json.loads(next((Path(temp)/'diagnostics/video-probe').glob('*.json')).read_text())
        assert failed == (case not in ['normal', 'off']), case
        if case in ['wrong-core', 'wrong-capability']:
            assert b.writes == [] and not report['restored'], case
        elif case == 'f12-during-probe':
            assert b.writes == [0xa5202020] and not report['restored'], case
        else:
            assert b.value == 0 and report['restored'], case
            assert b.writes == ([0] if case == 'off' else [0xa5202020,0]), case
        results.append(case)
report = {'passed':True, 'cases':results, 'hardware':False,
          'source_sha256':{p:hashlib.sha256((root/p).read_bytes()).hexdigest()
                           for p in ['board_video_probe.py', 'tests/test_board_video_probe.py']}}
(root/'tests/board-video-probe-result.json').write_text(json.dumps(report, indent=2)+'\n')
print('PASS diagnostic client: normal/explicit restore, identity rejection, lost reply recovery, F12 core switch protection')
