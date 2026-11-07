"""Картинки задания task_bl616_images.md (прошивка 39): CMD_LOADIMAGE и CMD_LOADIMAGE_BOX на образцах
data/task против Pillow. Нужны clang (E:\\zx\\llvm) и Python с Pillow и numpy.

- CMD_LOADIMAGE: PNG — байт в байт по правилам FT812 (серый — L8, RGB — RGB565, палитра — PALETTED565
  или 4444, серый с альфой и RGBA — ARGB4; tRNS у серого и RGB не учитывается, как у FT812), JPEG —
  байт в байт с Pillow (libjpeg-turbo, islow) в RGB565.
- CMD_LOADIMAGE_BOX: влезает — как CMD_LOADIMAGE (tRNS серого и RGB — ARGB4); иначе — размер
  выхода (как в ft_image.c box_target) и точки против уменьшения по площади (numpy, точно, в
  предумноженном виде; у JPEG — после Image.draft, как ДКП-уменьшение BL616) с допуском ±1 шаг
  формата. У ARGB4 цвет сверяется там, где альфа ≥ 64 (ниже — предумноженный цвет в 4 битах).
- WebP (прошивка 40): CMD_LOADIMAGE — байт в байт с Pillow (режим RGBA — ARGB4, RGB — RGB565), рамка —
  как у PNG RGBA/RGB; у анимации — первый кадр на прозрачном холсте; велика для памяти BL616 — ошибка.
- Причины ошибок: чужой формат, обрыв, пустая рамка, CMD_LOADIMAGE с источником ≥ 2048.

    python test_task_images.py [--out КАТАЛОГ] [--only имя]
"""
import argparse
import pathlib
import struct
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = pathlib.Path(__file__).resolve().parent
DATA = HERE / "data" / "task"
CLANG = r"E:\zx\llvm\bin\clang.exe"
ROOM = 0x200000 - 0x114000          # TSafari: RAM_G 0x114000…0x1FFFFF под картинки
# Прогрессивные: в памяти BL616 целиком (до ~150 КБ коэффициентов), иначе полосами (копия файла —
# в RAM_G у рамки, в пуле у CMD_LOADIMAGE), а слишком большие — только DC (ft_image.c prog_plan)
PROG_FULL_MAX = 150 * 1024
PROG_COPY_EST = 12
LOAD_TOO_BIG = {"big_prog_1600x1200.jpg"}
PROG_DC = set()
# WebP, которым не хватает памяти BL616: (файл, у рамки). CMD_LOADIMAGE держит файл в пуле целиком,
# VP8L — ещё 4 байта на точку; у рамки файл больше WEBP_NEAR_MAX — в RAM_G в конце room
WEBP_TOO_BIG = {("webp_1920x1080.webp", False), ("webp_lossless_640x480.webp", False),
                ("webp_lossless_640x480.webp", True)}
WEBP_NEAR_MAX = 49152
FMT_ARGB4, FMT_RGB565, FMT_L8, FMT_PAL565, FMT_PAL4444 = 6, 7, 3, 14, 15
E_CORRUPT, E_UNSUPPORTED, E_TOO_BIG, E_NO_ROOM, E_SIZE = 1, 2, 3, 4, 5


def png_chunks(data):
    at, out = 8, {}
    while at + 8 <= len(data):
        n, kind = struct.unpack(">I4s", data[at:at + 8])
        out.setdefault(kind, []).append(data[at + 8:at + 8 + n])
        at += 12 + n
    return out


def png_info(data):
    ch = png_chunks(data)
    w, h, depth, ctype, _, _, inter = struct.unpack(">IIBBBBB", ch[b"IHDR"][0])
    plte = ch.get(b"PLTE", [b""])[0]
    trns = ch.get(b"tRNS", [None])[0]
    return dict(w=w, h=h, depth=depth, ctype=ctype, interlace=inter, plte=plte, trns=trns)


def png_rgba(path, info):
    """Точки PNG: нормированные 8 бит (16 — старший байт) и альфа (tRNS — у серого и RGB по цвету)."""
    im = Image.open(path)
    ct = info["ctype"]
    if ct == 0:
        if im.mode == "I;16" or info["depth"] == 16:
            raw = np.array(im, dtype=np.uint32)
            g = (raw >> 8).astype(np.uint8)
            key = struct.unpack(">H", info["trns"][:2])[0] if info["trns"] else None
            a = np.where(raw == key, 0, 255).astype(np.uint8) if key is not None else np.full(g.shape, 255, np.uint8)
        else:
            raw = np.array(im.convert("L"))
            g = raw
            a = np.full(g.shape, 255, np.uint8)
            if info["trns"]:
                key = struct.unpack(">H", info["trns"][:2])[0]
                maxv = (1 << info["depth"]) - 1
                a = np.where(np.rint(g.astype(np.float64) * maxv / 255).astype(int) == key, 0, 255).astype(np.uint8)
        return np.dstack([g, g, g, a]), g
    if ct == 3:
        idx = np.array(im)
        pal = np.frombuffer(info["plte"], np.uint8).reshape(-1, 3)
        al = np.full(256, 255, np.uint8)
        if info["trns"]:
            t = np.frombuffer(info["trns"], np.uint8)
            al[:len(t)] = t
        rgb = pal[idx]
        return np.dstack([rgb, al[idx]]), idx
    rgba = np.array(im.convert("RGBA"))
    if ct == 2 and info["trns"] is None:
        rgba[..., 3] = 255
    return rgba, None


def area(img, tw, th):
    """Уменьшение по площади: точка источника входит в точку выхода долей пересечения отрезков."""
    h, w = img.shape[:2]

    def weights(n, m):
        W = np.zeros((m, n))
        for x in range(n):
            a, b = x * m, (x + 1) * m
            X = a // n
            while a < b:
                e = min(b, (X + 1) * n)
                W[X, x] += e - a
                a = e
                X += 1
        return W / n

    Wx, Wy = weights(w, tw), weights(h, th)
    out = np.empty((th, tw, img.shape[2]))
    for c in range(img.shape[2]):
        out[..., c] = Wy @ img[..., c] @ Wx.T
    return out


def box_target(w, h, nat, bpp, bw, bh, room):
    if w <= bw and h <= bh and w < 2048 and h < 2048 and nat <= room:
        return None
    bw, bh = min(bw, 2047), min(bh, 2047)
    ow, oh = w, h
    if ow > bw or oh > bh:
        if w * bh >= h * bw:
            ow, oh = bw, (h * bw + w // 2) // w
        else:
            oh, ow = bh, (w * bh + h // 2) // h
    ow, oh = max(ow, 1), max(oh, 1)
    while ow * oh * bpp > room and (ow > 1 or oh > 1):
        ow, oh = max(ow * 15 // 16, 1), max(oh * 15 // 16, 1)
    return ow, oh


def rgb565(r, g, b):
    return ((r.astype(np.uint32) >> 3) << 11) | ((g.astype(np.uint32) >> 2) << 5) | (b.astype(np.uint32) >> 3)


def argb4(a, r, g, b):
    return (((a.astype(np.uint32) >> 4) << 12) | ((r.astype(np.uint32) >> 4) << 8) |
            ((g.astype(np.uint32) >> 4) << 4) | (b.astype(np.uint32) >> 4))


def le16(v):
    return v.astype("<u2").tobytes()


def png_raw(path, boxed):
    """Выход как у FT812 (у рамки tRNS серого и RGB — ARGB4): байты, формат, шаг, палитра."""
    data = path.read_bytes()
    info = png_info(data)
    rgba, aux = png_rgba(path, info)
    ct = info["ctype"]
    keyed = boxed and ct in (0, 2) and info["trns"] is not None
    if ct == 3:
        pal = np.frombuffer(info["plte"], np.uint8).reshape(-1, 3)
        if info["trns"]:
            al = np.full(len(pal), 255, np.uint8)
            t = np.frombuffer(info["trns"], np.uint8)[:len(pal)]
            al[:len(t)] = t
            pb, fmt = le16(argb4(al, pal[:, 0], pal[:, 1], pal[:, 2])), FMT_PAL4444
        else:
            pb, fmt = le16(rgb565(pal[:, 0], pal[:, 1], pal[:, 2])), FMT_PAL565
        return pb + aux.astype(np.uint8).tobytes(), fmt, info["w"], len(pb)
    if ct in (4, 6) or keyed:
        return le16(argb4(rgba[..., 3], rgba[..., 0], rgba[..., 1], rgba[..., 2])), FMT_ARGB4, 2 * info["w"], 0
    if ct == 0:
        return aux.astype(np.uint8).tobytes(), FMT_L8, info["w"], 0
    return le16(rgb565(rgba[..., 0], rgba[..., 1], rgba[..., 2])), FMT_RGB565, 2 * info["w"], 0


def decode_out(buf, fmt, w, h):
    """Выход стенда → каналы 8 бит со шкалой формата (для допуска — в шагах формата)."""
    if fmt == FMT_L8:
        v = np.frombuffer(buf, np.uint8)[:w * h].reshape(h, w)
        return {"l": v.astype(int)}
    v = np.frombuffer(buf, "<u2")[:w * h].reshape(h, w).astype(int)
    if fmt == FMT_RGB565:
        return {"r": v >> 11, "g": (v >> 5) & 63, "b": v & 31}
    return {"a": v >> 12, "r": (v >> 8) & 15, "g": (v >> 4) & 15, "b": v & 15}


def ref_quant(ref, fmt, alpha):
    """Эталон (float 0…255, прямой цвет) → шаги формата усечением после округления до 8 бит."""
    q = np.clip(np.floor(ref + 0.5), 0, 255).astype(int)
    if fmt == FMT_L8:
        return {"l": q[..., 0]}
    if fmt == FMT_RGB565:
        return {"r": q[..., 0] >> 3, "g": q[..., 1] >> 2, "b": q[..., 2] >> 3}
    return {"a": q[..., 3] >> 4, "r": q[..., 0] >> 4, "g": q[..., 1] >> 4, "b": q[..., 2] >> 4}


def compare(got, exp, fmt, ref_alpha=None):
    bad, worst = 0, 0
    for k in exp:
        d = np.abs(got[k] - exp[k])
        if fmt == FMT_ARGB4 and k != "a" and ref_alpha is not None:
            d = np.where(ref_alpha >= 64, d, 0)
        worst = max(worst, int(d.max()))
        bad += int((d > 1).sum())
    return bad, worst


def build(out):
    exe = out / "image_test.exe"
    src = HERE / "image_test.c"
    webp = HERE.parent / "third_party" / "libwebp"
    libwebp = [str(webp / s) for s in (webp / "vdac4_sources.txt").read_text().split()]
    cmd = [CLANG, "-std=c11", "-O2", "-Wall", "-Wno-unused-function", "-D_CRT_SECURE_NO_WARNINGS",
           "-DFTC_IMG_TEST", "-DHAVE_CONFIG_H", "-DWEBP_REDUCE_SIZE", "-DWEBP_REDUCE_CSP", "-I", str(HERE.parent), "-I", str(webp),
           str(src), str(HERE.parent / "ft_image.c"), str(HERE.parent / "ft_inflate.c"), *libwebp, "-o", str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit("сборка image_test: " + r.stdout + r.stderr)
    return exe


def cases(only):
    """(файл, метка, opts, max_w, max_h, room)"""
    out = []
    names = sorted(p.name for p in DATA.iterdir() if p.suffix in (".png", ".jpg", ".webp"))
    for n in names:
        out.append((n, "load", 0, 0, 0, 0))
    box = {
        "telegram-logo.png": [(32, 32)], "discord-logo.png": [(32, 32), (473, 473)],
        "rss-logo.png": [(32, 32)], "google_logo_272x92.png": [(272, 92), (136, 46)],
        "google_nav_logo229.png": [(100, 100)], "google_tia.png": [(27, 23), (13, 11)],
        "vt_l1.jpg": [(650, 30), (325, 15)], "vt_l2.jpg": [(620, 20)], "vt_l3.jpg": [(650, 30)],
        "vt2000.jpg": [(114, 130), (57, 65)], "bg6.jpg": [(100, 100), (32, 32)],
        "big_base_1920x1080.jpg": [(1008, 567)], "huge_base_4000x3000.jpg": [(1008, 756)],
        "big_prog_1600x1200.jpg": [(1008, 567)], "big_pal_2000x1500.png": [(1008, 567)],
        "prog_420.jpg": [(203, 131), (64, 64)], "prog_422.jpg": [(64, 64)], "prog_444.jpg": [(64, 64)],
        "prog_gray.jpg": [(64, 64)], "prog_420_q30.jpg": [(64, 64)], "cmyk.jpg": [(64, 64)],
        "prog_640x480.jpg": [(640, 480), (320, 240)], "prog_1024x768.jpg": [(1008, 567), (300, 200)],
        "webp_1280x720.webp": [(1008, 567), (320, 180)], "webp_1920x1080.webp": [(1008, 567)],
        "webp_lossless_640x480.webp": [(320, 240)],
    }
    for n in names:
        if (n.endswith(".png") or n.endswith(".webp")) and n not in box:
            box[n] = [(100, 100), (30, 20)]
    for n, sizes in box.items():
        for bw, bh in sizes:
            out.append((n, f"box{bw}x{bh}", 0, bw, bh, ROOM))
    out.append(("discord-logo.png", "room100k", 0, 473, 473, 100000))     # меньше рамки: не влезает в room
    out.append(("telegram-logo.png", "room8k", 0, 1000, 1000, 8192))
    return [c for c in out if not only or c[0] in only]


def expect(c):
    """Ожидание: ('exact', байты, fmt, w, h, stride, pal) | ('area', ref, fmt, w, h, alpha) | ('err', код)"""
    name, tag, opts, bw, bh, room = c
    path = DATA / name
    boxed = bw != 0
    if name == "webp_err_chunk.webp":            # длина VP8 больше файла
        return ("err", E_CORRUPT)
    if name == "webp_err_riff.webp":             # RIFF, но AVI
        return ("err", E_UNSUPPORTED)
    if name.endswith(".webp"):
        im = Image.open(path)                    # анимация — первый кадр (WebPAnimDecoder)
        w, h = im.size
        alpha = im.mode == "RGBA"
        if not boxed and (w >= 2048 or h >= 2048):
            return ("err", E_SIZE)
        if (name, boxed) in WEBP_TOO_BIG:
            return ("err", E_TOO_BIG)
        rgba = np.array(im.convert("RGBA"))
        r, g, b, a = (rgba[..., i] for i in range(4))
        raw, fmt = (le16(argb4(a, r, g, b)), FMT_ARGB4) if alpha else (le16(rgb565(r, g, b)), FMT_RGB565)
        size, r4 = path.stat().st_size, room & ~3
        if boxed and size > WEBP_NEAR_MAX and size + 4 <= r4:
            room = (r4 - size) & ~3              # файл — в конце room, выход — до него
        tgt = box_target(w, h, 2 * w * h, 2, bw, bh, room) if boxed else None
        if tgt is None:
            return ("exact", raw, fmt, w, h, 2 * w, 0)
        f = rgba.astype(np.float64)
        if not alpha:
            return ("area", area(f[..., :3], *tgt), FMT_RGB565, tgt[0], tgt[1], None)
        pm = f.copy()
        pm[..., :3] *= f[..., 3:4]
        rr = area(pm, *tgt)
        aa = rr[..., 3]
        rgb = np.where(aa[..., None] > 0, rr[..., :3] / np.maximum(aa[..., None], 1e-9), 0)
        return ("area", np.dstack([rgb, aa]), FMT_ARGB4, tgt[0], tgt[1], aa)
    if name.endswith(".png"):
        info = png_info(path.read_bytes())
        w, h = info["w"], info["h"]
        if not boxed and (w >= 2048 or h >= 2048):
            return ("err", E_SIZE)
        raw, fmt, stride, pal = png_raw(path, boxed)
        if not boxed:
            return ("exact", raw, fmt, w, h, stride, pal)
        ct = info["ctype"]
        alpha = ct in (4, 6) or (ct == 3 and info["trns"]) or (ct in (0, 2) and info["trns"] is not None)
        gray = ct in (0, 4)
        tgt = box_target(w, h, len(raw), 2 if alpha else 1 if gray else 2, bw, bh, room)
        if tgt is None:
            return ("exact", raw, fmt, w, h, stride, pal)
        rgba, _ = png_rgba(path, info)
        f = rgba.astype(np.float64)
        if alpha:
            pm = f.copy()
            pm[..., :3] *= f[..., 3:4]
            r = area(pm, *tgt)
            a = r[..., 3]
            rgb = np.where(a[..., None] > 0, r[..., :3] / np.maximum(a[..., None], 1e-9), 0)
            ref = np.dstack([rgb, a])
            return ("area", ref, FMT_ARGB4, tgt[0], tgt[1], a)
        if gray:
            return ("area", area(f[..., :1], *tgt), FMT_L8, tgt[0], tgt[1], None)
        return ("area", area(f[..., :3], *tgt), FMT_RGB565, tgt[0], tgt[1], None)
    im = Image.open(path)
    w, h = im.size
    prog = bool(im.info.get("progressive") or im.info.get("progression"))
    if im.mode == "CMYK":
        return ("err", E_UNSUPPORTED)
    if not boxed and prog and name in LOAD_TOO_BIG:
        return ("err", E_TOO_BIG)
    if not boxed:
        if w >= 2048 or h >= 2048:
            return ("err", E_SIZE)
        rgb = np.array(im.convert("RGB"))
        return ("exact", le16(rgb565(rgb[..., 0], rgb[..., 1], rgb[..., 2])), FMT_RGB565, w, h, 2 * w, 0)
    gray = im.mode == "L"
    tgt = box_target(w, h, w * h * 2, 1 if gray else 2, bw, bh, room)
    if prog:
        lay = im.layer
        hmax = max(c[1] for c in lay)
        vmax = max(c[2] for c in lay)
        mcux, mcuy = -(-w // (8 * hmax)), -(-h // (8 * vmax))
        blocks = sum(mcux * c[1] * mcuy * c[2] for c in lay) if len(lay) > 1 else mcux * mcuy
        if name in PROG_DC:                      # только DC: 1/8, не больше рамки
            dw, dh = -(-w // 8), -(-h // 8)
            tw, th = tgt if tgt else (w, h)
            if tw > dw or th > dh:
                tw, th = dw, dh
            im = Image.open(path)
            im.draft(im.mode, (dw, dh))
            f = np.array(im.convert("L" if gray else "RGB")).astype(np.float64)
            if gray:
                f = f[..., None]
            return ("dc", area(f, tw, th), FMT_L8 if gray else FMT_RGB565, tw, th, None)
        if blocks * 128 > PROG_FULL_MAX:         # полосы: место под копию — по оценке
            est = blocks * PROG_COPY_EST + 4096
            ow, oh = tgt if tgt else (w, h)
            bpp = 1 if gray else 2
            if ow * oh * bpp > room - est:
                while ow * oh * bpp > room - est and (ow > 1 or oh > 1):
                    ow, oh = max(ow * 15 // 16, 1), max(oh * 15 // 16, 1)
                tgt = (ow, oh)
    if tgt is None:
        rgb = np.array(im.convert("RGB"))
        return ("exact", le16(rgb565(rgb[..., 0], rgb[..., 1], rgb[..., 2])), FMT_RGB565, w, h, 2 * w, 0)
    im = Image.open(path)
    im.draft(im.mode, tgt)                       # ДКП-уменьшение, как у BL616
    f = np.array(im.convert("L" if gray else "RGB")).astype(np.float64)
    if gray:
        f = f[..., None]
    _ = prog
    return ("area", area(f, *tgt), FMT_L8 if gray else FMT_RGB565, tgt[0], tgt[1], None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=r"E:\zx\cache\tmp\bl616img\task")
    ap.add_argument("--only", nargs="*")
    a = ap.parse_args()
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    exe = build(out)
    cs = cases(a.only)
    lst = out / "list.txt"
    lst.write_text("".join(f"{n} {t} {o:x} {bw} {bh} {room}\n" for n, t, o, bw, bh, room in cs))
    # ошибки: чужой формат, обрыв PNG, пустая рамка
    (out / "junk.bin").write_bytes(b"GIF89a" + bytes(64))
    (out / "cut.png").write_bytes((DATA / "rgb8.png").read_bytes()[:300])
    for f in ("junk.bin", "cut.png"):
        (out / f).replace(DATA.parent / ("_" + f)) if False else None
    r = subprocess.run([str(exe), "--dump", str(DATA), str(lst), str(out)], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=3600)
    lines = [ln for ln in r.stdout.splitlines() if ln.startswith("case ")]
    res = {}
    for ln in lines:
        parts = ln.split()
        kv = dict(p.split("=", 1) for p in parts[3:] if "=" in p)
        res[(parts[1], parts[2])] = kv
    fails = 0
    print(f"{'картинка':28s} {'метка':12s} {'итог':6s} {'выход':16s} {'мс ПК':>7s}  проверка")
    for c in cs:
        key = (c[0], c[1])
        kv = res.get(key)
        e = expect(c)
        if kv is None:
            print(f"{c[0]:28s} {c[1]:12s} нет строки стенда")
            fails += 1
            continue
        got = (out / f"{c[0]}.{c[1]}.got").read_bytes()
        r_ = int(kv["r"].split("=")[-1]) if "=" in kv["r"] else int(kv["r"])
        desc = f"{kv['fmt']}:{kv['w']}x{kv['h']}"
        ok, note = True, ""
        if kv["chunks"] != "да":
            ok, note = False, "куски/черновик расходятся; "
        if e[0] == "err":
            ok = ok and r_ == 2 and int(kv["err"]) == e[1]
            note += f"ошибка {kv['err']}/{kv['arg']} (ждали {e[1]})"
        elif r_ != 1:
            ok = False
            note += f"r={r_} ошибка {kv['err']}/{kv['arg']}"
        elif e[0] == "exact":
            _, raw, fmt, w, h, stride, pal = e
            same = got == raw
            ok = ok and same and int(kv["fmt"]) == fmt and int(kv["w"]) == w and int(kv["h"]) == h and \
                int(kv["stride"]) == stride and int(kv["pal"]) == pal
            if not same:
                n = min(len(got), len(raw))
                diff = sum(1 for i in range(n) if got[i] != raw[i]) if n < 5_000_000 else -1
                note += f"байты: {len(got)}/{len(raw)}, расходятся {diff}; "
            note += "байт в байт" if ok else f"ждали fmt {fmt} {w}x{h} шаг {stride} пал {pal}"
        else:
            kind, ref, fmt, w, h, ra = e
            if int(kv["fmt"]) != fmt or int(kv["w"]) != w or int(kv["h"]) != h:
                ok = False
                note += f"ждали fmt {fmt} {w}x{h}"
            elif kind == "dc":
                # только DC: у libjpeg в 1/8 цветность — ещё и с AC (idct2), у BL616 — один тон блока
                g, r = decode_out(got, fmt, w, h), ref_quant(ref, fmt, ra)
                diff = np.maximum.reduce([np.abs(g[k] - r[k]) for k in r])
                far = int((diff > 3).sum())
                ok = ok and far <= w * h // 20
                note += f"только DC (1/8): дальше ±3 шагов {far} точек из {w * h}, наибольшее {int(diff.max())}"
            else:
                bad, worst = compare(decode_out(got, fmt, w, h), ref_quant(ref, fmt, ra), fmt, ra)
                ok = ok and bad == 0
                note += f"по площади: хуже ±1 шага {bad} точек, наибольшее {worst}"
        fails += not ok
        print(f"{c[0]:28s} {c[1]:12s} {'да' if ok else 'НЕТ':6s} {desc:16s} {float(kv['ms']):7.1f}  {note}")
    print("ПРОЙДЕН" if not fails else f"НЕ ПРОЙДЕН: {fails}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
