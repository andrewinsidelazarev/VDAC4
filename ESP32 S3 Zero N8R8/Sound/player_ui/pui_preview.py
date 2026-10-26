"""Сборка и PNG-превью отрисовки C++17. Ничего не скачивает и не устанавливает.

Запуск: E:\\zx\\esp\\pio-venv\\Scripts\\python.exe pui_preview.py
Все производные файлы остаются в player_ui. Исходники прошивки только читаются.
"""
from pathlib import Path
import argparse
import array
import cmath
import csv
import html
import json
import math
import os
import struct
import subprocess
import sys
import wave
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / 'tools'))
from generate_font import draw_text, main as generate_font

BUILD = ROOT / 'build-pc'
OUT = ROOT / 'previews'
CLANG = Path(r'E:\zx\llvm\bin\clang.exe')
MUSIC = Path(r'E:\zx\Music')
BAYER = (0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5)
CAPTIONS = {
    '01_mp3': 'MP3: обложка, все метки, 1:23 / 3:45, Track 3 of 10',
    '02_cyrillic': 'Длинные название и исполнитель в CP866: многоточие',
    '03_xm_8_channels': 'XM: 8 каналов, Samples / Effects, без обложки',
    '04_xm_32_channels': 'XM: 32 канала',
    '05_vgz_opl3': 'VGZ: YMF262 (OPL3), 18 каналов',
    '06_midi': 'MIDI: Instruments / Tracks',
    '07_radio_icy': 'Радио: станция, ICY, время без длительности',
    '08_radio_stations': 'Список станций, текущая выделена',
    '09_loading_45': 'Загрузка 45%: Sending to the ESP: OPL.ELF',
    '10_buffering': 'Buffering',
    '11_paused': 'Пауза, спектр успокоился',
    '12_error_skipping': 'Cannot play this file, skipping',
    '13_error_stream_closed': 'ERROR: ESP closed the stream',
    '14_help_files': 'Справка H: файлы, перемотка и особенность модулей',
    '15_help_radio': 'Справка H: радио',
    '16_details': 'Подробности I: File, State, Format, Sent, Queue, Faults, Volume',
    '17_idle': 'Нет открытого файла',
    '18_finishing': 'Файл передан, очередь доигрывает',
    '19_temperature_70': 'Температура 70°C: предупреждение',
    '20_temperature_75': 'Температура 75°C: тревога',
    '21_filename_fallback': 'Нет меток: название заменено именем файла',
    '22_unknown_duration': 'Неизвестная длительность, время больше часа',
    '23_flac': 'FLAC 24 bit / 96 kHz',
    '24_connecting': 'Радио: Connecting...',
    '25_view_none': 'Вид без спектра и каналов',
    '26_fade_alpha_4': 'Появление над WC: alpha 4 (5/16)',
    '27_fade_alpha_8': 'Появление над WC: alpha 8 (9/16)',
    '28_fade_alpha_12': 'Появление над WC: alpha 12 (13/16)',
    '29_wc_background': 'Нарисованный экран WC для демонстрации появления',
}


def font_text_header():
    fields = {
        'kLongTitle': 'Последний трамвай уходит за горизонт большого города',
        'kLongArtist': 'Электрические сны далёкого северного побережья',
        'kAlbum': 'Ночные маршруты',
        'kStation': 'Радио Ночные огни',
    }
    lines = ['// Данные превью в CP866; не входят в отрисовку ESP.']
    for key, value in fields.items():
        encoded = ''.join(f'\\{b:03o}' for b in value.encode('cp866'))
        lines.append(f'constexpr char {key}[] = "{encoded}";')
    (BUILD / 'preview_text.h').write_text('\n'.join(lines)+'\n',encoding='utf-8')


def cover_art():
    """Собственная процедурная обложка, только для стенда превью."""
    size=192
    im=Image.new('RGB',(size,size)); pix=im.load()
    for y in range(size):
        for x in range(size):
            radial=max(0,1-((x-138)**2+(y-60)**2)/18000)
            pix[x,y]=(int(16+radial*29+y*.1),int(34+radial*42),int(49+radial*31))
    d=ImageDraw.Draw(im)
    d.ellipse((93,23,169,99),fill=(240,165,108))
    d.ellipse((101,31,161,91),fill=(251,196,133))
    for band in range(13):
        points=[]
        for x in range(-5,size+6,2):
            y=91+band*7+math.sin(x/37+band*.2)*14+math.cos(x/63)*10
            points.append((x,y))
        d.line(points,fill=(54+band*4,113+band*5,122+band*4),width=2)
    draw_text(im,(15,15),'NORTHBOUND',13,(232,235,215))
    d.rectangle((0,146,192,192),fill=(14,31,41))
    draw_text(im,(15,150),'QUIET',16,(237,224,194))
    draw_text(im,(15,171),'HORIZONS',13,(154,196,190))
    values=[]
    for y in range(size):
        for x in range(size):
            r,g,b=im.getpixel((x,y)); threshold=BAYER[(y%4)*4+x%4]
            rr=min(31,(r*31*16//255+threshold)//16)
            gg=min(63,(g*63*16//255+threshold)//16)
            bb=min(31,(b*31*16//255+threshold)//16)
            values.append((rr<<11)|(gg<<5)|bb)
    (BUILD/'cover.rgb565').write_bytes(struct.pack('<'+'H'*len(values),*values))


def fft(values):
    """Радикс-2, только стандартная библиотека; инструмент ПК, не код ESP."""
    n=len(values); j=0
    for i in range(1,n):
        bit=n>>1
        while j&bit: j^=bit; bit>>=1
        j^=bit
        if i<j: values[i],values[j]=values[j],values[i]
    length=2
    while length<=n:
        factor=cmath.exp(-2j*math.pi/length)
        for start in range(0,n,length):
            w=1
            for pos in range(start,start+length//2):
                a=values[pos]; b=values[pos+length//2]*w
                values[pos]=a+b; values[pos+length//2]=a-b; w*=factor
        length*=2
    return values


def audio_spectrum(wav_override=None):
    source=Path(wav_override) if wav_override else next(MUSIC.rglob('melodia.wav'),None)
    rate=44100; samples=[]; origin='Синтез: WAV не найден или не поддерживается'
    declared_frames=0; actual_seconds=0; synthetic=False
    if source:
        try:
            with wave.open(str(source),'rb') as w:
                rate=w.getframerate(); channels=w.getnchannels(); size=w.getsampwidth()
                if size not in (1,2,3,4): raise ValueError('PCM width')
                declared_frames=w.getnframes()
                # Старый WAV может обрываться раньше длины из заголовка. Читаем с начала.
                raw=w.readframes(rate*5)
                for at in range(0,len(raw)-size*channels+1,size*channels):
                    row=[]
                    for ch in range(channels):
                        data=raw[at+ch*size:at+(ch+1)*size]
                        value=(data[0]-128)/128 if size==1 else int.from_bytes(data,'little',signed=True)/(1<<(size*8-1))
                        row.append(value)
                    samples.append(sum(row)/channels)
            if not samples: raise ValueError('нет доступных PCM-отсчётов')
            actual_seconds=len(samples)/rate
            origin=str(source)
        except (OSError,wave.Error,ValueError) as e:
            origin=f'Синтез: {type(e).__name__}'
    if not samples:
        rate=44100; synthetic=True
        samples=[.3*math.sin(2*math.pi*110*i/rate)*(.5+.5*math.sin(i/rate*12))+
                 .12*math.sin(2*math.pi*880*i/rate) for i in range(rate*5)]
    n=2048; window=[.5-.5*math.cos(2*math.pi*i/(n-1)) for i in range(n)]
    result=bytearray()
    for frame in range(100):
        start=round(frame*.04*rate)
        spectrum=fft([complex(samples[(start+i)%len(samples)]*window[i]) for i in range(n)])
        for band in range(64):
            low=40*(20000/40)**(band/64); high=40*(20000/40)**((band+1)/64)
            if low>=rate/2: value=0
            else:
                first=max(1,math.ceil(low*n/rate)); last=min(n//2,max(first+1,math.ceil(high*n/rate)))
                amplitude=max((abs(spectrum[k])*4/n for k in range(first,last)),default=0)
                db=20*math.log10(max(amplitude,1e-8))
                value=round(max(0,min(255,(db+72)*255/72)))
            result.append(value)
    (BUILD/'spectrum.bin').write_bytes(result)
    (OUT/'audio_source.json').write_text(json.dumps({
        'source':origin,'sample_rate_hz':rate,'seconds':4,'frames':100,'fps':25,
        'synthetic':synthetic,'available_excerpt_seconds':actual_seconds,
        'declared_frames':declared_frames,'looped_excerpt':not synthetic and actual_seconds<4.3,
        'fft':n,'window':'Hann','bands':64,'range_hz':[40,20000],
        'scale':'0..255 = -72..0 dBFS; 32 display bars = max of adjacent pair',
        'note':f'Полосы выше частоты Найквиста ({rate//2} Hz) пустые. Короткий фрагмент зациклен. Аудио не копируется.'
    },ensure_ascii=False,indent=2)+'\n',encoding='utf-8')


def build():
    cmd=[str(CLANG),'-std=c++17','-O2','-Wall','-Wextra','-Werror','-fno-exceptions','-fno-rtti',
         '-D_CRT_SECURE_NO_WARNINGS',
         '-I',str(ROOT),'-I',str(BUILD),'-I',str(ROOT.parent.parent/'Firmware/modules/sdk/include'),
         str(ROOT/'player_ui.cpp'),str(ROOT/'fonts/pui_font.cpp'),str(ROOT/'preview/pui_preview.cpp'),
         '-o',str(BUILD/'pui_preview.exe')]
    env=os.environ.copy(); env['TEMP']=str(BUILD); env['TMP']=str(BUILD)
    result=subprocess.run(cmd,cwd=BUILD,env=env,capture_output=True)
    log=result.stdout.decode('utf-8',errors='replace')+result.stderr.decode('utf-8',errors='replace')
    (BUILD/'build.log').write_text(subprocess.list2cmdline(cmd)+'\n'+log,encoding='utf-8')
    if result.returncode: raise RuntimeError(log)
    result=subprocess.run([str(BUILD/'pui_preview.exe')],cwd=BUILD,env=env,capture_output=True,check=True)
    print(result.stdout.decode('utf-8',errors='replace').strip())


def unpack565(path):
    values=array.array('H'); values.frombytes(path.read_bytes())
    if sys.byteorder!='little': values.byteswap()
    out=bytearray(len(values)*3)
    for i,p in enumerate(values):
        r=p>>11; g=(p>>5)&63; b=p&31
        out[i*3]=(r<<3)|(r>>2); out[i*3+1]=(g<<2)|(g>>4); out[i*3+2]=(b<<3)|(b>>2)
    return Image.frombytes('RGB',(640,360),bytes(out))


def wc_background():
    """Макет WC на Evo, не снимок реального компьютера. Все надписи собственным шрифтом."""
    im=Image.new('RGB',(640,360),(0,0,136)); d=ImageDraw.Draw(im)
    d.rectangle((14,12,625,329),outline=(170,210,255),width=2)
    draw_text(im,(220,12),'Wild Commander',(13),(255,255,255))
    for x in (20,325):
        d.rectangle((x,34,x+294,295),outline=(160,200,255),width=1)
        draw_text(im,(x+8,37),'C:/MUSIC/',13,(255,255,255))
        for i,name in enumerate(('..','NORTHB~1  <DIR>','AFTER    MP3','EVENING  MID','MIDNIGHT XM','NEON     VGZ','RADIO    M3U','COVER    JPG','POWERKIT MWK','SESSION  FLAC')):
            draw_text(im,(x+10,61+i*21),name,13,(160,210,255))
    d.rectangle((22,102,311,121),fill=(0,160,170))
    draw_text(im,(30,103),'AFTER    MP3     3 600 112',13,(255,255,255))
    d.rectangle((96,81,548,280),fill=(0,0,112),outline=(255,255,255),width=2)
    draw_text(im,(214,88),'VDAC4 Media Player',13,(255,255,255))
    for i,t in enumerate(('After the Light','Northbound','Quiet Horizons, 2026','MP3  128 kbps  44.1 kHz  stereo','Track 3 of 10','01:23                          03:45','Playing')):
        draw_text(im,(114,113+i*20),t,13,(255,255,255))
    draw_text(im,(23,307),'C:  10 files         36 001 128 bytes',13,(255,255,255))
    draw_text(im,(22,338),'1 Help   2 Menu   3 View   4 Edit   5 Copy   6 Move   7 MkDir',13,(255,255,255))
    return im


def export():
    for raw in sorted((BUILD/'raw').glob('[0-9]*.rgb565')):
        unpack565(raw).resize((1280,720),Image.Resampling.NEAREST).save(OUT/(raw.stem+'.png'))
    frames=[]
    for raw in sorted((BUILD/'raw').glob('motion_*.rgb565')):
        frames.append(unpack565(raw).resize((1280,720),Image.Resampling.NEAREST))
    frames[0].save(OUT/'spectrum_25fps.apng',format='PNG',save_all=True,append_images=frames[1:],
                   duration=40,loop=0,disposal=0,blend=0)
    bg=wc_background()
    bg.resize((1280,720),Image.Resampling.NEAREST).save(OUT/'29_wc_background.png')
    player=unpack565(BUILD/'raw/01_mp3.rgb565')
    background_bytes=bg.tobytes(); player_bytes=player.tobytes()
    for i,a in enumerate((4,8,12),26):
        blended=bytes(b+(p-b)*(a+1)//16 for b,p in zip(background_bytes,player_bytes))
        Image.frombytes('RGB',(640,360),blended).resize((1280,720),Image.Resampling.NEAREST).save(OUT/f'{i:02d}_fade_alpha_{a}.png')
    # Обзорный лист — только навигация, полноразмерные PNG всегда отдельно.
    keys=['01_mp3','02_cyrillic','03_xm_8_channels','04_xm_32_channels','07_radio_icy','08_radio_stations',
          '09_loading_45','13_error_stream_closed','14_help_files','16_details','17_idle','27_fade_alpha_8']
    sheet=Image.new('RGB',(1280,4*280),(10,16,25))
    for i,key in enumerate(keys):
        thumb=Image.open(OUT/(key+'.png')).resize((426,240),Image.Resampling.NEAREST)
        # Три столбца, четыре ряда; подпись — номер и имя файла.
        x=(i%3)*426;y=(i//3)*280
        sheet.paste(thumb,(x,y))
        draw_text(sheet,(x+10,y+251),key[:26],13,(190,220,222))
    sheet.save(OUT/'00_overview.png')


def write_gallery():
    cards=[]
    for key,caption in CAPTIONS.items():
        cards.append(f'<figure><a href="{key}.png"><img loading="lazy" src="{key}.png" width="1280" height="720" alt="{html.escape(caption)}"></a><figcaption>{key}: {html.escape(caption)}</figcaption></figure>')
    page='''<!doctype html><html lang="ru"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>VDAC4 — этап 1</title><style>body{margin:24px auto;max-width:1280px;padding:0 20px;background:#0a1019;color:#e9f1f3;font:16px sans-serif}h1{font-size:28px}p{color:#a7bec9}figure{margin:32px 0}img{width:100%;height:auto;image-rendering:pixelated;border-radius:8px}figcaption{margin:8px 0 0}a{color:#5ce2c6}</style>
<h1>VDAC4 Media Player · этап 1</h1><p>640×360 RGB565 → 1280×720, точное увеличение ×2. Нажмите картинку, чтобы открыть PNG.</p>
<p>Анимация: 4 секунды, 25 кадров/с. Спектр — из локального WAV, без звука.</p>
<figure><img src="spectrum_25fps.apng" width="1280" height="720" alt="Анимация спектра"><figcaption>Спектр: attack / decay / peak hold.</figcaption></figure>
'''+''.join(cards)+'</html>'
    (OUT/'index.html').write_text(page,encoding='utf-8')
    with (BUILD/'preview_metrics.csv').open(newline='') as f:
        rows=list(csv.DictReader(f))
    animation=[int(row['pixels']) for row in rows if row['series']=='animation' and row['frame']!='0']
    (OUT/'render_stats.json').write_text(json.dumps({
        'animation_incremental_frames':len(animation),'animation_mean_dirty_pixels':round(sum(animation)/len(animation),1),
        'animation_max_dirty_pixels':max(animation),'static_pngs':len(CAPTIONS),
        'note':'Статистика генерации превью, не тесты этапа 2 и не замер времени ESP.'
    },ensure_ascii=False,indent=2)+'\n',encoding='utf-8')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wav',help='Другой локальный PCM WAV для спектра')
    parser.add_argument('--no-build',action='store_true',help='Повторно экспортировать уже нарисованный RGB565')
    args=parser.parse_args()
    BUILD.mkdir(exist_ok=True); (BUILD/'raw').mkdir(exist_ok=True); OUT.mkdir(exist_ok=True)
    if not args.no_build:
        generate_font(); font_text_header(); cover_art(); audio_spectrum(args.wav); build()
    export(); write_gallery()
    print(f'Готово: {len(CAPTIONS)} PNG + обзорный лист + APNG, {OUT}')


if __name__=='__main__':
    main()
