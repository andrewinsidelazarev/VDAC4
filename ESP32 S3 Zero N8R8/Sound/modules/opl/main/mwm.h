/* mwm.h — проигрыватель MoonBlaster Wave (MWM, MoonSound/OPL4) для OPL.ELF.
 *
 * Перевод Z80-проигрывателя Moonsoft (1995) в порте Mick для ZXM-MoonSound
 * (2015, mwm_player.asm): те же процедуры, та же таблица каналов (20 байт на
 * канал, поля по смещениям IY), те же таблицы патчей ПЗУ (mwm_tables.h). Запись
 * в регистры OPL4 совпадает с оригиналом байт в байт на каждом тике
 * (tests/test_mwm.py гоняет оба на настоящих песнях). Тик — прерывание
 * таймера 1 OPL4 (208, 248 или xhzequal отсчётов по 80,8 мкс).
 *
 * Отличие одно и по выбору: в конце позиций оригинал ZX останавливает песню,
 * а с loop = true она идёт с места повтора, как у MSX (xloop, 255 — нет).
 */
#ifndef MWM_H
#define MWM_H

#include <stdint.h>

namespace mwm {

constexpr int CHANNELS = 24;
constexpr int MAX_BANKS = 4;
constexpr uint32_t WAVES_SIZE = 48 * 25;
constexpr uint32_t TONES_SIZE = 64;

/* Песня в файле (файл должен жить, пока играет). */
struct Song {
  const uint8_t* info;            /* 278 байт настроек (файл + 6) */
  const uint8_t* positions;       /* xleng + 1 позиций */
  const uint8_t* patterns;        /* адреса узоров: [смещение 14 бит | банк 2 бита] */
  uint32_t header;                /* с этого смещения банка 0 — данные узоров */
  const uint8_t* chunk[MAX_BANKS];
  uint32_t chunk_size[MAX_BANKS];
  int chunks;
  char name[51];
  char kit[9];                    /* имя набора волн (MWK), "" — NONE */

  uint8_t byte(int bank, uint32_t offset) const;
};

/* Разбор: подпись MBMS 1.08, настройки, позиции, узоры. */
bool parse(const uint8_t* file, uint32_t size, Song* song);
/* Отсчётов таймера 1 на тик: 208 (60 Гц), 248 (50 Гц) или свой. */
uint32_t timer_count(const Song* song);

/* Набор волн из MWK: записи волн и флаги тонов (как в памяти проигрывателя). */
struct Kit {
  uint8_t waves[WAVES_SIZE];
  uint8_t tones[TONES_SIZE];
};

/* Набор MWK [MBMS 10 0D][длина звука LE24][записей][флаги 64 тонов][записи по 25
 * байт][тоны с флагом 1 по порядку: 13 байт заголовка, звук] — в таблицы kit и
 * в образ ОЗУ OPL4 (с 0x200000): 64 заголовка тонов по 12 байт, звук подряд,
 * в конце 4 нулевых байта — на них заголовки тонов без звука (тишина). ram ==
 * nullptr — только проверка и длина образа. 0 — не MWK или испорчен.
 * file == nullptr — набора нет: образ из одних заголовков тишины. */
uint32_t load_kit(const uint8_t* file, uint32_t size, Kit* kit, uint8_t* ram, uint32_t ram_size);

/* Куда идут записи: регистры волн (порты 7E/7F) и FM (0..0x1FF); wait — пауза
 * Z80 (ожидание загрузки заголовков волн), микросекунды; может быть nullptr. */
struct Output {
  void* context;
  void (*wave)(void* context, uint8_t reg, uint8_t value);
  void (*fm)(void* context, uint16_t reg, uint8_t value);
  void (*wait)(void* context, uint32_t microseconds);
};

class Player {
 public:
  /* MBPlayer_start_music. kit — таблицы набора (нулевые, если набора нет). */
  void start(const Song* song, const Kit* kit, const Output* out, bool loop);
  /* MBPlayer_play_music: false — песня кончилась (стоп уже записан). */
  bool tick();
  /* MBPlayer_stop_music: ключи прочь, демпфер. */
  void stop();
  bool looped() const { return looped_; }
  uint8_t position() const { return pos_; }

 private:
  void wave_out(uint8_t reg, uint8_t value);
  uint8_t info(uint32_t index) const;
  uint8_t kit_byte(uint32_t offset) const;
  void init_voices();
  void second();
  void first();
  void calc_wave(int t, uint8_t note);
  void rom_patch(int t, uint8_t note, uint8_t patch);
  void own_wave(int t, uint8_t note);
  void finish_freq(int t, uint16_t freq);
  void pitch(int t, uint8_t mode);
  void note_on(int t);
  void note_off(int t);
  void wave_change(int t, uint8_t number);
  void volume(int t, uint8_t value);
  void stereo(int t, uint8_t value);
  void link(int t, uint8_t event);
  void damp(int t);

  const Song* song_;
  const Kit* kit_;
  const Output* out_;
  bool loop_;
  bool looped_;
  bool finished_;
  uint8_t info_[220];             /* xleng .. xwavvols */
  uint8_t ch_[CHANNELS][20];      /* play_table_wav: поля по смещениям IY */
  uint8_t step_[CHANNELS + 1];    /* step_buffer: 24 канала и команда */
  uint8_t regs_[256];             /* записанное в регистры волн (IN из 7F) */
  uint8_t pos_, step_count_, speed_, timer_, transpose_;
  int bank_;
  uint32_t offset_;               /* songdata_ptr: банк и смещение */
};

}  // namespace mwm

#endif  // MWM_H
