"""Small original 68000 test program: RAM readback followed by colour output.

No Kickstart code or disk image is required. This is a hardware smoke test,
not an Amiga OS replacement. Run with the ordinary reset sequence intact.
"""
from pathlib import Path
import struct
def make(path):
    words=[]
    def emit(*w):words.extend(w)
    def move_w(value,address):emit(0x33fc,value,address>>16,address&65535)
    def move_b(value,address):emit(0x13fc,value,address>>16,address&65535)
    emit(0x0020,0x0000,0x00f8,0x0008) # initial SSP and PC
    emit(0x46fc,0x2700)              # MOVE #$2700,SR
    move_w(0x7fff,0xdff09a)         # disable interrupts
    move_w(0x7fff,0xdff096)         # disable DMA
    move_b(3,0xbfe201)              # CIA-A: LED/overlay pins are outputs
    move_b(2,0xbfe001)              # remove ROM overlay from chip RAM
    emit(0x203c,0x1234,0x5678)      # MOVE.L #$12345678,D0
    emit(0x23c0,0,0x0100)           # MOVE.L D0,$100
    emit(0x2239,0,0x0100)           # MOVE.L $100,D1
    emit(0xb280)                   # CMP.L D0,D1
    branch=len(words);emit(0x6600,0) # BNE.W fail
    loop=len(words)
    for colour in [0xf00,0x0f0,0x00f,0xfff]:
        move_w(colour,0xdff180)     # COLOR00
        emit(*([0x4e71]*24))        # NOP: visible bands, no timer dependency
    emit(0x6000,((loop-len(words))*2-2)&65535) # BRA.W loop (relative extension address)
    fail=len(words);move_w(0xf0f,0xdff180);emit(0x60fe) # magenta on RAM error
    words[branch+1]=((fail-branch)*2-2)&65535
    data=struct.pack('>'+str(len(words))+'H',*words)
    Path(path).parent.mkdir(parents=True,exist_ok=True)
    Path(path).write_bytes(data+b'\xff'*(524288-len(data)))
    return Path(path)
if __name__=='__main__':
    print(make(Path(__file__).parent/'fixtures/diagnostic.rom'))
