"""Real software JPEG decoder and ROM fallback boundary (MSVC + Pillow).

ROM pixels are modeled; progressive pixels use the same jpgd as PLAYER.ELF.
Run: python -m unittest discover -s tests -p test_cover_jpeg.py -v
"""
import json
import pathlib
import subprocess
import tempfile
import unittest

from test_weather_parse import ROOT, build_exe
try:
    from PIL import Image
except ImportError:
    Image = None


@unittest.skipIf(Image is None, 'Pillow is required to generate JPEG fixtures')
class CoverJpegTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.exe = build_exe(ROOT / '.test-build' / 'cover_jpeg', 'cover_jpeg_host.exe',
                            [ROOT / 'tests/stubs_cover', ROOT / 'include'],
                            [(ROOT / 'tests/cover_jpeg_host.cpp', 'host.obj'),
                             (ROOT / 'src/cover_jpeg.cpp', 'jpeg.obj'),
                             (ROOT / 'src/cover_art.cpp', 'art.obj')])
        if cls.exe is None:
            raise unittest.SkipTest('Visual Studio Build Tools not found')

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = pathlib.Path(self.temp.name)

    def jpeg(self, size=(257, 129), mode='RGB', sampling=2, progressive=True):
        image = Image.new('RGB', size)
        image.putdata([((x*7+y*3)%256, (x*2+y*9)%256, (x*11+y)%256)
                       for y in range(size[1]) for x in range(size[0])])
        image = image.convert(mode)
        path = self.directory / 'image.jpg'
        image.save(path, quality=91, subsampling=sampling, progressive=progressive)
        return path

    def decode(self, path, cancel=0, fail=0, rom=8):
        out = self.directory / 'thumb.bin'
        run = subprocess.run([str(self.exe), str(path), str(out), str(cancel), str(fail), str(rom)],
                             capture_output=True, timeout=15)
        self.assertEqual(run.returncode, 0, run.stderr)
        result = json.loads(run.stdout)
        self.assertEqual(result['live_bytes'], 0, result)
        return result, out.read_bytes()

    def test_all_progressive_scans_match_sequential_coefficients(self):
        for size in ((1, 1), (37, 63), (257, 129)):
            for mode, sampling in (('RGB', 0), ('RGB', 1), ('RGB', 2), ('L', 0)):
                with self.subTest(size=size, mode=mode, sampling=sampling):
                    outputs = []
                    for progressive in (False, True):
                        r, pixels = self.decode(self.jpeg(size, mode, sampling, progressive))
                        self.assertEqual(r['result'], 0)
                        self.assertEqual((r['width'], r['height']), size)
                        outputs.append(pixels)
                    self.assertEqual(outputs[0], outputs[1])

    def test_cancel_during_scan_and_output(self):
        path = self.jpeg((700, 700))
        complete, _ = self.decode(path)
        for checkpoint in (1, complete['yields']//2, complete['yields']):
            r, _ = self.decode(path, cancel=checkpoint)
            self.assertEqual(r['result'], 201)
            self.assertTrue(r['cancelled'])

    def test_each_allocation_failure_releases_everything(self):
        path = self.jpeg()
        complete, _ = self.decode(path)
        for allocation in range(1, complete['allocations']+1):
            r, _ = self.decode(path, fail=allocation)
            # Первое — пул TJpgDec во внутренней памяти: нет его — тот же пул в PSRAM (0.1.88+)
            self.assertIn(r['result'], (0, 1, 2) if allocation == 1 else (1, 2))

    def test_only_unsupported_rom_format_enters_software(self):
        path = self.jpeg()
        for rom in (0, 2, 3, 6, 7, 8):
            r, _ = self.decode(path, rom=rom)
            self.assertEqual(r['result'], 0 if rom in (0, 8) else 100+rom)
            self.assertEqual(r['rom_decode'], int(rom == 0))
            if rom == 8:
                self.assertGreater(r['yields'], 0)

    def test_software_memory_and_dimension_limits(self):
        for size in ((5000, 1), (1600, 1600)):
            path = self.directory / 'large.jpg'
            Image.new('RGB', size, (40, 90, 120)).save(path, progressive=True, subsampling=0)
            r, _ = self.decode(path)
            self.assertNotEqual(r['result'], 0)
            self.assertLessEqual(r['peak_bytes'], 4*1024*1024 + 65536)

    def decode_picture(self, path, fail=0):
        out = self.directory / 'thumb.bin'
        big = self.directory / 'picture.bin'
        run = subprocess.run([str(self.exe), str(path), str(out), '0', str(fail), '8', str(big)],
                             capture_output=True, timeout=30)
        self.assertEqual(run.returncode, 0, run.stderr)
        result = json.loads(run.stdout)
        self.assertEqual(result['live_bytes'], 0, result)
        data = big.read_bytes()
        pixels = [int.from_bytes(data[i:i + 2], 'little') for i in range(0, len(data), 2)]
        return result, pixels

    def test_screen_cover_is_an_area_average_of_the_centre(self):
        # Обложка экрана (CoverPicture, ABI 1.5): квадрат из середины, усреднение по площади до
        # 256×256 — как BOX-уменьшение Pillow, с точностью до RGB565 и шума JPEG.
        for size in ((700, 500), (500, 700), (120, 90)):
            with self.subTest(size=size):
                image = Image.new('RGB', size)
                image.putdata([(x * 255 // size[0], y * 255 // size[1], 128)
                               for y in range(size[1]) for x in range(size[0])])
                path = self.directory / 'cover.jpg'
                image.save(path, quality=95, subsampling=0, progressive=True)
                r, pixels = self.decode_picture(path)
                self.assertEqual((r['result'], r['picture']), (0, 1))
                side = min(size)
                left, top = (size[0] - side) // 2, (size[1] - side) // 2
                reference = Image.open(path).convert('RGB').crop(
                    (left, top, left + side, top + side)).resize((256, 256), Image.BOX)
                worst = total = 0
                raw = reference.tobytes()
                for index, p in enumerate(pixels):
                    r8, g8, b8 = raw[3 * index:3 * index + 3]
                    got = ((p >> 11) * 255 // 31, ((p >> 5) & 63) * 255 // 63, (p & 31) * 255 // 31)
                    for a, b in zip(got, (r8, g8, b8)):
                        worst = max(worst, abs(a - b))
                        total += abs(a - b)
                mean = total / (256 * 256 * 3)
                self.assertLess(mean, 4.0, (size, mean))
                self.assertLess(worst, 40, (size, worst))

    def test_screen_cover_without_memory_keeps_the_thumbnail(self):
        path = self.directory / 'cover.jpg'
        Image.new('RGB', (600, 600), (200, 40, 90)).save(path, progressive=True, subsampling=0)
        complete, _ = self.decode_picture(path)
        self.assertEqual((complete['result'], complete['picture']), (0, 1))
        for allocation in range(1, complete['allocations'] + 1):
            with self.subTest(allocation=allocation):
                r, _ = self.decode_picture(path, fail=allocation)
                # Не хватило на обложку экрана — миниатюра всё равно есть; иначе — отказ. Пул
                # TJpgDec (первое выделение) берётся и в PSRAM — тогда всё сводится
                expected = ((0, 0), (1, 0), (2, 0)) + (((0, 1),) if allocation == 1 else ())
                self.assertIn((r['result'], r['picture']), expected)

    def test_invalid_headers(self):
        for data in (b'', b'not a jpeg', b'\xff\xd8\xff\xc2\x00\x11'):
            path = self.directory / 'invalid.jpg'
            path.write_bytes(data)
            r, _ = self.decode(path)
            self.assertNotEqual(r['result'], 0)


if __name__ == '__main__':
    unittest.main()
