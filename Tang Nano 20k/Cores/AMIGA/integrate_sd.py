"""Add Amiga personality to the shared ESP firmware's existing SD bridge gate.

Only edits Firmware/src/tang_sd.cpp. Does not build, flash or access a board.
Writes the exact source diff and hashes into integration/ for review.
"""
from pathlib import Path
import difflib, hashlib, json

root=Path(__file__).resolve().parent
firmware=root.parents[2]/'ESP32 S3 Zero N8R8/Firmware'
path=firmware/'src/tang_sd.cpp'
old=path.read_text(encoding='utf-8')
if 'kPersonalityAmiga' in old:
    print('Amiga SD personality already present; left unchanged.')
    raise SystemExit(0)
new=old
replacements=[
    ('constexpr uint32_t kMinNes = 0x0007;',
     'constexpr uint32_t kMinNes = 0x0007;\n'
     '// Amiga 0.1+: the same fb_sdspi LINK registers, checked by SD_STAT below.\n'
     'constexpr uint32_t kPersonalityAmiga = 0x05;\n'
     'constexpr uint32_t kMinAmiga = 0x0001;'),
    ('  if (!ts && !nes) {',
     '  const bool amiga = (version >> 24) == kPersonalityAmiga && (version & 0xFFFF) >= kMinAmiga;\n'
     '  if (!ts && !nes && !amiga) {'),
    ('(TS-Config 1.30+ in slot 0 or NES 0.7+ needed)',
     '(TS-Config 1.30+, NES 0.7+ or Amiga 0.1+ needed)'),
    ('  if (nes) {','  if (nes || amiga) {'),
    ('the NES core does not answer as an SD bridge','the Tang core does not answer as an SD bridge'),
]
for a,b in replacements:
    if new.count(a)!=1: raise SystemExit('Unexpected firmware source; no edit made: '+a)
    new=new.replace(a,b)
out=root/'integration';out.mkdir(exist_ok=True)
(out/'tang_sd.patch').write_text(''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),
    fromfile='a/src/tang_sd.cpp',tofile='b/src/tang_sd.cpp')),encoding='utf-8')
sha=lambda b:hashlib.sha256(b).hexdigest()
before=sha(path.read_bytes())
path.write_text(new,encoding='utf-8',newline='\n')
(out/'tang_sd.json').write_text(json.dumps({'path':str(path),'before_sha256':before,
    'after_sha256':sha(path.read_bytes()),'uploaded':False},indent=2)+'\n')
print('Added Amiga SD bridge recognition: '+str(path))
