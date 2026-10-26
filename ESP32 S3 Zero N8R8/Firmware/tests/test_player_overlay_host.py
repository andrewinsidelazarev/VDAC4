"""Host-тест экрана проигрывателя оверлеем Tang (src/player_overlay.cpp + отрисовка — дизайн
Codex, Sound/player_ui/player_ui.cpp; ABI приложений 1.5, zm_ui_model_t).

Стенд tests/player_overlay_host.cpp — модель Tang (регион оверлеев, настройка оверлея),
приложения, меню и памяти. Проверяется то, что увидит Tang: оверлей включается только после
того, как весь холст уже в его памяти; после каждого кадра его память совпадает с холстом ESP
точка в точку; появление и уход — по ступени прозрачности за кадр; перезагрузка ПЛИС, ядро без
оверлеев, нехватка памяти и отказ записи — экран выключается и память возвращается. Собирается
MSVC; без Visual Studio Build Tools тест пропускается.
"""
import subprocess
import unittest

from test_weather_parse import ROOT, build_exe

BUILD = ROOT / '.test-build' / 'player_overlay'
PLAYER_UI = ROOT.parent / 'Sound' / 'player_ui'
OFF, FADE_IN, SHOWN, FADE_OUT = 0, 1, 2, 3
CANVAS_WORDS = 320 * 360


class PlayerOverlayTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        exe = build_exe(BUILD, 'player_overlay_host.exe',
                        [ROOT / 'include', ROOT / 'modules' / 'sdk' / 'include', PLAYER_UI],
                        [(ROOT / 'src' / 'player_overlay.cpp', 'overlay.obj'),
                         (PLAYER_UI / 'player_ui.cpp', 'view.obj'),
                         (PLAYER_UI / 'fonts' / 'pui_font.cpp', 'font.obj'),
                         (ROOT / 'tests' / 'player_overlay_host.cpp', 'host.obj')])
        if exe is None:
            raise unittest.SkipTest('нет Visual Studio Build Tools')
        out = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120, check=True)
        cls.r = dict(line.split('=', 1) for line in out.stdout.splitlines() if '=' in line)

    def v(self, key: str) -> int:
        return int(self.r[key])

    def test_hidden_without_the_plugin_window(self):
        self.assertEqual(self.v('hidden_configures'), 0)
        self.assertEqual(self.v('hidden_words'), 0)
        self.assertEqual(self.v('hidden_state'), OFF)

    def test_show_uploads_the_whole_canvas_before_switching_on(self):
        self.assertEqual(self.v('show_state'), FADE_IN)
        self.assertEqual(self.v('show_canvas_before_on'), 1)
        self.assertEqual(self.v('show_words'), CANVAS_WORDS)
        self.assertEqual(self.v('show_yields'), 1, 'меню уходит: холст общий')
        self.assertEqual(self.v('show_cover_calls'), 1)
        self.assertEqual(self.v('show_mismatch'), 0)

    def test_fade_in_one_step_per_frame(self):
        self.assertEqual(self.v('faded_state'), SHOWN)
        self.assertEqual(self.v('faded_alpha'), 15)
        self.assertEqual(self.v('fade_first'), 0)
        self.assertEqual(self.v('fade_steps'), 16)
        self.assertEqual(self.v('fade_rising'), 1)

    def test_frames_keep_the_tang_copy_exact_and_small(self):
        self.assertEqual(self.v('frames_mismatch'), 0)
        self.assertGreaterEqual(self.v('frames_counted'), 200)
        # Спектр и время: в среднем меньше пятой части холста за кадр
        self.assertLess(self.v('frames_mean_words'), CANVAS_WORDS // 5)
        self.assertEqual(self.v('random_mismatch'), 0)
        print('\nслов за кадр: среднее', self.v('frames_mean_words'), 'наибольшее',
              self.v('frames_max_words'), 'случайные полосы', self.v('random_max_words'))
        for key in ('title_mismatch', 'help_mismatch', 'main_mismatch', 'stations_mismatch',
                    'stations_next_mismatch', 'garbage_mismatch'):
            self.assertEqual(self.v(key), 0, key)
        self.assertEqual(self.v('title_cover_calls'), 2, 'обложка — только по смене номера')
        self.assertEqual(self.v('garbage_state'), SHOWN)

    def test_close_fades_out_and_returns_memory(self):
        self.assertEqual(self.v('closed_state'), OFF)
        self.assertEqual(self.v('closed_on'), 0)
        self.assertEqual(self.v('closed_live'), 0)
        self.assertEqual(self.v('close_falling'), 1)
        self.assertEqual(self.v('close_last'), -1, 'в конце — выключен')
        self.assertGreaterEqual(self.v('close_steps'), 15)

    def test_lost_application_hides_the_screen(self):
        self.assertEqual(self.v('again_state'), SHOWN)
        self.assertEqual(self.v('lost_mid_state'), SHOWN, 'меньше секунды — держится')
        self.assertEqual(self.v('lost_state'), OFF)
        self.assertEqual(self.v('lost_on'), 0)

    def test_fpga_reload_uploads_again(self):
        self.assertEqual(self.v('before_reload_state'), SHOWN)
        self.assertEqual(self.v('reload_state'), SHOWN)
        self.assertEqual(self.v('reload_full_words') >= CANVAS_WORDS, True)
        self.assertEqual(self.v('reload_mismatch'), 0)
        self.assertEqual(self.v('reload_canvas_before_on'), 1)

    def test_core_without_overlays(self):
        self.assertEqual(self.v('unsupported_state'), OFF)
        self.assertEqual(self.v('unsupported_live'), 0)

    def test_memory_release_and_hold(self):
        self.assertEqual(self.v('released_state'), OFF)
        self.assertEqual(self.v('released_live'), 0)
        self.assertEqual(self.v('released_on'), 0)
        self.assertEqual(self.v('released_hold_state'), OFF)
        self.assertNotEqual(self.v('released_back_state'), OFF)

    def test_no_memory_retries_once_a_second(self):
        self.assertEqual(self.v('nomem_state'), OFF)
        self.assertEqual(self.v('nomem_configures'), 0)
        self.assertGreaterEqual(self.v('nomem_attempts'), 2)
        self.assertLessEqual(self.v('nomem_attempts'), 4)

    def test_write_failure_switches_off(self):
        self.assertNotEqual(self.v('before_fail_state'), OFF)
        self.assertEqual(self.v('write_fail_state'), OFF)
        self.assertEqual(self.v('write_fail_live'), 0)
        self.assertEqual(self.v('writes_outside'), 0, 'запись — только в холст экрана')
        self.assertEqual(self.v('end_live'), 0)


if __name__ == '__main__':
    unittest.main()
