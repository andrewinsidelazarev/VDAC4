"""AY/TurboSound/TFC в общем медиаплеере (WMF #11; прежде — отдельный VDAC4TSF.WMF)."""
import unittest
import test_mp3_plugin_z80 as media

EXTENSIONS = 'AS0 ASC FTC GTR PSC PSM PT1 PT2 PT3 SQT ST3 STC STP TS TFC TFD MTC'.split()


class TsfmPluginTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        media.need(media.WMF, media.SYM)

    def test_header_lists_compiled_music(self):
        entries = media.wmf_extensions(media.WMF.read_bytes())
        self.assertEqual([name for name, handler in entries if handler == 7], EXTENSIONS,
                         "все 17 форматов TSFM.ELF — в общем списке, код обработчика 7")

    def test_all_extensions_delivery_end_and_cpu_restore(self):
        for version in (1, 2):
            for extension in EXTENSIONS:
                with self.subTest(version=version, extension=extension):
                    audio = media.AudioEsp(); audio.module_format = 9; audio.length = 4
                    content = bytes(1025)
                    h = media.Mp3Harness(content, name=('Song.' + extension.lower()).encode(), esp=audio)
                    h.link.core.version = version; h.link.core.from_capacity = 16384
                    helper = media.ModuleDeliveryTest()
                    helper.card(h, PLAYER_ELF=helper.elf(1024, 1), TSFM_ELF=helper.elf(2048, 2))
                    h.start(index=2, count=5, extension=7)
                    self.assertEqual([q[0] for q in audio.queries], [b'PLAYER.ELF', b'TSFM.ELF'])
                    self.assertEqual(audio.opens, [bytes([media.OPEN_MODULE]) + len(content).to_bytes(4, 'little') + extension.ljust(3).encode()])
                    self.assertEqual(bytes(audio.received), content)
                    self.assertTrue(audio.finished)
                    self.assertEqual(h.machine.a, media.EXIT_NEXT)
                    self.assertEqual(h.turbo_requests, [2, 0xFF00])


if __name__ == '__main__':
    unittest.main()
