from pathlib import Path
import hashlib, json, os, subprocess, sys
root=Path(__file__).resolve().parents[1]
stage=root/'tests/build'; stage.mkdir(exist_ok=True)
tool=Path(r'E:\zx\tools\rtl\ucrt64')
vc=Path(r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207')
sdk=Path(r'C:\Program Files (x86)\Windows Kits\10'); v='10.0.26100.0'
env=os.environ.copy()
(stage/'tmp').mkdir(exist_ok=True)
env['TEMP']=env['TMP']=str(stage/'tmp')
env['PATH']=str(tool/'bin')+';'+str(vc/'bin/Hostx64/x64')+';'+env['PATH']
env['VERILATOR_ROOT']=str(tool/'share/verilator')
env['INCLUDE']=';'.join(map(str,[vc/'include',*[sdk/'Include'/v/x for x in ['ucrt','shared','um']]]))
# Windows SDK libraries are laid out as <version>/<kind>/x64.
env['LIB']=';'.join(map(str,[vc/'lib/x64',*[sdk/'Lib'/v/x/'x64' for x in ['ucrt','um']]]))
logs=[]
def run(cmd):
    p=subprocess.run(list(map(str,cmd)),cwd=stage,env=env,capture_output=True,timeout=240)
    text=(p.stdout+p.stderr).decode('utf-8','replace'); logs.append(text)
    (root/'tests/run.log').write_text('\n'.join(logs),encoding='utf-8')
    if p.returncode: raise RuntimeError(text[-6000:])
    return text
run([tool/'bin/verilator_bin.exe','--cc','--Wno-fatal','--top-module','link_top','--Mdir','obj',
     root/'tests/link_top.sv',root/'fpga/amiga_link.sv',root/'fpga/amiga_disk.sv',root/'fpga/next_qspi.sv',root/'fpga/fb_sdspi.sv'])
inc=tool/'share/verilator/include'
run([vc/'bin/Hostx64/x64/cl.exe','/nologo','/EHsc','/std:c++20','/utf-8','/O2','/MD','/DVL_TIME_CONTEXT',
     '/I'+str(inc),'/I'+str(inc/'vltstd'),'/Iobj',root/'tests/link_test.cpp',*list((stage/'obj').glob('*.cpp')),
     inc/'verilated.cpp',inc/'verilated_threads.cpp','/Felink_test.exe'])
rom=Path(sys.argv[1]) if len(sys.argv)>1 else root/'ROM/kickstart-3.1-a500_a600_a2000.rom'
result=run([stage/'link_test.exe',rom])
run([vc/'bin/Hostx64/x64/cl.exe','/nologo','/TC','/std:c11','/utf-8','/O2','/MD',
     '/I'+str(root/'esp/main'),root/'tests/app_test.c',root/'esp/main/ps2_hid.c','/Feapp_test.exe'])
# Настоящие ZIP: сжатие deflate (разные блоки), без сжатия; эталон — рядом в .raw.
import random,zipfile
disk=bytearray(901120)
random.seed(5)
for i in range(0,901120,512):
    kind=i//512%4
    disk[i:i+512]=bytes(512) if kind==0 else bytes(random.getrandbits(8) for _ in range(512)) if kind==1 else bytes((i+k*k)&7 for k in range(512)) if kind==2 else (b'AMIGA test sector '*29)[:512]
zips=[]
for name,method,level in (('deflate9',zipfile.ZIP_DEFLATED,9),('deflate1',zipfile.ZIP_DEFLATED,1),('stored',zipfile.ZIP_STORED,None)):
    path=stage/(name+'.zip')
    with zipfile.ZipFile(path,'w',method,compresslevel=level) as z:
        z.writestr('readme.txt','x');z.writestr('A game (1990)(Disk 1 of 2).ADF',bytes(disk))
    (stage/(name+'.zip.raw')).write_bytes(disk);zips.append(path)
# Том EVO: карта Evo FAT32 и FAT16 (make_fat.py); app_test обходит том, как FFS, а дамп блоков сверяет check_evo.py.
# Архивы LHA: свои без сжатия всегда; настоящие со сжатием (-lh1-, -lh5-…-lh7-, уровни 0–2) — из тестов libxmp, если есть.
import shutil
lha_src=Path(r'E:\zx\esp\downloads\libxmp\test-dev\data'); lha_dir=stage/'lha'; lha_names=['l0_lzhuff1','l0_lzhuff5','l1_lzhuff5','l1_lzhuff6','l1_lzhuff7','l2_lzhuff7']
lha_args=[]
if all((lha_src/n).exists() for n in lha_names):
    lha_dir.mkdir(exist_ok=True)
    for n in lha_names: shutil.copyfile(lha_src/n,lha_dir/n)
    lha_args=['--lha',lha_dir]; env['EVO_LHA_SAMPLES']='1'
for name,extra in (('evo32',[]),('evo16',['--fat16'])):
    run([sys.executable,root/'tests/make_fat.py',stage/(name+'.img'),*extra,*lha_args])
env.update(EVO_FAT=str(stage/'evo32.img'),EVO_DUMP=str(stage/'evo32.dump'),EVO_FAT2=str(stage/'evo16.img'),EVO_DUMP2=str(stage/'evo16.dump'))
result+=run([stage/'app_test.exe',rom,*zips])
for name in ('evo32','evo16'):
    result+=run([sys.executable,root/'tests/check_evo.py',stage/(name+'.dump'),stage/(name+'.json')])
result+=run([sys.executable,root/'tests/check_images.py',Path(os.environ.get('AMIGA_TEST_ADF',str(root/'disks/AmigaTestKit-1.21/AmigaTestKit-1.21/AmigaTestKit.adf'))),stage/'new.adf',stage/'new.hdf'])
tested=['fpga/amiga_link.sv','fpga/amiga_disk.sv','fpga/next_qspi.sv','fpga/fb_sdspi.sv',
        'esp/main/app.c','esp/main/disk_menu.h','esp/main/video_config.h','esp/main/zm_level.h','esp/main/amiga_fs.h','esp/main/evo_vol.h','esp/main/evo_lha.h','tests/make_fat.py','tests/check_evo.py','esp/main/net_disks.h','esp/main/ps2_hid.c','esp/main/ps2_hid.h','esp/main/zm_app.h',
        'tests/link_top.sv','tests/link_test.cpp','tests/app_test.c','tests/check_images.py','tests/run.py']
hashes={p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in tested}
(root/'tests/result.json').write_text(json.dumps({'passed':True,'result':result,'hardware':False,'sdf':False,'source_sha256':hashes},indent=2))
print(result)
