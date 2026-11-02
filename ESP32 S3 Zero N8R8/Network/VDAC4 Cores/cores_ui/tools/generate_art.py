"""Собственный пиксель-арт: контуры корпусов, палитра RGB565 и RLE.

Только геометрические примитивы Pillow. Чужих картинок и логотипов нет.
Нулевой индекс пропускается при рисовании спрайта в непрозрачный холст.
"""
from pathlib import Path
import json
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
COLORS = [
    '#000000', '#090f1d', '#172332', '#293a4b', '#405365', '#698294',
    '#a4b3ba', '#d3dbd8', '#f0ede1', '#c0bba8', '#8b8d80', '#ece2c5',
    '#d8ccb0', '#aaa28e', '#616c6e', '#e46b58', '#ffa76e', '#f2cd70',
    '#79c48d', '#63d4c4', '#559cc6', '#8d92c8', '#c9a2ef', '#344856',
]


def machine(kind):
    im = Image.new('P', (256, 128), 0)
    d = ImageDraw.Draw(im)
    # Тень и подиум общие; пустота вокруг не имеет отдельного альфа-канала.
    d.polygon([(18,105),(210,95),(250,110),(54,124)], fill=1)
    d.line([(34,119),(207,110)], fill=3)
    if kind == 'nes':
        d.polygon([(64,23),(194,13),(231,40),(99,51)], fill=8)
        d.polygon([(64,23),(99,51),(99,91),(64,66)], fill=6)
        d.polygon([(99,51),(231,40),(231,80),(99,91)], fill=7)
        d.polygon([(99,76),(231,65),(231,80),(99,91)], fill=4)
        d.polygon([(157,16),(174,15),(211,42),(193,43)], fill=3)
        for i in range(9):
            d.line([(160+i*3,19+i*2),(172+i*3,18+i*2)], fill=5)
        d.polygon([(111,55),(188,48),(188,64),(111,71)], fill=9)
        d.line([(112,55),(187,49)], fill=8)
        d.line([(111,69),(187,62)], fill=14)
        d.rectangle((108,78,122,81), fill=15)
        d.rectangle((126,76,137,79), fill=6)
        d.rectangle((109,83,112,85), fill=15)
        for x in (200,215):
            d.rectangle((x,68,x+8,73), fill=1)
            d.rectangle((x+2,69,x+6,70), fill=10)
        d.line([(204,72),(206,95),(183,104),(150,101)], fill=3, width=3)
        # Прямоугольный геймпад: крестовина, две круглые кнопки, без надписей.
        d.polygon([(35,83),(133,77),(154,91),(154,112),(55,119),(35,105)], fill=6)
        d.polygon([(39,84),(132,80),(150,92),(57,98)], fill=8)
        d.polygon([(55,98),(150,92),(150,109),(55,115)], fill=2)
        d.rectangle((63,98,70,110), fill=6)
        d.rectangle((59,102,75,106), fill=6)
        d.rectangle((65,100,68,108), fill=1)
        d.rectangle((61,103,73,105), fill=1)
        d.ellipse((123,97,133,106), fill=15)
        d.ellipse((137,95,147,104), fill=15)
        d.line([(88,105),(97,104)], fill=7, width=2)
        d.line([(103,104),(112,103)], fill=7, width=2)
    elif kind in ('next','amiga'):
        is_next = kind == 'next'
        top, side, edge, key, face = (3,2,4,2,5) if is_next else (11,13,9,12,8)
        d.polygon([(28,35),(211,20),(242,89),(11,107)], fill=side)
        d.polygon([(28,29),(211,14),(242,82),(11,100)], fill=top)
        d.line([(28,29),(211,14),(242,82)], fill=edge, width=2)
        d.line([(11,100),(242,82)], fill=edge)
        # Верхняя полка, клавиатура и отдельная группа справа у Amiga.
        d.polygon([(34,45),(209,31),(226,76),(24,92)], fill=1 if is_next else 13)
        rows = 4
        cols = 13 if is_next else 15
        for row in range(rows):
            for col in range(cols):
                x = 34 + col*12 - row*2
                y = 47 + row*10 - col
                if not is_next and col == 11:
                    continue
                d.polygon([(x,y),(x+9,y-1),(x+11,y+6),(x+1,y+7)], fill=key)
                d.line([(x+1,y),(x+8,y-1)], fill=face)
                if (col+row)%3 != 0:
                    d.line([(x+4,y+2),(x+5,y+2)], fill=7 if is_next else 14)
        d.polygon([(69,85),(160,78),(163,85),(68,93)], fill=key)
        d.line([(70,85),(160,78)], fill=face)
        if is_next:
            for i,col in enumerate((15,17,18,20)):
                x=174+i*8
                d.polygon([(x,21),(x+5,21),(x+11,30),(x+6,30)], fill=col)
            d.rectangle((39,32,52,34), fill=4)
            d.rectangle((56,31,64,33), fill=4)
        else:
            for i in range(9):
                d.line([(50+i*12,28-i),(52+i*12,35-i)], fill=13)
            d.line([(220,52),(229,72)], fill=14, width=2)
            d.rectangle((32,39,37,40), fill=18)
            d.rectangle((40,38,45,39), fill=15)
        # Кабель и мышь рядом с клавиатурой.
        d.line([(215,91),(244,97),(239,110),(218,111)], fill=4, width=2)
        d.polygon([(195,103),(213,101),(226,116),(207,121),(195,114)], fill=top)
        d.line([(205,104),(214,114)], fill=edge)
    else:
        d.polygon([(63,37),(178,22),(220,91),(101,111)], fill=3)
        d.polygon([(63,32),(178,17),(220,86),(101,105)], fill=19)
        d.polygon([(70,37),(175,23),(211,82),(104,98)], fill=23)
        for x in range(95,173,9):
            d.line([(x,41),(x+10,58)], fill=10, width=3)
            d.line([(x+21,76),(x+28,88)], fill=10, width=3)
        d.polygon([(101,44),(159,36),(186,76),(125,85)], fill=2)
        d.polygon([(111,48),(155,42),(175,71),(130,77)], fill=4)
        d.line([(117,52),(153,47),(167,67)], fill=19)
        for x,y in ((79,43),(175,30),(111,91),(201,79)):
            d.ellipse((x-3,y-3,x+3,y+3), fill=17)
            d.ellipse((x-1,y-1,x+1,y+1), fill=1)
    im.putpalette([c for s in COLORS for c in bytes.fromhex(s[1:])] + [0]*(768-len(COLORS)*3))
    return im


def encode(im):
    data = list(im.getdata())
    out = []
    i = 0
    while i < len(data):
        end = i+1
        while end < len(data) and data[end] == data[i] and end-i < 255:
            end += 1
        out += [end-i,data[i]]
        i=end
    return out


def main():
    out = ROOT/'assets'
    out.mkdir(exist_ok=True)
    arrays, entries, metrics = [], [], {}
    for kind in ('generic',):
        im=machine(kind)
        im.convert('RGB').save(out/f'{kind}_art.png')
        for suffix,frame in (('',im),('_small',im.resize((56,28),Image.Resampling.NEAREST))):
            name=kind+suffix
            data=encode(frame)
            arrays.append(f'const uint8_t {name}[] = {{\n'+ '\n'.join('  '+','.join(map(str,data[i:i+40]))+',' for i in range(0,len(data),40))+'\n};')
            entries.append(f'  {{{name}, {len(data)}, {frame.width}, {frame.height}}},')
            metrics[name]=len(data)
    pal=[]
    for color in COLORS:
        r,g,b=bytes.fromhex(color[1:])
        pal.append(((r>>3)<<11)|((g>>2)<<5)|(b>>3))
    # Таблицы постоянного фона: деления и квантование выполняются на ПК.
    bayer=(0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5)
    bg_x=[(x-490)**2//1800 for x in range(640)]
    bg_y=[(y-170)**2//700 for y in range(360)]
    bg_color=[]
    for glow in range(91):
        r,g,b=10+glow//12,17+glow//9,28+glow//6
        for n in bayer:
            rr=(r*31*16//255+n)//16
            gg=(g*63*16//255+n)//16
            bb=(b*31*16//255+n)//16
            bg_color.append((rr<<11)|(gg<<5)|bb)
    backgrounds='\n'.join('const uint16_t '+name+'['+str(len(data))+'] = {\n'+
        '\n'.join('  '+','.join(map(str,data[i:i+32]))+',' for i in range(0,len(data),32))+'\n};'
        for name,data in (('bg_x',bg_x),('bg_y',bg_y),('bg_color',bg_color)))
    (out/'cui_art.h').write_text('''#pragma once
#include "../cui_card.h"
namespace cui_art {
using Sprite = cui_card_image_t;
extern const uint16_t palette[24];
extern const Sprite sprites[2];
extern const uint16_t bg_x[640], bg_y[360], bg_color[1456];
}
''',encoding='utf-8')
    (out/'cui_art.cpp').write_text('// Сгенерировано tools/generate_art.py; оригинальные контуры корпусов.\n#include "cui_art.h"\nnamespace cui_art {\nnamespace {\n'+'\n'.join(arrays)+'\n}\n'+backgrounds+'\nconst uint16_t palette[24] = {'+','.join(map(str,pal))+'};\nconst Sprite sprites[2] = {\n'+'\n'.join(entries)+'\n};\n}\n',encoding='utf-8')
    metrics['palette_bytes']=len(pal)*2
    metrics['background_bytes']=(len(bg_x)+len(bg_y)+len(bg_color))*2
    metrics['total_without_descriptors']=sum(metrics.values())
    (out/'metrics.json').write_text(json.dumps(metrics,indent=2)+'\n',encoding='utf-8')
    print('Art:',metrics['total_without_descriptors'],'bytes + descriptors')


if __name__ == '__main__':
    main()
