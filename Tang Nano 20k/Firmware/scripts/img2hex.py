"""Картинка → входной кадр тестбенча кадрового буфера (720×576, слова {PAL_SEL, B5, G5, R5}).

    py -3 scripts/img2hex.py картинка.png кадр.hex [эталон.png]
    py -3 scripts/img2hex.py --test360 кадр.hex [эталон.png]

Картинка 360×288 (точки TS-Config) удваивается по обеим осям, 720×576 берётся как есть.
Цвет квантуется до 5 бит на канал при PAL_SEL=1 (линейная ветвь палитры).
--test360 рисует проверочный экран TS-Config 360×288 без бордюра: рамка в одну точку
по краям, сетка, цветные полосы — край картинки совпадает с краем видимой области.
"""
import sys
from PIL import Image, ImageDraw, ImageFont


def test360():
    img = Image.new("RGB", (360, 288), (0, 0, 64))
    d = ImageDraw.Draw(img)
    bars = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
            (255, 0, 255), (255, 0, 0), (0, 0, 255), (128, 128, 128)]
    for i, c in enumerate(bars):
        d.rectangle([20 + i * 40, 150, 20 + i * 40 + 39, 230], fill=c)
    for x in range(0, 360, 20):
        d.line([x, 0, x, 287], fill=(0, 96, 160))
    for y in range(0, 288, 20):
        d.line([0, y, 359, y], fill=(0, 96, 160))
    d.line([0, 0, 359, 287], fill=(255, 160, 0))
    d.line([359, 0, 0, 287], fill=(255, 160, 0))
    try:
        font = ImageFont.truetype("arial.ttf", 22)
    except OSError:
        font = ImageFont.load_default()
    d.rectangle([60, 40, 300, 120], fill=(0, 0, 0))
    d.text((80, 50), "TS-Config 360x288", fill=(255, 255, 255), font=font)
    d.text((80, 84), "без бордюра", fill=(255, 255, 0), font=font)
    # рамка в одну точку по самому краю — главное для проверки окна
    d.rectangle([0, 0, 359, 287], outline=(255, 0, 0))
    d.rectangle([1, 1, 358, 286], outline=(255, 255, 255))
    return img


def main():
    args = sys.argv[1:]
    img = test360() if args[0] == "--test360" else Image.open(args[0]).convert("RGB")
    dst = args[1]
    if img.size == (360, 288):
        img = img.resize((720, 576), Image.NEAREST)
    if img.size != (720, 576):
        raise SystemExit("нужна картинка 360×288 или 720×576")
    words, ref = [], []
    for r, g, b in img.getdata():
        r5, g5, b5 = r >> 3, g >> 3, b >> 3
        # чёрная точка должна остаться чёрной: границы картинки ищутся по не чёрным
        words.append((1 << 15) | (b5 << 10) | (g5 << 5) | r5)
        ref.append((r5 << 3, g5 << 3, b5 << 3))
    with open(dst, "w") as f:
        f.write("\n".join(f"{w:04x}" for w in words) + "\n")
    if len(args) > 2:
        out = Image.new("RGB", (720, 576))
        out.putdata(ref)
        out.save(args[2])


if __name__ == "__main__":
    main()
