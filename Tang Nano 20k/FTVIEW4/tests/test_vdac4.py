"""Видео ftview на коде сопроцессора BL616 (VDAC4) вместо bt8xxemu.

Модель Z80 плагина (test_ftview.Machine) работает с DLL сопроцессора BL616 (bl616/copro/test/
copro_sim.c проекта VDAC4: ft_copro.c, ft_image.c — тот же код, что в прошивке, плюс модель
регистров ядра FT). Путь к DLL — VDAC4_COPRO_DLL; без него тесты пропускаются. Рисования у
модели нет (это ядро FT), поэтому кадр проверяется в RAM_G по адресу последнего CMD_VIDEOFRAME:
он должен совпасть с декодом libjpeg (Pillow), эталоном JPEG сопроцессора.
"""
import ctypes
import io
import os
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_ftview import Machine  # noqa: E402
from PIL import Image  # noqa: E402
import avigen  # noqa: E402


class Vdac4Chip:
    """Сопроцессор BL616 с памятью FT812: интерфейс, которым Machine ходит к чипу."""

    def __init__(self, dll, per_access=40):
        self.lib = ctypes.CDLL(dll)
        self.lib.sim_init.argtypes = [ctypes.c_int]
        self.lib.sim_write.argtypes = [ctypes.c_uint32, ctypes.c_char_p, ctypes.c_uint32]
        self.lib.sim_read.argtypes = [ctypes.c_uint32, ctypes.c_char_p, ctypes.c_uint32]
        self.lib.sim_poll.argtypes = [ctypes.c_int]
        self.lib.sim_rom.argtypes = [ctypes.c_char_p]
        self.lib.sim_init(per_access)
        # ПЗУ-шрифты для CMD_TEXT/CMD_NUMBER панели (на плате образ заливает ESP)
        rom = Path(os.environ.get('FT812_ROM_DIR', r'E:\zx\ft812_rom')) / 'ft812_rom_1E0000.bin'
        if not self.lib.sim_rom(str(rom).encode()):
            raise unittest.SkipTest('FT812 ROM image absent: ' + str(rom))

    def read(self, addr, n):
        buf = ctypes.create_string_buffer(n)
        self.lib.sim_read(addr, buf, n)
        return buf.raw

    def write(self, addr, data):
        self.lib.sim_write(addr, bytes(data), len(data))

    def rd32(self, addr):
        return int.from_bytes(self.read(addr, 4), 'little')

    @property
    def fault(self):
        return self.lib.sim_fault()


def rgb565(jpeg):
    raw = Image.open(io.BytesIO(jpeg)).convert('RGB').tobytes()
    return b''.join(struct.pack('<H', ((raw[i] >> 3) << 11) | ((raw[i + 1] >> 2) << 5) | (raw[i + 2] >> 3))
                    for i in range(0, len(raw), 3))


def last_videoframe(m):
    """Адрес dst последнего CMD_VIDEOFRAME в потоке кольца плагина."""
    stream = b''.join(d for a, d in m.writes if a == 0x302578)
    at = stream.rfind(struct.pack('<I', 0xFFFFFF41))
    return struct.unpack_from('<I', stream, at + 4)[0]


class EndMachine(Machine):
    """Esc, когда показан последний кадр и плеер встал на паузу (как ControlledMachine)."""

    def api(self):
        key = self.cpu.a
        if 0x10 <= key <= 0x1C:
            frame = self.get('_avi_frame', 4)
            if frame and frame == self.get('_avi_total', 4) and self.get('_avi_paused'):
                self.cpu.memory[0x6004] = 1
            self.cpu.f = 0x40
            self.api_calls.append(key)
            self.ret()
        else:
            super().api()


def photo(n, w, h):
    """Кадр с деталями (не одноцветный): ошибка декода видна побайтно."""
    from PIL import ImageDraw
    img = Image.new('RGB', (w, h), (30 + n * 20 % 200, 60, 120))
    d = ImageDraw.Draw(img)
    for k in range(5):
        d.rectangle((k * w // 7 + n % 9, k * h // 8, k * w // 7 + w // 5, k * h // 8 + h // 4),
                    fill=((k * 50 + n * 9) % 256, (k * 90) % 256, (255 - k * 40) % 256))
    out = io.BytesIO()
    img.save(out, 'JPEG', quality=75, subsampling=1)   # 4:2:2, как у MJPEG ffmpeg
    return out.getvalue()


@unittest.skipUnless(os.environ.get('VDAC4_COPRO_DLL') and Path(os.environ.get('VDAC4_COPRO_DLL', '')).exists(),
                     'VDAC4_COPRO_DLL not set')
class Vdac4VideoTests(unittest.TestCase):
    def chip(self):
        return Vdac4Chip(os.environ['VDAC4_COPRO_DLL'])

    def test_plays_to_last_frame(self):
        # Ролик целиком: плеер доходит до последнего кадра, ошибок сопроцессора нет, в RAM_G —
        # последний кадр побайтно как libjpeg.
        frames = [photo(n, 64, 48) for n in range(24)]
        data = avigen.make_avi(24, audio=False, frame=lambda k: frames[k])[0]
        chip = self.chip()
        m = EndMachine(data, chip=chip)
        m.call('_show_avi', limit=400000)
        self.assertEqual(m.get('_view_failed'), 0, m.get('_fail_at'))
        self.assertEqual(m.get('_avi_frame', 4), 24)
        self.assertEqual(chip.fault, 0)
        dst = last_videoframe(m)
        self.assertEqual(chip.read(dst, 64 * 48 * 2), rgb565(frames[23]))

    def test_ftview_size(self):
        # Размер ftview 512×384. Без звука: звукового блока FT812 (REG_PLAYBACK_*) в ядре FT VDAC4
        # нет, а без GS плагин выводит PCM8 им (отказ 14). Чанки звука в потоке проверяет стенд
        # сопроцессора (bl616/copro/test: ролик audio). Перемотку — он же (seek2): тест перемотки
        # плагина и на bt8xxemu встаёт в avi_probe (0x9b4d, test_chip).
        frames = [photo(n, 512, 384) for n in range(6)]
        data = avigen.make_avi(6, width=512, height=384, audio=False, frame=lambda k: frames[k])[0]
        chip = self.chip()
        m = EndMachine(data, chip=chip)
        m.call('_show_avi', limit=800000)
        self.assertEqual(m.get('_view_failed'), 0, m.get('_fail_at'))
        self.assertEqual(m.get('_avi_frame', 4), 6)
        self.assertEqual(chip.fault, 0)
        self.assertEqual(chip.read(last_videoframe(m), 512 * 384 * 2), rgb565(frames[5]))


if __name__ == '__main__':
    unittest.main()
