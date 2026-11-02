"""Одна команда: проверки ПК/ASan, Xtensa GCC, размеры и обновление превью.

Все сборки, временные файлы и отчёты записываются в cores_ui/build-pc на E:.
Никаких установок, сети и действий с платой.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import subprocess
import struct
import sys

sys.dont_write_bytecode = True
sys.stdout.reconfigure(encoding='utf-8')
sys.stderr.reconfigure(encoding='utf-8')
ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT/'build-pc'
CLANG = Path(r'E:\zx\llvm\bin\clang.exe')
XTENSA = Path(r'E:\zx\esp\.espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin')
ENV = os.environ.copy()
ENV.update(TEMP=str(BUILD), TMP=str(BUILD), PYTHONDONTWRITEBYTECODE='1', PYTHONIOENCODING='utf-8')
ENV['ASAN_OPTIONS'] = 'detect_leaks=0:halt_on_error=1'
ENV['PATH'] = str(CLANG.parent.parent/'lib/clang/23/lib/windows')+os.pathsep+ENV.get('PATH','')
COMMON = ['-std=c++17','-O2','-Wall','-Wextra','-Werror','-fno-exceptions','-fno-rtti']
SOURCES = ['cores_ui.cpp','cui_card.cpp','fonts/pui_font.cpp','assets/cui_art.cpp']


def run(command, log, cwd=BUILD):
    result = subprocess.run([str(s) for s in command],cwd=cwd,env=ENV,capture_output=True)
    output=(result.stdout+result.stderr).decode('utf-8',errors='replace')
    (BUILD/log).write_text(subprocess.list2cmdline([str(s) for s in command])+'\n'+output,encoding='utf-8')
    if result.returncode:
        raise RuntimeError(f'{log}: exit {result.returncode}\n{output}')
    return output


def host(asan=False):
    name='asan' if asan else 'host'
    print(f'Сборка и тесты ПК: {name}',flush=True)
    flags=COMMON+['-D_CRT_SECURE_NO_WARNINGS','-I',str(ROOT)]
    if asan:flags+=['-fsanitize=address','-g','-fno-omit-frame-pointer']
    objects=[]
    for instance,prefix in (('main','cui_'),('ref','cui_ref_'),('repeat','cui_repeat_')):
        output=BUILD/f'stage2_{name}_{instance}.obj'
        renames=[f'-Dcui_{api}={prefix}{api}' for api in ('init','render','reset')]
        run([CLANG,*flags,*renames,'-c',ROOT/'cores_ui.cpp','-o',output],f'build_{name}_{instance}.log')
        objects.append(output)
    exe=BUILD/f'cui_stage2_{name}.exe'
    run([CLANG,*flags,*objects,ROOT/'cui_card.cpp',ROOT/'fonts/pui_font.cpp',ROOT/'assets/cui_art.cpp',
         ROOT/'tests/cui_stage2.cpp','-o',exe],f'build_{name}.log')
    output=run([exe],f'stage2_{name}.log')
    print(output.strip(),flush=True)
    metrics=json.loads((BUILD/'stage2_metrics.json').read_text())
    (BUILD/f'stage2_{name}_metrics.json').write_text(json.dumps(metrics,indent=2)+'\n',encoding='utf-8')


def xtensa():
    print('Сборка Xtensa и размеры секций',flush=True)
    cc=XTENSA/'xtensa-esp32s3-elf-g++.exe'
    version=run([cc,'--version'],'xtensa_version.log')
    if '15.2.0' not in version:raise RuntimeError('Ожидался Xtensa GCC 15.2.0')
    probe=BUILD/'abi_sizes.o'
    run([cc,*COMMON,'-fdata-sections','-I',ROOT,'-c',ROOT/'tests/abi_sizes.cpp','-o',probe],'abi_build.log')
    run([XTENSA/'xtensa-esp32s3-elf-objcopy.exe','-O','binary','-j','.rodata.cui_abi_sizes',probe,BUILD/'abi_sizes.bin'],'abi_extract.log')
    abi=dict(zip(('card_struct','card_alignment','core_struct','model_struct'),struct.unpack('<4I',(BUILD/'abi_sizes.bin').read_bytes())))
    run([CLANG,'-std=c11','-Wall','-Wextra','-Werror','-I',ROOT,'-fsyntax-only',ROOT/'tests/c_api_check.c'],'c_api_check.log')
    objects=[]
    for source in SOURCES:
        name=Path(source).stem
        target=BUILD/f'{name}_xtensa.o'
        run([cc,*COMMON,'-mlongcalls','-ffunction-sections','-fdata-sections','-fstack-usage',
             '-I',ROOT,'-c',ROOT/source,'-o',target],f'build_xtensa_{name}.log')
        objects.append(target)
    size=run([XTENSA/'xtensa-esp32s3-elf-size.exe',*objects],'xtensa_size.log')
    sections=run([XTENSA/'xtensa-esp32s3-elf-size.exe','-A',*objects],'xtensa_sections.log')
    symbols=run([XTENSA/'xtensa-esp32s3-elf-nm.exe','-u',*objects],'xtensa_undefined.log')
    if re.search(r'\b(malloc|calloc|realloc|free|_Zn[aw]\w*|__cxa_throw|__dynamic_cast)\b',symbols):
        raise RuntimeError('Непредусмотренная зависимость памяти/исключений: '+symbols)
    totals={}; rows=[]
    for line in size.splitlines():
        match=re.match(r'\s*(\d+)\s+(\d+)\s+(\d+)\s+\d+\s+[0-9a-f]+\s+(.+)',line)
        if match:rows.append(dict(zip(('text','data','bss'),map(int,match.groups()[:3])),object=Path(match[4]).name))
    if len(rows)!=len(SOURCES):raise RuntimeError('Не разобран вывод xtensa size')
    for key in ('text','data','bss'):totals[key]=sum(row[key] for row in rows)
    groups={'instructions':0,'literals':0,'rodata':0,'data':0,'bss':0}
    for line in sections.splitlines():
        match=re.match(r'(\S+)\s+(\d+)\s+\d+\s*$',line)
        if not match:continue
        section,value=match[1],int(match[2])
        for prefix,key in (('.text','instructions'),('.literal','literals'),('.rodata','rodata'),('.data','data'),('.bss','bss')):
            if section==prefix or section.startswith(prefix+'.'):groups[key]+=value
    result={'objects':rows,'total':totals,'sections':groups,'abi':abi,'compiler':'Xtensa GCC 15.2.0','firmware_linked':False}
    (BUILD/'xtensa_sizes.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(size.strip(),flush=True)


def compare_approved():
    baseline=json.loads((ROOT/'tests/approved_stage2.json').read_text(encoding='utf-8'))
    unchanged=0
    for name,reference in baseline.items():
        if name in ('04_many_unknown_shared.rgb565','05_many_future_c64.rgb565','27_manage_model_key.rgb565'):
            # У C64 больше нет встроенных метаданных; кадр 27 теперь без карточки; 04 и 05 — с
            # 2026-11-02 список больше трёх ядер не компактный, а с полосой прокрутки (пользователь).
            continue
        data=(BUILD/'raw'/name).read_bytes()
        if len(data)!=640*360*2 or hashlib.sha256(data).hexdigest()!=reference:
            raise RuntimeError('Изменился утверждённый дизайн: '+name)
        unchanged+=1
    result={'unchanged_pixel_exact':unchanged,'semantic_changes':['04, 05: more than 3 cores — big cards with a scroll bar','05: C64 without card uses generic fallback','27: no card, Manage hidden']}
    (BUILD/'approved_comparison.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('Сохранение дизайна:',json.dumps(result,ensure_ascii=False),flush=True)


def previews():
    print('Обновление PNG/APNG и сравнение с утверждёнными кадрами',flush=True)
    run([sys.executable,'-B',ROOT/'cui_preview.py'],'stage2_previews.log')
    compare_approved()
    from PIL import Image
    images=sorted((ROOT/'previews').glob('[0-9][0-9]_*.png'))
    count=0
    for path in images:
        if path.name.startswith('00_'):continue
        with Image.open(path) as im:
            im.load()
            if im.size!=(1280,720):raise RuntimeError('Размер PNG: '+path.name)
        count+=1
    durations=[]
    with Image.open(ROOT/'previews/motion_25fps.apng') as im:
        if im.n_frames!=100:raise RuntimeError('APNG: ожидалось 100 кадров')
        for i in range(im.n_frames):
            im.seek(i);im.load();durations.append(im.info.get('duration'))
    if durations!=[40.0]*100:raise RuntimeError('APNG: ожидалось 40 мс/кадр')
    result={'png_count':count,'size':[1280,720],'apng_frames':100,'duration_ms':sum(durations),'fps':25}
    (BUILD/'artifact_check.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('Превью:',json.dumps(result),flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--only',choices=('host','asan','xtensa','previews'))
    args=parser.parse_args();BUILD.mkdir(exist_ok=True)
    actions={'host':lambda:host(False),'asan':lambda:host(True),'xtensa':xtensa,'previews':previews}
    for name,action in actions.items():
        if not args.only or args.only==name:action()
    if not args.only:
        files=[ROOT/s for s in SOURCES]+[ROOT/'cores_ui.h',ROOT/'tests/cui_stage2.cpp',ROOT/'tests/approved_stage1.json',Path(__file__)]
        (BUILD/'stage2_inputs.json').write_text(json.dumps({str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},indent=2)+'\n',encoding='utf-8')
        result={'status':'passed','checks':list(actions),
                'host':json.loads((BUILD/'stage2_host_metrics.json').read_text()),
                'asan':json.loads((BUILD/'stage2_asan_metrics.json').read_text()),
                'xtensa':json.loads((BUILD/'xtensa_sizes.json').read_text()),
                'approved_design':json.loads((BUILD/'approved_comparison.json').read_text()),
                'artifacts':json.loads((BUILD/'artifact_check.json').read_text())}
        (BUILD/'stage2_result.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('PASS: запрошенные проверки завершены; отчёты в build-pc.',flush=True)


if __name__=='__main__':main()
