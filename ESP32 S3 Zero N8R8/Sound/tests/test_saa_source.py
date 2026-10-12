"""SAA.ELF on the host: synthetic E-Tracker with looping instrument/arpeggio."""
import pathlib
import struct
import subprocess
import unittest
from sound_paths import FIRMWARE, ROOT
from test_weather_parse import build_exe, short

def make_saa():
    data=bytearray(30)
    data[10:29]=b'ETracker (C) BY ESI.'
    def add(value):
        at=len(data);data.extend(value);return at
    sample=add(bytes([0xfe,0x11,0,0xaf,0xfc]))
    ornament=add(bytes([0xfe,0,7,0,0xff]))
    sam_table=add(struct.pack('<H',sample))
    orn_table=add(struct.pack('<H',ornament))
    decode=add(bytes([0xff,0]))
    positions=add(bytes([0xfe,0,0xff]))
    channels=[]
    for c in range(6):
        channels.append(add(bytes([14,0x52,0x30,0x72+36+c*3,0xff,0x72+43+c*3,0xe3,0x51])))
    patterns=add(struct.pack('<6H',*channels))
    struct.pack_into('<5H',data,0,positions,patterns,sam_table,orn_table,decode)
    return bytes(data)

class SaaSourceTest(unittest.TestCase):
    def test_decoder(self):
        build=ROOT/'.test-build/saa_source';build.mkdir(parents=True,exist_ok=True)
        main=ROOT/'modules/saa/main';core=ROOT/'components/saasound'
        wrapper=build/'source.cpp'
        wrapper.write_text('#define main saa_main\n#include "'+short(main/'saa_source.cpp').replace('\\','/')+'"\n')
        sources=[(wrapper,'saa.obj'),(main/'etracker.cpp','etracker.obj'),(ROOT/'tests/saa_host.cpp','host.obj')]
        sources += [(core/(n+'.cpp'),n+'.obj') for n in ('SAAAmp','SAADevice','SAAEnv','SAAFreq','SAANoise')]
        exe=build_exe(build,'saa_host.exe',[main,core,FIRMWARE/'modules/sdk/include'],sources)
        if not exe:self.skipTest('MSVC required')
        sample=build/'test.saa';sample.write_bytes(make_saa())
        result=subprocess.run([str(exe),str(sample)],capture_output=True,text=True,timeout=30)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('OK ms=19200 orders=1 samples=1',result.stdout)

if __name__=='__main__':unittest.main()
