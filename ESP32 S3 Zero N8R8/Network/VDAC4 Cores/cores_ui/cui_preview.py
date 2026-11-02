"""Сборка C++ и упаковка RGB565 в точные PNG/APNG ×2. Загрузок нет."""
from pathlib import Path
import argparse
import array
import csv
import html
import io
import json
import os
import struct
import subprocess
import sys
import zlib

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parent
BUILD = ROOT/'build-pc'
OUT = ROOT/'previews'
os.environ['TEMP'] = os.environ['TMP'] = str(BUILD)
os.environ['PYTHONDONTWRITEBYTECODE'] = '1'
from PIL import Image
sys.path.insert(0,str(ROOT/'tools'))
from generate_font import draw_text, main as generate_font
from generate_art import main as generate_art

CAPTIONS = {
    '01_menu_nes':'NES — реальные три ядра, Manage - F1',
    '02_menu_next':'NEXT — карточка ZX Spectrum Next, Manage - F1',
    '03_menu_amiga':'AMIGA — корпус A1200, ядро NanoMig AGA, Manage - F11',
    '04_many_unknown_shared':'12 ядер — незнакомое имя, общий ESP, предупреждение о клавиатуре',
    '05_many_future_c64':'12 ядер — C64 без карточки: общий FPGA core, Manage скрыт',
    '06_single_core':'Одно ядро на карте',
    '07_connecting':'Подключение к ESP',
    '08_reading_sd':'Чтение карты Tang',
    '09_starting_esp':'Запуск слоя ESP выбранного ядра',
    '10_cache_45_percent':'Копирование в кэш BL616 — реальные 45 %',
    '11_loading_unknown':'Загрузка без процентов — три движущиеся точки, без шкалы',
    '12_programming_fpga':'Загрузка в ПЛИС',
    '13_error_no_cores':'Ошибка — нет ядер в /CORES',
    '14_error_esp_text':'Ошибка — длинный текст от ESP',
    '15_error_core':'Ошибка — ядро не запустилось',
    '16_error_update_nes':'Требуется update CORES/NES.ELF to 0.7+',
    '17_error_no_sd':'Ошибка — нет карты Tang',
    '18_error_esp_layer':'Ошибка — слой ESP не запустился',
    '19_error_timeout':'Ошибка — тайм-аут 180 секунд',
    '20_error_esp_lost':'Ошибка — ESP перестал отвечать',
    '21_error_no_esp':'Ошибка — ESP отсутствует на SPI',
    '22_error_no_answer':'Ошибка — ESP не отвечает',
    '23_fade_alpha_4':'Появление над макетом WC — alpha 4, вес 5/16',
    '24_fade_alpha_8':'Появление над макетом WC — alpha 8, вес 9/16',
    '25_fade_alpha_12':'Появление над макетом WC — alpha 12, вес 13/16',
    '26_wc_background':'Исходный демонстрационный фон WC из плеера',
    '27_no_card_no_manage':'Ядро без карточки — общий рисунок, Manage скрыт',
}


def build():
    generate_font()
    generate_art()
    command = [r'E:\zx\llvm\bin\clang.exe','-std=c++17','-O2','-Wall','-Wextra','-Werror',
               '-fno-exceptions','-fno-rtti','-D_CRT_SECURE_NO_WARNINGS','-I',str(ROOT),
               str(ROOT/'cores_ui.cpp'),str(ROOT/'cui_card.cpp'),str(ROOT/'fonts/pui_font.cpp'),
               str(ROOT/'assets/cui_art.cpp'),str(ROOT/'preview/cui_preview.cpp'),
               '-o',str(BUILD/'cui_preview.exe')]
    result=subprocess.run(command,cwd=BUILD,capture_output=True)
    log=result.stdout.decode('utf-8',errors='replace')+result.stderr.decode('utf-8',errors='replace')
    (BUILD/'build.log').write_text(subprocess.list2cmdline(command)+'\n'+log,encoding='utf-8')
    if result.returncode:
        raise RuntimeError(log)
    result=subprocess.run([str(BUILD/'cui_preview.exe')],cwd=BUILD,capture_output=True,check=True)
    (BUILD/'preview.log').write_bytes(result.stdout+result.stderr)
    print(result.stdout.decode('utf-8',errors='replace').strip())


def unpack(path):
    values=array.array('H');values.frombytes(path.read_bytes())
    if sys.byteorder!='little':values.byteswap()
    if len(values)!=640*360:raise ValueError(f'Invalid canvas: {path}')
    out=bytearray(len(values)*3)
    for i,p in enumerate(values):
        r=p>>11;g=(p>>5)&63;b=p&31
        out[i*3]=(r<<3)|(r>>2)
        out[i*3+1]=(g<<2)|(g>>4)
        out[i*3+2]=(b<<3)|(b>>2)
    return Image.frombytes('RGB',(640,360),bytes(out)).resize((1280,720),Image.Resampling.NEAREST)


def fade(background,overlay,alpha):
    # Точная заданная формула с floor, в том числе при отрицательной разности.
    a=background.convert('RGB').tobytes();b=overlay.convert('RGB').tobytes()
    return Image.frombytes('RGB',overlay.size,bytes(x+(y-x)*(alpha+1)//16 for x,y in zip(a,b)))


def animation():
    # Обычный save_all объединяет одинаковые кадры. Сохраняем ровно 100 × 40 мс,
    # используя PNG-сжатие Pillow и стандартные служебные чанки APNG.
    def chunk(kind,data):
        return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
    with (OUT/'motion_25fps.apng').open('wb') as out:
        out.write(b'\x89PNG\r\n\x1a\n')
        sequence=0
        for i in range(100):
            frame=unpack(BUILD/f'raw/motion_{i:03d}.rgb565')
            encoded=io.BytesIO();frame.save(encoded,format='PNG')
            data=encoded.getvalue();offset=8;idat=[];header=None
            while offset<len(data):
                size=struct.unpack_from('>I',data,offset)[0]
                kind=data[offset+4:offset+8];payload=data[offset+8:offset+8+size]
                if kind==b'IHDR':header=payload
                if kind==b'IDAT':idat.append(payload)
                offset+=12+size
            if i==0:
                out.write(chunk(b'IHDR',header))
                out.write(chunk(b'acTL',struct.pack('>II',100,0)))
            control=struct.pack('>IIIIIHHBB',sequence,1280,720,0,0,1,25,0,0)
            out.write(chunk(b'fcTL',control));sequence+=1
            for payload in idat:
                if i==0:out.write(chunk(b'IDAT',payload))
                else:
                    out.write(chunk(b'fdAT',struct.pack('>I',sequence)+payload));sequence+=1
        out.write(chunk(b'IEND',b''))


def gallery():
    cards='\n'.join(f'<figure><a href="{name}.png"><img loading="lazy" src="{name}.png" width="1280" height="720"></a><figcaption>{html.escape(caption)}</figcaption></figure>' for name,caption in CAPTIONS.items())
    page='''<!doctype html><html lang="ru"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>VDAC4 Cores — утверждённый дизайн</title><style>
body{margin:0;background:#0b131e;color:#eeeede;font:17px/1.5 sans-serif}main{max-width:1280px;margin:auto;padding:32px 24px}
h1{font-size:32px;margin:0}p{color:#adbdca}a{color:#78d8c6}figure{margin:28px 0 48px}img{display:block;width:100%;height:auto;image-rendering:pixelated;border:1px solid #293d50;box-sizing:border-box}figcaption{padding-top:12px}button{padding:10px 16px;background:#253e49;color:#fff;border:1px solid #5da698;cursor:pointer}
</style><main><h1>VDAC4 Cores / утверждённый дизайн</h1><p>640 × 360 RGB565 → 1280 × 720, точный масштаб ×2. Карточки NES, NEXT и AMIGA прочитаны из ELF. Результаты этапа 3 — в README.</p>
<p><a href="../STAGE1.md">Список кадров</a> · <a href="00_overview.png">Обзорный лист</a> · <a href="motion_25fps.apng">Анимация APNG</a></p>
<figure><img id="motion" src="motion_25fps.apng" width="1280" height="720"><figcaption>4 секунды, 25 кадров/с: NES → NEXT → AMIGA → NEXT → NES → запуск и загрузка.</figcaption></figure>
<button onclick="document.getElementById('motion').src='01_menu_nes.png'">Остановить</button> <button onclick="document.getElementById('motion').src='motion_25fps.apng?'+Date.now()">Повторить</button>
'''+cards+'</main></html>'
    (OUT/'index.html').write_text(page,encoding='utf-8')
    sheet=Image.new('RGB',(1280,((len(CAPTIONS)+2)//3)*272),(10,17,28))
    for i,(name,caption) in enumerate(CAPTIONS.items()):
        x=(i%3)*426;y=(i//3)*272
        with Image.open(OUT/f'{name}.png') as im:
            # Обзорный лист служит навигацией; точные кадры лежат отдельно.
            sheet.paste(im.resize((426,240),Image.Resampling.NEAREST),(x,y))
        draw_text(sheet,(x+10,y+246),name,13,(174,194,203))
    sheet.save(OUT/'00_overview.png')


def statistics():
    with (BUILD/'preview_metrics.csv').open(encoding='utf-8') as f:rows=list(csv.DictReader(f))
    motion=[r for r in rows if r['name'].startswith('motion_')]
    partial=[r for r in motion if r['full']=='0']
    stats={
        'animation_frames':100,'fps':25,'duration_ms':4000,
        'animation_full_frames':sum(int(r['full']) for r in motion),
        'animation_partial_area_max':max(int(r['area']) for r in partial),
        'animation_partial_area_mean':round(sum(int(r['area']) for r in partial)/len(partial),1),
        'benchmark_pc':json.loads((BUILD/'benchmark.json').read_text()),
        'font':json.loads((ROOT/'fonts/metrics.json').read_text()),
        'art':json.loads((ROOT/'assets/metrics.json').read_text()),
        'canvas_bytes':460800,'extra_psram_bytes':460800,
        'esp_timing_verified':False,'stage2_started':True,'stage3_cards_from_elf':True,'design_approved':True,
    }
    (OUT/'render_stats.json').write_text(json.dumps(stats,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
    print(json.dumps(stats,ensure_ascii=False,indent=2))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--no-build',action='store_true',help='Упаковать ранее полученные RGB565')
    args=parser.parse_args()
    BUILD.mkdir(exist_ok=True);(BUILD/'raw').mkdir(exist_ok=True);OUT.mkdir(exist_ok=True)
    if not args.no_build:build()
    for path in sorted((BUILD/'raw').glob('[0-9][0-9]_*.rgb565')):
        unpack(path).save(OUT/f'{path.stem}.png')
    with Image.open(ROOT/'assets/wc_background.png') as src:wc=src.convert('RGB')
    if wc.size!=(1280,720):wc=wc.resize((1280,720),Image.Resampling.NEAREST)
    wc.save(OUT/'26_wc_background.png')
    with Image.open(OUT/'01_menu_nes.png') as menu:
        for index,alpha in enumerate((4,8,12),23):fade(wc,menu,alpha).save(OUT/f'{index}_fade_alpha_{alpha}.png')
    animation()
    gallery();statistics()
    print('Gallery:',OUT/'index.html')


if __name__=='__main__':main()
