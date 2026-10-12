/* mfm.h — проигрыватель MoonBlaster FM (MFM, MoonSound/OPL4) для OPL.ELF.
 *
 * Перевод Z80-проигрывателя Moonsoft (1995) в порте Mick для ZXM-MoonSound
 * (2015, mfm_player.asm): 18 каналов FM (до шести из них — 4-операторные),
 * голоса — из песни, и 6 дорожек таблиц волн с патчами ПЗУ YRW801. Память
 * проигрывателя — массив с раскладкой Z80 (mfm_image.h): процедуры работают с
 * теми же таблицами каналов и голосов по тем же смещениям, поэтому записи в
 * регистры OPL4 совпадают с оригиналом байт в байт (tests/test_mfm.py).
 * Регистры FM проигрыватель и читает (OPL4 это умеет): здесь — из своей копии
 * записанного.
 *
 * Тик — 60 или 50 Гц (xhzequal, как у MWM: таймер 1 OPL4 на 208 или 248).
 */
#ifndef MFM_H
#define MFM_H

#include <stdint.h>

#include "mfm_image.h"

namespace mfm {

constexpr int MAX_BANKS = 4;
constexpr uint32_t SETTINGS = 812;   /* 718 байт настроек и 94 — название и прочее */

struct Song {
  const uint8_t* info;               /* 812 байт настроек (файл + 6) */
  const uint8_t* positions;
  const uint8_t* patterns;
  uint32_t header;                   /* с этого смещения банка 0 — данные узоров */
  const uint8_t* chunk[MAX_BANKS];
  uint32_t chunk_size[MAX_BANKS];
  int chunks;
  char name[51];

  uint8_t byte(int bank, uint32_t offset) const;
};

/* Разбор: подпись MBMS 1.01, настройки, позиции, узоры. */
bool parse(const uint8_t* file, uint32_t size, Song* song);
/* Отсчётов таймера 1 OPL4 на тик: 208 (60 Гц), 248 (50 Гц) или свой. */
uint32_t timer_count(const Song* song);

/* Куда идут записи: FM (0..0x1FF), волны; wait — пауза Z80 (ожидание загрузки
 * заголовков волн), микросекунды; может быть nullptr. */
struct Output {
  void* context;
  void (*wave)(void* context, uint8_t reg, uint8_t value);
  void (*fm)(void* context, uint16_t reg, uint8_t value);
  void (*wait)(void* context, uint32_t microseconds);
};

class Player {
 public:
  /* MBPlayer_start_music. loop — после конца позиций с места повтора; без
   * него (и у песни без повтора) — стоп. */
  void start(const Song* song, const Output* out, bool loop);
  /* MBPlayer_play_music: false — песня кончилась (стоп записан). */
  bool tick();
  /* MBPlayer_stop_music. */
  void stop();
  bool looped() const { return looped_; }
  uint8_t position() const { return mem_[MFM_POS - MFM_BASE]; }

 private:
  uint8_t& m(uint32_t address);
  uint8_t rd(uint32_t address);
  uint16_t rd16(uint32_t address);
  void fm_select(uint8_t port, uint8_t reg);
  void fm_data(uint8_t value);
  void fm_out(uint8_t port, uint8_t reg, uint8_t value);
  void wave_select(uint8_t reg);
  void wave_data(uint8_t value);
  void wave_out(uint8_t reg, uint8_t value);
  void init_opl4();
  void init_channels();
  void fm_zero(uint32_t iy);
  void read_step();
  void next_position();
  void calc_fm(uint32_t iy, uint8_t count, uint32_t hl);
  void wave_calc(uint32_t iy, uint8_t note);
  void rom_patch(uint32_t iy, uint8_t note, uint8_t patch);
  void wave_freq(uint32_t iy, uint16_t freq);
  void pitch_tick();
  void fm_pitch(uint32_t iy, uint8_t mode);
  void wave_pitch(uint32_t iy, uint8_t mode);
  void play_wtones();
  void event_4op(uint32_t iy, uint8_t a, uint8_t b);
  void event_2op(uint32_t iy, uint8_t a, uint8_t b);
  void event_wave(uint32_t iy, uint8_t a, uint8_t b);
  void note_on_fm(uint32_t iy);
  void note_off_fm(uint32_t iy);
  void instrument_2op(uint32_t iy, uint8_t a, uint8_t b);
  void instrument_4op(uint32_t iy, uint8_t a, uint8_t b);
  void stereo_2op(uint32_t iy, uint8_t a);
  void stereo_4op(uint32_t iy, uint8_t a);
  void link_fm(uint32_t iy, uint8_t a);
  void normalize_down(uint32_t iy);
  void wave_note_on(uint32_t iy, uint8_t b);
  void wave_note_off(uint32_t iy, uint8_t b);
  void wave_damp(uint32_t iy, uint8_t b);
  void wave_change(uint32_t iy, uint8_t number);
  void wave_volume(uint32_t iy, uint8_t b, uint8_t value);
  void wave_stereo(uint32_t iy, uint8_t b, uint8_t value);
  void wave_link(uint32_t iy, uint8_t b, uint8_t a);

  const Song* song_;
  const Output* out_;
  bool loop_;
  bool looped_;
  bool finished_;
  uint8_t mem_[MFM_END - MFM_BASE];
  uint8_t scratch_;
  uint8_t fm_[512];                  /* записанное в регистры FM (IN из C5/C7) */
  uint8_t wave_[256];                /* записанное в регистры волн (IN из 7F) */
  uint16_t fm_latch_;
  uint8_t wave_latch_;
  int bank_;
  uint32_t offset_;                  /* songdata_ptr: банк и смещение */
};

}  // namespace mfm

#endif  // MFM_H
