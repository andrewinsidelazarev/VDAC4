from pathlib import Path
import hashlib, json, os, re, subprocess
root=Path(__file__).resolve().parents[1]
stage=root/'tests/video-build';stage.mkdir(exist_ok=True)
result_path=root/'tests/video-result.json'
result_path.write_text(json.dumps({'passed':False,'state':'running'}))
tool=Path(r'E:\zx\tools\rtl\ucrt64')
vc=Path(r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207')
sdk=Path(r'C:\Program Files (x86)\Windows Kits\10');v='10.0.26100.0'
env=os.environ.copy()
env['PATH']=str(tool/'bin')+';'+str(vc/'bin/Hostx64/x64')+';'+env['PATH']
env['VERILATOR_ROOT']=str(tool/'share/verilator')
env['INCLUDE']=';'.join(map(str,[vc/'include',*[sdk/'Include'/v/x for x in ('ucrt','shared','um')]]))
env['LIB']=';'.join(map(str,[vc/'lib/x64',*[sdk/'Lib'/v/x/'x64' for x in ('ucrt','um')]]))
sources=['fpga/nanomig.v','fpga/Amber.v','fpga/amiga_vdac4.sv',
         'upstream/src/minimig/Amber.v','upstream/src/minimig-aga/denise_colortable.v',
         'upstream/src/minimig-aga/denise_colortable_ram_mf.v',
         'upstream/src/hdmi/hdmi.sv','upstream/src/hdmi/tmds_channel.sv',
         'tests/video_top.sv','tests/video_test.cpp','tests/run_video.py']
hashes={s:hashlib.sha256((root/s).read_bytes()).hexdigest() for s in sources}
nano=(root/'fpga/nanomig.v').read_text(encoding='utf-8')
assert len(re.findall(r'output \[7:0\]\s+[rgb],',nano))==3
blanking='\n'.join(re.findall(r'^wire \[7:0\] [rgb]_in = .+;',nano,re.M))
assert blanking.count('\n')==2
amber=re.search(r'Amber AMBER\s*\(.*?\);',nano,re.S)[0]
wrapper='''module video_path_from_source(input clk_sys,hbl,vbl,hs_in,vs_in,
input [8:0] htotal,input [1:0] res,input [3:0] video_config,input [23:0] rgb_in,
output [7:0] r,g,b,output hs,vs);
wire [7:0] red=rgb_in[23:16],green=rgb_in[15:8],blue=rgb_in[7:0];
'''+blanking+'\n'+amber+'\nendmodule\n'
(stage/'video_path_from_source.sv').write_text(wrapper)
top=(root/'fpga/amiga_vdac4.sv').read_text(encoding='utf-8')
assert 'wire [7:0] red, green, blue;' in top
rgb=re.search(r'\.rgb\(\s*(.*?)\s*\)',top)[1]
(stage/'video_hdmi_from_source.svh').write_text('''
wire [7:0] red=r,green=g,blue=b;
wire [15:0] silence[1:0];assign silence[0]=0,silence[1]=0;
hdmi #(.DVI_OUTPUT(1)) dvi(.clk_pixel(clk),.clk_pixel_x5(clk),.clk_audio(1'b0),
.pal_mode(1'b1),.screen(2'b0),.short_frame(1'b0),.interlace(1'b0),.reset(reset),
.rgb('''+rgb+'''),.audio_sample_word(silence),.tmds(),.tmds_clock());
''')
(stage/'Amber4.v').write_text((root/'upstream/src/minimig/Amber.v').read_text().replace('module Amber','module Amber4'))
logs=[]
def run(cmd):
    p=subprocess.run(list(map(str,cmd)),cwd=stage,env=env,capture_output=True,timeout=240)
    output=(p.stdout+p.stderr).decode('utf-8','replace');logs.append(output)
    (root/'tests/video-run.log').write_text('\n'.join(logs),encoding='utf-8')
    if p.returncode: raise RuntimeError(output[-6000:])
    return output
run([tool/'bin/verilator_bin.exe','--cc','--Wno-fatal','--no-timing','--top-module','video_top',
     '--Mdir','obj','-I'+str(stage),root/'tests/video_top.sv',stage/'video_path_from_source.sv',
     stage/'Amber4.v',root/'fpga/Amber.v',*[root/s for s in sources[4:8]]])
inc=tool/'share/verilator/include'
run([vc/'bin/Hostx64/x64/cl.exe','/nologo','/EHsc','/std:c++20','/utf-8','/O2','/MD','/DVL_TIME_CONTEXT',
     '/I'+str(inc),'/I'+str(inc/'vltstd'),'/Iobj',root/'tests/video_test.cpp',*list((stage/'obj').glob('*.cpp')),
     inc/'verilated.cpp',inc/'verilated_threads.cpp','/Fevideo_test.exe'])
result=run([stage/'video_test.exe'])
result_path.write_text(json.dumps({'passed':True,'result':result,'source_sha256':hashes,
    'hardware':False,'sdf':False,'serializer':'parallel TMDS symbols only'},indent=2))
print(result)
