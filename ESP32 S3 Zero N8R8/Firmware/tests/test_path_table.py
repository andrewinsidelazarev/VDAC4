"""Host-тесты таблицы папок программ Evo (src/path_table.cpp, path.cfg во flash ESP).

Команды PATH_GET/PATH_SET: программа (DOOM VDAC4) по имени и серийному номеру тома FAT32
берёт свою папку; не нашла в ней своих файлов — ищет по карте и записывает найденную.
Модуль собирается MSVC вместе с tests/path_table_host.cpp. Без Visual Studio Build Tools
тесты пропускаются.
"""
import subprocess
import unittest

from test_weather_parse import ROOT, build_exe

BUILD = ROOT / '.test-build' / 'path_table'


class Host:
    def __init__(self, exe):
        self.process = subprocess.Popen([str(exe)], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, text=True)

    def send(self, line):
        self.process.stdin.write(line + '\n')
        self.process.stdin.flush()

    def ask(self, line):
        self.send(line)
        return self.process.stdout.readline().strip()

    def text(self, text):
        self.send('text')
        for row in text.splitlines():
            self.send(row)
        return int(self.ask('.'))

    def dump(self):
        self.send('dump')
        rows = []
        while (row := self.process.stdout.readline().rstrip('\n')) != '.':
            rows.append(row)
        return rows

    def close(self):
        self.process.stdin.close()
        self.process.wait(timeout=10)
        self.process.stdout.close()


class PathTableTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.exe = build_exe(BUILD, 'path_table_host.exe', [ROOT / 'include'],
                            [(ROOT / 'src' / 'path_table.cpp', 'table.obj'),
                             (ROOT / 'tests' / 'path_table_host.cpp', 'host.obj')])
        if cls.exe is None:
            raise unittest.SkipTest('нет Visual Studio Build Tools')

    def setUp(self):
        self.host = Host(self.exe)

    def tearDown(self):
        self.host.close()

    def test_set_get_and_file_round_trip(self):
        h = self.host
        self.assertEqual(h.ask('get DOOM 1A2B3C4D'), 'none')
        self.assertEqual(h.ask('set doom 1A2B3C4D 7'), 'ok')               # регистр имени не важен
        self.assertEqual(h.ask('get DOOM 1A2B3C4D'), '00000007')
        self.assertEqual(h.ask('get Doom 1A2B3C4D'), '00000007')
        self.assertEqual(h.ask('get DOOM 1A2B3C4E'), 'none')               # другая карта
        self.assertEqual(h.ask('set DOOM 1A2B3C4E 1234'), 'ok')
        self.assertEqual(h.ask('set RTYPE 1A2B3C4D FFFFFFF'), 'ok')
        self.assertEqual(h.ask('set DOOM 1A2B3C4D 9'), 'ok')               # обновление — в конец
        rows = h.dump()
        self.assertTrue(rows[0].startswith('; '))
        entries = [r for r in rows if not r.startswith(';')]
        self.assertEqual(entries, ['DOOM         1A2B3C4E 00001234',
                                   'RTYPE        1A2B3C4D 0FFFFFFF',
                                   'DOOM         1A2B3C4D 00000009'])
        # тот же текст, прочитанный заново, — та же таблица
        other = Host(self.exe)
        try:
            self.assertEqual(other.text('\n'.join(rows)), 3)
            self.assertEqual(other.ask('get DOOM 1A2B3C4D'), '00000009')
            self.assertEqual(other.dump(), rows)
        finally:
            other.close()

    def test_bad_names_rejected(self):
        h = self.host
        for name in ['ABCDEFGHIJKLM', 'A;B', '#X', 'Доом']:
            with self.subTest(name=name):
                self.assertEqual(h.ask(f'set {name} 1 2'), 'bad')
        self.assertEqual(h.ask('set ABCDEFGHIJKL 1 2'), 'ok')              # 12 знаков — можно
        self.assertEqual(h.ask('set !~ 1 2'), 'ok')

    def test_parse_skips_comments_and_garbage(self):
        text = '\r\n'.join([
            '; comment', '# comment', '', '   ',
            'DOOM 00000001 00000007',
            'DOOM 00000002',                          # мало полей
            'DOOM 0000000G 00000007',                 # не шестнадцатеричное
            'DOOM 123456789 00000007',                # больше 8 цифр
            'TOOLONGNAME13 1 2',
            '  rtype\t1a2b 0c  extra',                # лишнее поле — строка не годится
            '  rtype\t1a2b 0c',
            'DOOM 00000001 00000008',                 # повтор — последняя побеждает
        ])
        self.assertEqual(self.host.text(text), 2)
        self.assertEqual(self.host.ask('get DOOM 1'), '00000008')
        self.assertEqual(self.host.ask('get RTYPE 1A2B'), '0000000C')

    def test_full_table_drops_oldest(self):
        h = self.host
        for i in range(64):
            self.assertEqual(h.ask(f'set P{i} 1 {i:x}'), 'ok')
        self.assertEqual(h.ask('set P0 1 100'), 'ok')                     # P0 обновлена — теперь новейшая
        self.assertEqual(h.ask('set NEW 1 200'), 'ok')                    # вытесняет давнюю — P1
        self.assertEqual(h.ask('get P1 1'), 'none')
        self.assertEqual(h.ask('get P0 1'), '00000100')
        self.assertEqual(h.ask('get NEW 1'), '00000200')
        self.assertEqual(h.ask('get P2 1'), '00000002')
        rows = [r for r in h.dump() if not r.startswith(';')]
        self.assertEqual(len(rows), 64)
        self.assertEqual(rows[-1], 'NEW          00000001 00000200')


if __name__ == '__main__':
    unittest.main()
