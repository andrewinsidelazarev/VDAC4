"""Звук при запуске прошивки (0.1.145, 14.11.2026).

Сигнала при включении (три тона: левый канал, правый, оба) больше нет: последний тон на
плате шёл с запинками, хотя ESP подавала его в I2S без разрывов (журнал: «tone: …
gap3558us udf0»), и пользователь велел убрать сигнал совсем. Сеть подключается сразу, как
и до 0.1.144. Печать журнала при запуске с 0.1.143 ждала тишины: её снимок писал во flash, а
запись во flash останавливает кэш обоих ядер дольше запаса I2S. С 0.1.183 журнал живёт в ОЗУ,
снимок flash не трогает, и печать тишины не ждёт. Проверяется исходный текст: сама прошивка на ПК
не собирается.
"""
import pathlib
import unittest

from test_wifi_power_save import function_body

ROOT = pathlib.Path(__file__).resolve().parents[1]


class BootSoundTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        cls.player = (ROOT / "src" / "audio_player.cpp").read_text(encoding="utf-8")
        cls.player_header = (ROOT / "include" / "zifi" / "audio_player.hpp").read_text(
            encoding="utf-8")
        cls.app = (ROOT / "src" / "apps" / "audio_app.cpp").read_text(encoding="utf-8")
        cls.led = (ROOT / "src" / "status_led.cpp").read_text(encoding="utf-8")

    def test_no_boot_chime(self) -> None:
        self.assertIn("bool begin(bool usePsram);", self.player_header)
        begin = function_body(self.player, "bool AudioPlayer::begin(")
        self.assertNotIn("sendTone", begin, "при запуске тонов нет")
        self.assertNotIn("chime", self.player)
        self.assertNotIn("bootChimeWanted", self.app)
        self.assertEqual(self.app.count("player_.begin(host_.psram());"), 2, "оба запуска звука — без сигнала")

    def test_wifi_connects_at_task_start(self) -> None:
        loop = function_body(self.main, "void Application::networkTaskLoop()")
        start = loop.index("for (;;)")
        head, body = loop[:start], loop[start:]
        self.assertRegex(head, r"if \(config_\.ssid\(\)\[0\] != 0\) \{\s*beginSavedWifi\(\);\s*\}")
        self.assertNotIn("beginSavedWifi", body)
        self.assertNotIn("chimePlaying", self.main)
        self.assertLess(head.index("beginSavedWifi"), head.index("networkReady_ = true"))

    def test_stored_log_print_needs_no_silence(self) -> None:
        # 0.1.183: снимок журнала flash не трогает — ждать конца звука незачем
        self.assertRegex(self.led, r"\} else \{(?:\s*//[^\n]*)*\s*bringupPrintStoredLog\(\);")
        self.assertNotIn("sounding()", self.led)


if __name__ == "__main__":
    unittest.main()
