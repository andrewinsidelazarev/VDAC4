"""Обновлятор всей платы VDAC4 с GitHub (VDAC4 Online Update 2) на стенде ПК.

tools/host_vup собирает НАСТОЯЩИЕ Vdac4Updater, VfsFileStore, VfsBridge, VfsClient и протокол
прошивки. Вместо GitHub — папка с ответами API и файлами репозитория, вместо платы — модель
(раздел ESP, flash Tang, CP_INFO, VERSION, карта Tang), вместо Z80 — эмулятор WC поверх папки
карты Evo. Проверяется (Network/Online Update/DESIGN.md, раздел 7):
- строки всех видов и их порядок: ESP, BL616, слоты 0/1/3, карта Tang, карта Evo (по имени);
- состояние каждой: совпало, отличается, новый, понижение запрещено (OLDER), не прочиталось;
- текущее и доступное: версии ESP, BL616, слотов из README, размеры файлов;
- правила отбора: дискета Amiga и чужие файлы не попадают, '*' не переходит в подкаталог;
- установка в безопасном порядке, ESP последней (этап 7), файлы совпадают байт в байт;
- помехи: испорченное скачивание, сбой записи слота и карты Tang, нет Tang, лимит API, STOP;
- события не длиннее 63 байт.
Сборка стенда: tools/host_vup/build.ps1 (нужны Visual Studio Build Tools).
"""
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
EXE = ROOT / '.test-build' / 'host_vup' / 'host_vup.exe'
COMMIT = '9139a30f2b6e0c1d4a5b6c7d8e9f00112233aabb'
# Стадии — на E:, не в %TEMP% диска C:
STAGE = pathlib.Path(os.environ.get('ZIFI_TEST_STAGE', r'E:\zx\cache\tmp\vup\host_runs'))

SAME, DIFFERENT, NEW, READ_ERROR, UPDATED, FAILED, OLDER = 1, 2, 3, 7, 8, 9, 10
REMOTE, LOCAL, CAN, AUTO, REBOOT = 1, 2, 8, 16, 32
ESP, BL616, SLOT, TANG, EVO = 1, 2, 3, 4, 5

ESP_BIN = 'ESP32 S3 Zero N8R8/Firmware/firmware/vdac4/firmware.bin'
ESP_SUMS = 'ESP32 S3 Zero N8R8/Firmware/firmware/vdac4/firmware.sha256'
BL616_BIN = 'Tang Nano 20k/BL616/firmware/vdac4_copro_bl616.bin'
BL616_MAIN = 'Tang Nano 20k/BL616/app/main.c'
TANG_README = 'Tang Nano 20k/Firmware/README.md'
SLOTS = {0: 'Tang Nano 20k/Firmware/firmware/ts_fb_hdmi_top.bin',
         1: 'Tang Nano 20k/Firmware/firmware/ts_fb_slot1.bin',
         3: 'Tang Nano 20k/Firmware/firmware/ts_fb_video.bin'}
CORES = 'Tang Nano 20k/Cores/SD/CORES/'
NET = 'ESP32 S3 Zero N8R8/Network/'
MODULES = 'ESP32 S3 Zero N8R8/Sound/modules/out/'


def git_sha(data: bytes) -> str:
    return hashlib.sha1(b'blob %d\0' % len(data) + data).hexdigest()


def payload(seed: int, size: int) -> bytes:
    return bytes((seed * 31 + index * 7 + (index >> 8)) & 0xFF for index in range(size))


def bitstream(seed: int, size: int) -> bytes:
    return b'BITS' + payload(seed, size - 4)


def bl616_image(version: int, size: int = 20000) -> bytes:
    return b'BFNP' + bytes([version]) + payload(version, size - 5)


MAIN_C = """/* прошивка BL616 */
#ifdef FTC_BRIDGE
#define FW_VERSION 7
#else
#define FW_VERSION {version}
#endif
"""

README = """| Слот flash | Ядро | Файл | Версия |
|---|---|---|---|
| 0 (0x000000) | **TS-Config** | [`firmware/ts_fb_hdmi_top.fs`](firmware/ts_fb_hdmi_top.fs) | {v0}, выводы |
| 1 (0x100000) | **FT812** | [`firmware/ts_fb_slot1.fs`](firmware/ts_fb_slot1.fs) | 1.45, выводы |
| 3 (0x300000) | **видео** | [`firmware/ts_fb_video.bin`](firmware/ts_fb_video.bin) | 1.43, выводы |
"""


class Stand:
    """Три папки: «GitHub» (repo/ и ответы API), модель платы (tang/) и карта Evo (evo/)."""

    def __init__(self) -> None:
        STAGE.mkdir(parents=True, exist_ok=True)
        self.base = pathlib.Path(tempfile.mkdtemp(prefix='vup_', dir=STAGE))
        self.github = self.base / 'github'
        self.repo = self.github
        self.tang = self.base / 'tang'
        self.evo = self.base / 'evo'
        for folder in (self.github, self.tang / 'sd', self.evo):
            folder.mkdir(parents=True)
        self.board = {'esp': 'vdac4-0.1.123', 'version': '0101012E', 'cpinfo': 'C0290000'}
        self.flash = bytearray(b'\xFF' * 0x400000)
        self.truncated = False

    # --- репозиторий ---
    def remote(self, path: str, data: bytes) -> bytes:
        target = self.repo / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        return data

    def remote_bytes(self, path: str) -> bytes:
        return (self.repo / path).read_bytes()

    def esp_release(self, version: str, image: bytes) -> bytes:
        self.remote(ESP_BIN, image)
        digest = hashlib.sha256(image).hexdigest()
        self.remote(ESP_SUMS, f'VERSION {version}\n{digest}  firmware.bin\n'.encode())
        return image

    def publish(self) -> None:
        tree = []
        for item in sorted(self.repo.rglob('*')):
            relative = item.relative_to(self.repo).as_posix()
            if relative.startswith('_'):
                continue
            if item.is_dir():
                tree.append({'path': relative, 'mode': '040000', 'type': 'tree',
                             'sha': hashlib.sha1(relative.encode()).hexdigest(),
                             'url': 'https://api.github.com/x'})
            else:
                data = item.read_bytes()
                tree.append({'path': relative, 'mode': '100644', 'type': 'blob',
                             'sha': git_sha(data), 'size': len(data),
                             'url': 'https://api.github.com/x'})
        (self.github / '_ref.json').write_text(json.dumps({
            'ref': 'refs/heads/main', 'node_id': 'X', 'url': 'https://api.github.com/x',
            'object': {'sha': COMMIT, 'type': 'commit', 'url': 'https://api.github.com/x'}}),
            encoding='utf-8')
        (self.github / '_tree.json').write_text(json.dumps(
            {'sha': 'f' * 40, 'url': 'https://api.github.com/x', 'tree': tree,
             'truncated': self.truncated}, indent=1), encoding='utf-8')

    # --- плата ---
    def esp_running(self, image: bytes) -> None:
        # раздел приложения длиннее образа: хвост — стёртая flash
        (self.tang / 'esp.bin').write_bytes(image + b'\xFF' * 4096)

    def slot(self, number: int, data: bytes) -> None:
        base = number * 0x100000
        self.flash[base:base + 0x100000] = b'\xFF' * 0x100000
        self.flash[base:base + len(data)] = data

    def slot_bytes(self, number: int, size: int) -> bytes:
        flash = (self.tang / 'flash.bin').read_bytes()
        return flash[number * 0x100000:number * 0x100000 + size]

    def tang_file(self, path: str, data: bytes) -> None:
        target = self.tang / 'sd' / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)

    def evo_file(self, path: str, data: bytes) -> None:
        target = self.evo / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)

    def run(self, mode: str, environment: dict | None = None, evo: bool = True) -> 'Run':
        (self.tang / 'board.ini').write_text(
            ''.join(f'{key}={value}\n' for key, value in self.board.items()), encoding='utf-8')
        (self.tang / 'flash.bin').write_bytes(bytes(self.flash))
        env = {key: value for key, value in os.environ.items() if not key.startswith('ZIFI_')}
        env.update(environment or {})
        result = subprocess.run(
            [str(EXE), str(self.evo) if evo else '-', str(self.github), str(self.tang), mode],
            capture_output=True, timeout=300, env=env)
        return Run(result.stdout.decode('utf-8', 'replace'), result.returncode)

    def cleanup(self) -> None:
        shutil.rmtree(self.base, ignore_errors=True)


class Run:
    def __init__(self, output: str, code: int) -> None:
        self.output = output
        self.code = code
        self.lines = output.splitlines()
        self.rows: dict[str, dict] = {}
        self.order: list[str] = []
        self.states: list[tuple[int, int, int, int, str]] = []
        for line in self.lines:
            if line.startswith('ENTRY '):
                head, current, available, name = line.split('\t')
                _, index, status, flags, kind = head.split(' ')
                if name not in self.rows:
                    self.order.append(name)
                self.rows[name] = {'index': int(index), 'status': int(status),
                                   'flags': int(flags), 'kind': int(kind),
                                   'current': current, 'available': available}
            elif line.startswith('STATE '):
                parts = line.split(' ', 5)
                self.states.append((int(parts[1]), int(parts[2]), int(parts[3]),
                                    int(parts[4]), parts[5] if len(parts) > 5 else ''))

    def row(self, name: str) -> dict:
        return self.rows[name]

    def last_state(self) -> tuple[int, int, int, int, str]:
        return self.states[-1]


def stand_is_stale() -> bool:
    if not EXE.exists():
        return True
    built = EXE.stat().st_mtime
    sources = [ROOT / 'tools' / 'host_vup' / 'main.cpp',
               ROOT / 'tools' / 'host_vup' / 'build.ps1',
               ROOT / 'tools' / 'host_smb' / 'z80_sim.cpp',
               ROOT / 'tools' / 'host_smb' / 'z80_sim.hpp']
    sources += list((ROOT / 'src').glob('*.cpp'))
    sources += list((ROOT / 'include' / 'zifi').glob('*.hpp'))
    return any(path.stat().st_mtime > built for path in sources if path.exists())


VSWHERE = pathlib.Path(r'C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe')

ESP_OLD = payload(1, 300000)
ESP_NEW = payload(2, 310000)


class Vdac4UpdateHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if stand_is_stale():
            env = dict(os.environ, TMP=r'E:\zx\cache\tmp', TEMP=r'E:\zx\cache\tmp')
            script = ROOT / 'tools' / 'host_vup' / 'build.ps1'
            result = subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass',
                                     '-File', str(script)], capture_output=True, cwd=ROOT, env=env)
            if not VSWHERE.exists():
                raise unittest.SkipTest('стенд host_vup не собран (нужны Build Tools)')
            if result.returncode != 0 or not EXE.exists():
                raise RuntimeError('сборка host_vup не удалась:\n' +
                                   result.stdout.decode('utf-8', 'replace') +
                                   result.stderr.decode('utf-8', 'replace'))

    def setUp(self) -> None:
        self.stand = Stand()
        self.addCleanup(self.stand.cleanup)

    def scenario(self) -> None:
        """Плата после прошлых выпусков: всего понемногу."""
        s = self.stand
        s.esp_release('vdac4-0.1.124', ESP_NEW)
        s.esp_running(ESP_OLD)
        s.remote(BL616_BIN, bl616_image(42))
        s.remote(BL616_MAIN, MAIN_C.format(version=42).encode())
        s.remote(TANG_README, README.format(v0='1.46').encode('utf-8'))
        s.slot(0, s.remote(SLOTS[0], bitstream(10, 200000)))       # совпадает
        s.remote(SLOTS[1], bitstream(11, 180000))
        s.slot(1, bitstream(12, 180000))                           # другой
        s.remote(SLOTS[3], bitstream(13, 150000))                  # слот 3 пуст
        s.tang_file('CORES/NES.BIN', s.remote(CORES + 'NES.BIN', payload(20, 90000)))
        s.remote(CORES + 'MD.BIN', payload(21, 80000))
        s.tang_file('CORES/MD.BIN', payload(22, 80000))            # та же длина, другой SHA
        s.remote(CORES + 'AMIGA.ELF', payload(23, 30000))          # новый
        s.remote(CORES + 'sub/X.BIN', payload(24, 100))            # подкаталог — не CORES/*
        s.remote('Tang Nano 20k/Cores/SD/AMIGA/DF0.ADF', payload(25, 901120))  # дискета: не трогать
        s.evo_file('WC/VDAC4UPD.WMF',
                   s.remote(NET + 'Online Update/build/vdac4/VDAC4UPD.WMF', payload(30, 12000)))
        s.remote(NET + 'Cores/build/vdac4/COREMENU.WMF', payload(31, 9000))
        s.evo_file('WC/COREMENU.WMF', payload(32, 8000))           # другая длина
        s.remote(NET + 'VDAC4 VFS/build/VDAC4VFS.WMF', payload(33, 5000))   # новый
        s.remote(NET + 'Cores/build/zifi/ZIFICORE.WMF', payload(34, 100))   # не vdac4 — мимо
        s.evo_file('WC/ESP/PLAYER.ELF', s.remote(MODULES + 'PLAYER.ELF', payload(35, 40000)))
        s.remote(MODULES + 'SIDLEN.BIN', payload(36, 3000))       # новый
        s.remote(MODULES + 'notes.txt', b'not a module')           # мимо
        s.remote('Tang Nano 20k/Cores/VIDEO/esp/dist/VIDEO.ELF', payload(37, 20000))
        s.evo_file('WC/ESP/VIDEO.ELF', payload(37, 19000))
        s.evo_file('WC/OTHER.WMF', b'user plugin')                 # чужой: не трогать
        s.remote('README.md', b'readme')
        s.publish()

    def assert_tang_same(self, card: str, repo: str) -> None:
        self.assertEqual((self.stand.tang / 'sd' / card).read_bytes(), self.stand.remote_bytes(repo))

    def assert_evo_same(self, card: str, repo: str) -> None:
        self.assertEqual((self.stand.evo / card).read_bytes(), self.stand.remote_bytes(repo))

    def assert_no_long_events(self, run: Run) -> None:
        self.assertNotIn('TOOLONG', run.output)
        self.assertNotIn('BADENTRY', run.output)

    # --- сверка ---
    def test_check_lists_every_part(self) -> None:
        self.scenario()
        run = self.stand.run('check')
        self.assertEqual(run.code, 0, run.output)
        self.assert_no_long_events(run)
        self.assertEqual(run.order, [
            'ESP32-S3 firmware', 'BL616 firmware', 'FPGA slot 0 TS-Config',
            'FPGA slot 1 FT812', 'FPGA slot 3 video',
            'Tang SD: CORES/AMIGA.ELF', 'Tang SD: CORES/MD.BIN', 'Tang SD: CORES/NES.BIN',
            'WC/COREMENU.WMF', 'WC/ESP/PLAYER.ELF', 'WC/ESP/SIDLEN.BIN', 'WC/ESP/VIDEO.ELF',
            'WC/VDAC4UPD.WMF', 'WC/VDAC4VFS.WMF'])
        esp = run.row('ESP32-S3 firmware')
        self.assertEqual((esp['status'], esp['kind'], esp['current'], esp['available']),
                         (DIFFERENT, ESP, '0.1.123', '0.1.124'))
        self.assertEqual(esp['flags'], REMOTE | LOCAL | CAN | AUTO | REBOOT)
        bl = run.row('BL616 firmware')
        self.assertEqual((bl['status'], bl['current'], bl['available']), (DIFFERENT, '41', '42'))
        slot0 = run.row('FPGA slot 0 TS-Config')
        self.assertEqual((slot0['status'], slot0['current'], slot0['available']),
                         (SAME, '1.46', '1.46'))
        self.assertFalse(slot0['flags'] & AUTO)
        self.assertEqual(run.row('FPGA slot 1 FT812')['status'], DIFFERENT)
        self.assertEqual(run.row('FPGA slot 1 FT812')['available'], '1.45')
        self.assertEqual(run.row('FPGA slot 3 video')['status'], DIFFERENT)
        self.assertEqual(run.row('Tang SD: CORES/NES.BIN')['status'], SAME)
        self.assertEqual(run.row('Tang SD: CORES/MD.BIN')['status'], DIFFERENT)
        amiga = run.row('Tang SD: CORES/AMIGA.ELF')
        self.assertEqual((amiga['status'], amiga['current'], amiga['available']), (NEW, '-', '30K'))
        self.assertTrue(amiga['flags'] & AUTO)
        self.assertEqual(run.row('WC/VDAC4UPD.WMF')['status'], SAME)
        self.assertEqual(run.row('WC/COREMENU.WMF')['status'], DIFFERENT)
        self.assertEqual(run.row('WC/VDAC4VFS.WMF')['status'], NEW)
        self.assertEqual(run.row('WC/ESP/PLAYER.ELF')['status'], SAME)
        self.assertEqual(run.row('WC/ESP/SIDLEN.BIN')['status'], NEW)
        self.assertEqual(run.row('WC/ESP/VIDEO.ELF')['status'], DIFFERENT)
        phase, can, total, percent, text = run.last_state()
        self.assertEqual((phase, total, percent), (4, 14, 100))
        self.assertEqual(text, '10 to update - mark and press Enter')
        # два запроса API и файлы описей — с того же коммита
        api = [line for line in run.lines if line.startswith('HTTP api.github.com')]
        self.assertEqual(len(api), 2)
        self.assertIn(f'/andrewinsidelazarev/VDAC4/{COMMIT}/ESP32%20S3%20Zero%20N8R8/Firmware/'
                      'firmware/vdac4/firmware.sha256', run.output)

    def test_everything_up_to_date(self) -> None:
        s = self.stand
        s.esp_release('vdac4-0.1.123', ESP_OLD)
        s.esp_running(ESP_OLD)
        s.remote(BL616_BIN, bl616_image(41))
        s.remote(BL616_MAIN, MAIN_C.format(version=41).encode())
        s.remote(TANG_README, README.format(v0='1.46').encode('utf-8'))
        for number, path in SLOTS.items():
            s.slot(number, s.remote(path, bitstream(number, 1000 + number)))
        s.tang_file('CORES/NES.BIN', s.remote(CORES + 'NES.BIN', payload(1, 500)))
        s.evo_file('WC/ESP/PLAYER.ELF', s.remote(MODULES + 'PLAYER.ELF', payload(2, 700)))
        s.publish()
        run = s.run('auto')
        self.assertEqual(run.code, 0, run.output)
        self.assertTrue(all(row['status'] == SAME for row in run.rows.values()), run.output)
        self.assertIn((4, 7, 7, 100, 'Everything is up to date'), run.states)
        self.assertIn('APPLY 0', run.output)
        self.assertNotIn('ESPINSTALL', run.output)

    def test_without_evo_card(self) -> None:
        self.scenario()
        run = self.stand.run('check', evo=False)
        self.assertEqual(run.code, 0, run.output)
        self.assertFalse([name for name in run.order if name.startswith('WC/')])
        self.assertEqual(len(run.order), 8)

    def test_downgrade_is_refused(self) -> None:
        self.scenario()
        self.stand.board['esp'] = 'vdac4-0.1.130'
        self.stand.board['cpinfo'] = 'C02B0000'           # BL616 43, опубликована 42
        run = self.stand.run('apply')
        self.assertEqual(run.code, 0, run.output)
        esp = run.row('ESP32-S3 firmware')
        self.assertEqual(esp['status'], OLDER)
        self.assertFalse(esp['flags'] & CAN)
        self.assertEqual(run.row('BL616 firmware')['status'], OLDER)
        self.assertNotIn('ESPINSTALL', run.output)
        self.assertNotIn('BL616 C0', run.output)

    # --- установка ---
    def test_apply_marked_in_safe_order(self) -> None:
        self.scenario()
        s = self.stand
        run = s.run('auto')
        self.assertEqual(run.code, 0, run.output)
        self.assert_no_long_events(run)
        self.assertIn('APPLY 10', run.output)
        installing = [state[4] for state in run.states if state[0] == 5]
        self.assertEqual(installing, [
            'Installing WC/COREMENU.WMF', 'Installing WC/ESP/SIDLEN.BIN',
            'Installing WC/ESP/VIDEO.ELF', 'Installing WC/VDAC4VFS.WMF',
            'Installing Tang SD: CORES/AMIGA.ELF', 'Installing Tang SD: CORES/MD.BIN',
            'Installing BL616 firmware', 'Installing FPGA slot 1 FT812',
            'Installing FPGA slot 3 video', 'Installing ESP32-S3 firmware'])
        for name in ('WC/COREMENU.WMF', 'WC/ESP/SIDLEN.BIN', 'WC/ESP/VIDEO.ELF', 'WC/VDAC4VFS.WMF',
                     'Tang SD: CORES/AMIGA.ELF', 'Tang SD: CORES/MD.BIN', 'BL616 firmware',
                     'FPGA slot 1 FT812', 'FPGA slot 3 video', 'ESP32-S3 firmware'):
            self.assertEqual(run.row(name)['status'], UPDATED, name)
        self.assertEqual(run.row('BL616 firmware')['current'], '42')
        self.assertEqual(run.row('ESP32-S3 firmware')['current'], '0.1.124')
        # ESP — последней, затем этап 7 с новой версией и перезапуск
        self.assertEqual(run.last_state()[0], 7)
        self.assertEqual(run.last_state()[4], 'vdac4-0.1.124')
        self.assertIn('RESTARTPENDING', run.output)
        steps = [line.split(' ')[0] for line in run.lines
                 if line.split(' ')[0] in ('TANGSD', 'BL616', 'FLASH', 'ESPINSTALL')]
        self.assertEqual(steps, ['TANGSD', 'TANGSD', 'BL616', 'FLASH', 'FLASH', 'ESPINSTALL'])
        self.assertLess(run.output.index('FLASH 100000'), run.output.index('FLASH 300000'))
        # итог на плате и картах — байт в байт
        self.assertEqual((s.tang / 'esp_ota.bin').read_bytes(), ESP_NEW)
        self.assertEqual((s.tang / 'bl616.bin').read_bytes(), s.remote_bytes(BL616_BIN))
        for number in (0, 1, 3):
            data = s.remote_bytes(SLOTS[number])
            self.assertEqual(s.slot_bytes(number, len(data)), data, number)
        self.assert_tang_same('CORES/MD.BIN', CORES + 'MD.BIN')
        self.assert_tang_same('CORES/AMIGA.ELF', CORES + 'AMIGA.ELF')
        self.assertFalse((s.tang / 'sd' / 'CORES' / 'sub').exists())
        self.assertFalse((s.tang / 'sd' / 'AMIGA').exists())
        self.assert_evo_same('WC/COREMENU.WMF', NET + 'Cores/build/vdac4/COREMENU.WMF')
        self.assert_evo_same('WC/VDAC4VFS.WMF', NET + 'VDAC4 VFS/build/VDAC4VFS.WMF')
        self.assert_evo_same('WC/ESP/SIDLEN.BIN', MODULES + 'SIDLEN.BIN')
        self.assert_evo_same('WC/ESP/VIDEO.ELF', 'Tang Nano 20k/Cores/VIDEO/esp/dist/VIDEO.ELF')
        self.assertEqual((s.evo / 'WC' / 'OTHER.WMF').read_bytes(), b'user plugin')
        self.assertFalse(list(s.evo.rglob('WCUPD.*')))

    def test_damaged_download_is_retried(self) -> None:
        self.scenario()
        run = self.stand.run('auto', {'ZIFI_VUP_CORRUPT': 'MD.BIN'})
        self.assertEqual(run.row('Tang SD: CORES/MD.BIN')['status'], UPDATED, run.output)
        self.assert_tang_same('CORES/MD.BIN', CORES + 'MD.BIN')

    def test_damaged_download_keeps_old_file(self) -> None:
        self.scenario()
        old = (self.stand.tang / 'sd' / 'CORES' / 'MD.BIN').read_bytes()
        run = self.stand.run('auto', {'ZIFI_VUP_CORRUPT': 'MD.BIN', 'ZIFI_VUP_CORRUPT_TIMES': '9'})
        self.assertEqual(run.row('Tang SD: CORES/MD.BIN')['status'], FAILED, run.output)
        self.assertEqual((self.stand.tang / 'sd' / 'CORES' / 'MD.BIN').read_bytes(), old)
        # остальные строки продолжаются, ESP — последней
        self.assertEqual(run.row('FPGA slot 1 FT812')['status'], UPDATED)
        self.assertEqual(run.last_state()[0], 7)

    def test_slot_write_failure_is_reported(self) -> None:
        self.scenario()
        self.stand.esp_release('vdac4-0.1.123', ESP_OLD)   # ESP совпадает: итог виден
        self.stand.publish()
        run = self.stand.run('auto', {'ZIFI_VUP_FAIL_SLOT': '1'})
        self.assertEqual(run.row('FPGA slot 1 FT812')['status'], FAILED, run.output)
        self.assertEqual(run.row('FPGA slot 3 video')['status'], UPDATED)
        phase, _, _, _, text = run.last_state()
        self.assertEqual(phase, 4)
        self.assertTrue(text.startswith('FAILED FPGA slot 1 FT812: verify'), text)
        self.assert_no_long_events(run)

    def test_esp_checksum_mismatch_keeps_firmware(self) -> None:
        self.scenario()
        run = self.stand.run('auto', {'ZIFI_VUP_CORRUPT': 'firmware.bin'})
        self.assertEqual(run.row('ESP32-S3 firmware')['status'], FAILED, run.output)
        self.assertNotIn('RESTARTPENDING', run.output)
        self.assertFalse((self.stand.tang / 'esp_ota.bin').exists())
        self.assertEqual(run.last_state()[0], 4)
        self.assertIn('sha256 mismatch', run.last_state()[4])

    def test_tang_sd_write_failure(self) -> None:
        self.scenario()
        run = self.stand.run('auto', {'ZIFI_VUP_FAIL_SD': 'AMIGA'})
        self.assertEqual(run.row('Tang SD: CORES/AMIGA.ELF')['status'], FAILED, run.output)
        self.assertEqual(run.row('Tang SD: CORES/MD.BIN')['status'], UPDATED)

    # --- помехи ---
    def test_no_tang(self) -> None:
        self.scenario()
        self.stand.board['link'] = '0'
        run = self.stand.run('check')
        self.assertEqual(run.code, 0, run.output)
        for name in ('BL616 firmware', 'FPGA slot 0 TS-Config', 'FPGA slot 1 FT812',
                     'FPGA slot 3 video', 'Tang SD: CORES/NES.BIN'):
            self.assertEqual(run.row(name)['status'], READ_ERROR, name)
            self.assertFalse(run.row(name)['flags'] & CAN, name)
        self.assertEqual(run.row('ESP32-S3 firmware')['status'], DIFFERENT)
        self.assertEqual(run.row('WC/COREMENU.WMF')['status'], DIFFERENT)

    def test_tang_runs_other_core(self) -> None:
        self.scenario()
        self.stand.board['version'] = '0201012D'           # работает FT812: моста flash нет
        run = self.stand.run('check')
        self.assertEqual(run.row('FPGA slot 1 FT812')['status'], READ_ERROR, run.output)
        self.assertEqual(run.row('BL616 firmware')['status'], DIFFERENT)

    def test_tang_sd_held_by_wc(self) -> None:
        self.scenario()
        self.stand.board['held'] = '1'
        run = self.stand.run('check')
        self.assertEqual(run.row('Tang SD: CORES/MD.BIN')['status'], READ_ERROR, run.output)
        self.assertEqual(run.row('FPGA slot 1 FT812')['status'], DIFFERENT)

    def test_api_limit(self) -> None:
        self.scenario()
        run = self.stand.run('check', {'ZIFI_VUP_API_STATUS': '403'})
        self.assertEqual(run.code, 1)
        self.assertEqual(run.last_state()[0], 255)
        self.assertIn('API limit', run.last_state()[4])

    def test_truncated_tree_is_refused(self) -> None:
        self.scenario()
        self.stand.truncated = True
        self.stand.publish()
        run = self.stand.run('check')
        self.assertEqual(run.code, 1)
        self.assertEqual(run.last_state(), (255, 0, 0, 0, 'GitHub tree: truncated'))

    def test_stop_during_check(self) -> None:
        self.scenario()
        run = self.stand.run('check', {'ZIFI_VUP_STOP_AFTER': '3'})
        self.assertEqual(run.code, 1, run.output)
        self.assertEqual(run.last_state()[0], 255)
        self.assertEqual(run.last_state()[4], 'stopped')

    def test_sync_resends_list(self) -> None:
        self.scenario()
        run = self.stand.run('sync')
        self.assertEqual(run.code, 0, run.output)
        sync = [index for index, state in enumerate(run.states) if state[0] == 6]
        self.assertEqual(len(sync), 1)
        self.assertEqual(run.states[-1][0], 4)
        self.assertEqual(sum(line.startswith('ENTRY 13 ') for line in run.lines), 2)

    def test_long_names_fit_the_event(self) -> None:
        self.scenario()
        name = 'A_VERY_LONG_PLUGIN_NAME_FOR_THE_TEST.WMF'
        self.stand.remote(NET + 'Long/build/vdac4/' + name, payload(40, 100))
        self.stand.publish()
        run = self.stand.run('check')
        self.assert_no_long_events(run)
        tail = ('WC/' + name)[-30:]
        self.assertIn(tail, run.rows)

    def test_slot_image_not_published(self) -> None:
        # .bin слота ещё не выложен: строки нет, остальное сверяется
        self.scenario()
        (self.stand.repo / SLOTS[0]).unlink()
        self.stand.publish()
        run = self.stand.run('check')
        self.assertEqual(run.code, 0, run.output)
        self.assertNotIn('FPGA slot 0 TS-Config', run.rows)
        self.assertEqual(run.order[2], 'FPGA slot 1 FT812')
        self.assertEqual(run.row('FPGA slot 1 FT812')['status'], DIFFERENT)

    def test_nothing_of_the_board_published(self) -> None:
        self.stand.remote('README.md', b'readme')
        self.stand.publish()
        run = self.stand.run('check')
        self.assertEqual(run.code, 1, run.output)
        self.assertEqual(run.last_state()[0], 255)
        self.assertIn('no VDAC4 board files', run.last_state()[4])

    def test_missing_readme_and_main_c(self) -> None:
        self.scenario()
        (self.stand.repo / TANG_README).unlink()
        (self.stand.repo / BL616_MAIN).unlink()
        self.stand.publish()
        run = self.stand.run('check')
        self.assertEqual(run.code, 0, run.output)
        self.assertEqual(run.row('FPGA slot 1 FT812')['available'], '?')
        self.assertEqual(run.row('FPGA slot 1 FT812')['status'], DIFFERENT)
        self.assertEqual(run.row('BL616 firmware')['status'], READ_ERROR)


if __name__ == '__main__':
    unittest.main()
