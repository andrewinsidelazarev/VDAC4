"""Разбор MIDI и синтезатор SoundFont2 MIDI.ELF (components/tsf: tml.h, tsf.h) на ПК.

tests/midi_lib_host.cpp — исправления аудита MIDI (MIDI.ELF 1.6): слияние дорожек
формата 1, точное время при смене темпа, длинные паузы, отказ на испорченном заголовке
(деление 0, формат 2), дорожка длиннее файла (без выделения памяти), running status,
чужие блоки, нехватка памяти на каждом выделении (без утечек), сообщения песни из памяти —
одним блоком на все события, без realloc и копий дорожек (MIDI.ELF 1.7.3); банк SF2 — регионы при
глобальных зонах, ссылки за массивы, шкала затухания, высота (ScaleTuning, корневая
255), класс исключения, CC121, CC123 с педалью, выбор банка, RPN, возврат CC74 к 64,
глушение голосов при перемотке, тысячи случайных порч банка и конец петли за данными —
без чтения за образом.
Без Visual Studio Build Tools тест пропускается.
"""
import subprocess
import unittest

from sound_paths import ROOT
from test_weather_parse import build_exe

BUILD = ROOT / '.test-build' / 'midi_lib_host'
CHECKS = ['tml_format1_merge_keeps_every_track', 'tml_tempo_changes_do_not_drift',
          'tml_long_delta_is_kept', 'tml_bad_headers_are_refused',
          'tml_huge_track_is_not_allocated', 'tml_running_status_rules',
          'tml_unknown_chunk_is_skipped', 'tml_out_of_memory_is_clean',
          'tml_messages_in_one_block',
          'sf2_global_zone_ranges_count_regions', 'sf2_bad_references', 'sf2_attenuation_scale',
          'sf2_pitch_tuning', 'cc_exclusive_class', 'cc_reset_all_controllers',
          'cc_all_notes_off_keeps_sustain', 'cc_bank_select', 'rpn_tuning',
          'cc74_returns_to_neutral', 'voices_kill', 'sf2_damaged_banks_stay_in_bounds',
          'sf2_loop_end_inside_samples',
          # MIDI.ELF 1.7: модуляторы SF2, огибающая, голоса, педали, NRPN/RPN, SysEx.
          'mod_velocity_curve', 'mod_default_volume', 'mod_bank_overrides', 'mod_preset_adds',
          'sf2_send_generators', 'filter_resonance_gain', 'envelope_stage_carry',
          'envelope_gain_ramp', 'steal_quietest_fades', 'steal_whole_note', 'pedal_sostenuto',
          'pedal_soft_filter', 'nrpn_cutoff', 'rpn5_modulation_depth', 'data_increment',
          'mono_mode', 'sf2_fixed_key', 'sf2_rom_samples', 'missing_preset_is_silent',
          'tml_sysex_events', 'tml_many_tracks_fast', 'sf2_out_of_memory_is_clean']


class MidiLibHostTest(unittest.TestCase):
    def test_checks(self) -> None:
        exe = build_exe(BUILD, 'midi_lib_host.exe', [ROOT / 'components' / 'tsf' / 'include'],
                        [(ROOT / 'tests' / 'midi_lib_host.cpp', 'host.obj')])
        if exe is None:
            self.skipTest('нет MSVC (vcvars64.bat): стенд MIDI пропущен')
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=600)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.split(), [word for name in CHECKS for word in ('ok', name)])


if __name__ == '__main__':
    unittest.main()
