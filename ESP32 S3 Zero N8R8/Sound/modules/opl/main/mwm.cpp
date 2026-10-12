/* mwm.cpp — проигрыватель MoonBlaster Wave (см. mwm.h). Имена в комментариях —
 * метки mwm_player.asm; ch[N] — (IY+N) таблицы каналов. */
#include "mwm.h"

#include <string.h>

#include "mwm_tables.h"

namespace mwm {

namespace {

/* Настройки песни (220 байт). */
constexpr uint32_t XLENG = 0, XLOOP = 1, XWVSTPR = 2, XTEMPO = 26, XHZEQUAL = 27;
constexpr uint32_t XDETUNE = 28, XMODTAB = 52, XBEGWAV = 100, XWAVNRS = 124, XWAVVOLS = 172;
constexpr int SEARCH_LIMIT = 64;  /* поиск части патча: в порядочных данных — до 12 */
constexpr uint32_t LOAD_WAIT_US = 100;  /* ожидание флага LD перед событиями */

uint8_t tab(uint32_t offset) { return offset < MWM_TABLES_SIZE ? mwm_tables[offset] : 0; }
uint16_t tab_word(uint32_t offset) { return (uint16_t)(tab(offset) | (tab(offset + 1) << 8)); }

void copy_name(char* to, const uint8_t* from, int size) {
  int n = 0;
  for (int i = 0; i < size; ++i) {
    const uint8_t c = from[i];
    to[n++] = c >= 0x20 && c < 0x7F ? (char)c : ' ';
  }
  while (n > 0 && to[n - 1] == ' ') {
    --n;
  }
  to[n] = 0;
}

}  // namespace

uint8_t Song::byte(int bank, uint32_t offset) const {
  if (bank == 0) {
    if (offset < header) {
      return info[offset];
    }
    offset -= header;
  }
  if (bank >= chunks || offset >= chunk_size[bank]) {
    return 0;
  }
  return chunk[bank][offset];
}

/* Как mbload: [MBMS 10 08][278 байт настроек][позиции][адреса узоров], потом
 * куски [длина LE16][флаг] до флага 0: первый — в банк 0 сразу за адресами,
 * остальные — каждый в свой банк по 16 КБ. */
bool parse(const uint8_t* file, uint32_t size, Song* song) {
  memset(song, 0, sizeof(*song));
  if (size < 6 + 278 || memcmp(file, "MBMS\x10\x08", 6) != 0) {
    return false;
  }
  song->info = file + 6;
  uint32_t pos = 6 + 278;
  const uint32_t count = song->info[XLENG] + 1u;
  if (pos + count > size) {
    return false;
  }
  song->positions = file + pos;
  pos += count;
  uint32_t highest = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (song->positions[i] > highest) {
      highest = song->positions[i];
    }
  }
  const uint32_t patterns = (highest + 1) * 2;
  if (pos + patterns > size) {
    return false;
  }
  song->patterns = file + pos;
  pos += patterns;
  song->header = 278 + count + patterns;
  while (pos + 3 <= size && song->chunks < MAX_BANKS) {
    uint32_t length = file[pos] | (file[pos + 1] << 8);
    const uint8_t flag = file[pos + 2];
    pos += 3;
    if (flag == 0) {
      break;
    }
    if (length > size - pos) {
      length = size - pos;
    }
    song->chunk[song->chunks] = file + pos;
    song->chunk_size[song->chunks] = length;
    song->chunks++;
    pos += length;
  }
  if (song->chunks == 0) {
    return false;
  }
  copy_name(song->name, song->info + 220, 50);
  copy_name(song->kit, song->info + 270, 8);
  if (strcmp(song->kit, "NONE") == 0) {
    song->kit[0] = 0;
  }
  return true;
}

uint32_t load_kit(const uint8_t* file, uint32_t size, Kit* kit, uint8_t* ram, uint32_t ram_size) {
  constexpr uint32_t HEADERS = TONES_SIZE * 12;
  uint32_t total = 0;
  uint32_t records = 0;
  const uint8_t* tones = nullptr;
  if (file != nullptr) {
    if (size < 74 || memcmp(file, "MBMS\x10\x0d", 6) != 0) {
      return 0;
    }
    total = file[6] | (file[7] << 8) | ((uint32_t)file[8] << 16);
    records = file[9] <= 48 ? file[9] : 48;
    tones = file + 10;
    /* Тоны со звуком: длины должны сойтись с полем длины. */
    uint32_t pos = 74 + 25 * file[9];
    uint32_t sum = 0;
    for (uint32_t k = 0; k < TONES_SIZE; ++k) {
      if ((tones[k] & 1) == 0) {
        continue;
      }
      if (pos + 13 > size) {
        return 0;
      }
      const uint32_t length = file[pos + 11] | (file[pos + 12] << 8);
      pos += 13 + length;
      sum += length;
    }
    if (sum != total || pos > size) {
      return 0;
    }
  }
  const uint32_t image = HEADERS + total + 4;
  if (ram == nullptr) {
    return image;
  }
  if (ram_size < image) {
    return 0;
  }
  memset(kit, 0, sizeof(*kit));
  memset(ram, 0, image);
  const uint32_t silence = 0x200000 + HEADERS + total;
  uint32_t pos = file != nullptr ? 74 + 25 * file[9] : 0;
  uint32_t at = HEADERS;
  for (uint32_t k = 0; k < TONES_SIZE; ++k) {
    uint8_t* h = ram + 12 * k;
    if (tones == nullptr || (tones[k] & 1) == 0) {
      h[0] = (uint8_t)((silence >> 16) & 0x3F);
      h[1] = (uint8_t)(silence >> 8);
      h[2] = (uint8_t)silence;
      h[5] = 0xFF;                            /* конец — через отсчёт: петля по нулю */
      h[6] = 0xFF;
      continue;
    }
    const uint8_t* t = file + pos;
    const uint32_t length = t[11] | (t[12] << 8);
    const uint32_t address = 0x200000 + at;
    h[0] = (uint8_t)((tones[k] & 0xC0) | ((address >> 16) & 0x3F));   /* 8/12/16 бит */
    h[1] = (uint8_t)(address >> 8);
    h[2] = (uint8_t)address;
    memcpy(h + 3, t + 2, 9);                  /* петля, конец, LFO, AR..AM */
    memcpy(ram + at, t + 13, length);
    at += length;
    pos += 13 + length;
  }
  if (file != nullptr) {
    memcpy(kit->tones, tones, TONES_SIZE);
    memcpy(kit->waves, file + 74, 25 * records);
  }
  return image;
}

uint32_t timer_count(const Song* song) {
  const uint8_t hz = song->info[XHZEQUAL];
  return hz == 0 ? 208 : (hz == 1 ? 248 : hz);
}

void Player::wave_out(uint8_t reg, uint8_t value) {
  regs_[reg] = value;
  out_->wave(out_->context, reg, value);
}

uint8_t Player::info(uint32_t index) const { return index < sizeof(info_) ? info_[index] : 0; }

/* MBPlayer_waves, за ним в памяти Z80 — MBPlayer_tones_data. */
uint8_t Player::kit_byte(uint32_t offset) const {
  if (offset < WAVES_SIZE) {
    return kit_->waves[offset];
  }
  offset -= WAVES_SIZE;
  return offset < TONES_SIZE ? kit_->tones[offset] : 0;
}

/* MBPlayer_start_music */
void Player::start(const Song* song, const Kit* kit, const Output* out, bool loop) {
  song_ = song;
  kit_ = kit;
  out_ = out;
  loop_ = loop;
  looped_ = false;
  finished_ = false;
  memset(ch_, 0, sizeof(ch_));
  memset(step_, 0, sizeof(step_));
  memset(regs_, 0, sizeof(regs_));
  for (int t = 0; t < CHANNELS; ++t) {
    ch_[t][1] = (uint8_t)(0x37 - t);          /* регистр частоты канала */
  }
  step_count_ = 15;
  pos_ = 0xFF;
  bank_ = 0;
  offset_ = 0;
  memcpy(info_, song->info, sizeof(info_));
  out_->fm(out_->context, 0x105, 3);          /* MBPlayer_init_opl4: NEW2, NEW */
  wave_out(2, 0x10);                          /* заголовки тонов от 384 — с 0x200000 */
  init_voices();
  speed_ = info_[XTEMPO];
  timer_ = (uint8_t)(speed_ - 3);
  transpose_ = 0;
  out_->fm(out_->context, 0x002, (uint8_t)(0x100 - timer_count(song)));
  out_->fm(out_->context, 0x004, 0x21);       /* таймер 1 пошёл, таймер 2 в маске */
}

/* MBPlayer_init_voices */
void Player::init_voices() {
  for (int t = 0; t < CHANNELS; ++t) {
    uint8_t* ch = ch_[t];
    ch[5] = (uint8_t)(info_[XDETUNE + t] * 2);
    const uint8_t number = info_[XBEGWAV + t];
    wave_change(t, number);
    ch[15] = info(XWAVVOLS - 1 + number);
    volume(t, ch[15]);
    stereo(t, info_[XWVSTPR + t]);
  }
}

/* MBPlayer_stop_music */
void Player::stop() {
  for (int t = 0; t < CHANNELS; ++t) {
    note_off(t);
    damp(t);
  }
}

/* MBPlayer_play_music */
bool Player::tick() {
  if (finished_) {
    return false;
  }
  for (int t = 0; t < CHANNELS; ++t) {        /* MBPlayer_play_pitch */
    if (ch_[t][2] != 0) {
      pitch(t, ch_[t][2]);
    }
  }
  timer_++;
  if (speed_ != timer_) {                     /* MBPlayer_play_int_sec */
    if ((uint8_t)(speed_ - 1) == timer_) {
      second();
    } else if ((uint8_t)(speed_ - 2) == timer_) {
      first();
    }
    return !finished_;
  }
  timer_ = 0;
  for (int t = 16; t < CHANNELS; ++t) {
    if ((uint8_t)(step_[t] - 1) < 96) {
      calc_wave(t, (uint8_t)(step_[t] - 1));
    }
  }
  /* Новые ноты: ключ прочь, частота, тон (заголовок волны грузится), октава. */
  for (int t = 0; t < CHANNELS; ++t) {
    if ((uint8_t)(step_[t] - 1) >= 96) {
      continue;
    }
    uint8_t* ch = ch_[t];
    const uint8_t b = (uint8_t)(CHANNELS - t);
    wave_out((uint8_t)(0x67 + b), 0);
    ch[18] = ch[8];
    wave_out((uint8_t)(0x1F + b), ch[8] | ch[6]);
    wave_out((uint8_t)(0x07 + b), ch[7]);
    ch[19] = ch[9];
    wave_out((uint8_t)(0x37 + b), ch[9]);
  }
  /* Z80 ждёт, пока OPL4 загрузит заголовки волн (флаг LD): за это время
   * микросхема отпускает ключи, и новые ключи начинают огибающую заново. */
  if (out_->wait != nullptr) {
    out_->wait(out_->context, LOAD_WAIT_US);
  }
  /* MBPlayer_play_int_wlus: события каналов. */
  for (int t = 0; t < CHANNELS; ++t) {
    const uint8_t a = step_[t];
    if (a == 0) {
      continue;
    }
    if (a < 97) {
      note_on(t);
    } else if (a == 97) {
      note_off(t);
    } else if (a < 146) {
      wave_change(t, (uint8_t)(a - 0x61));
    } else if (a < 178) {
      volume(t, (uint8_t)((((a - 146) ^ 0x1F) * 2) & 0xFF));
    } else if (a < 193) {
      stereo(t, (uint8_t)(a - 0xB9));
    } else if (a < 212) {
      link(t, a);
    } else if (a < 231) {                     /* MBPlayer_play_wchgpit */
      uint8_t* ch = ch_[t];
      const uint8_t speed = (uint8_t)((a - 221) * 4);
      ch[2] = 1;
      ch[3] = speed;
      ch[4] = (speed & 0x80) != 0 ? 0xFF : 0;
    } else if (a < 238) {                     /* MBPlayer_play_wchgdet */
      ch_[t][5] = (uint8_t)((a - 234) * 4);
    } else if (a < 241) {                     /* MBPlayer_play_wchgmod */
      uint8_t* ch = ch_[t];
      const uint8_t mode = (uint8_t)(a - 0xEC);
      const uint32_t table = XMODTAB - 32 + 16u * mode;
      ch[2] = mode;
      ch[3] = (uint8_t)table;
      ch[4] = (uint8_t)(table >> 8);
    } else if (a < 243) {
      damp(t);
    }
  }
  /* Канал команд. */
  const uint8_t command = step_[CHANNELS];
  if (command != 0) {
    if (command < 24) {
      speed_ = (uint8_t)(25 - command);
    } else if (command == 24) {
      step_count_ = 15;                       /* конец узора */
    } else if (command < 76) {
      transpose_ = (uint8_t)(command - 52);
    }
  }
  return true;
}

/* Второе прерывание: у новых нот — без сдвига высоты; частоты каналов 8..15. */
void Player::second() {
  for (int t = 0; t < CHANNELS; ++t) {
    if ((uint8_t)(step_[t] - 1) < 96) {
      ch_[t][2] = 0;
    }
  }
  for (int t = 8; t < 16; ++t) {
    if ((uint8_t)(step_[t] - 1) < 96) {
      calc_wave(t, (uint8_t)(step_[t] - 1));
    }
  }
}

/* Первое прерывание: следующий шаг (позиция, распаковка), частоты каналов 0..7. */
void Player::first() {
  step_count_ = (uint8_t)((step_count_ + 1) & 15);
  if (step_count_ == 0) {
    uint8_t next = (uint8_t)(pos_ + 1);
    if (!(next < (uint8_t)(info_[XLENG] + 1))) {
      if (loop_ && info_[XLOOP] != 255 && info_[XLOOP] <= info_[XLENG]) {
        next = info_[XLOOP];
        looped_ = true;
      } else {
        stop();
        finished_ = true;
        return;
      }
    }
    pos_ = next;
    const uint8_t pattern = song_->positions[next];
    const uint16_t address = (uint16_t)(song_->patterns[2 * pattern] | (song_->patterns[2 * pattern + 1] << 8));
    bank_ = address >> 14;
    offset_ = address & 0x3FFF;
  }
  const uint8_t head = song_->byte(bank_, offset_++);
  if (head == 0xFF) {
    memset(step_, 0, sizeof(step_));          /* шаг пуст целиком */
  } else {
    step_[0] = head;
    const uint32_t masks = offset_;
    offset_ += 3;
    int k = 1;
    for (int group = 0; group < 3; ++group) {
      uint8_t mask = song_->byte(bank_, masks + group);
      for (int bit = 0; bit < 8; ++bit) {
        step_[k++] = (mask & 0x80) != 0 ? song_->byte(bank_, offset_++) : 0;
        mask = (uint8_t)(mask << 1);
      }
    }
  }
  for (int t = 0; t < 8; ++t) {
    if ((uint8_t)(step_[t] - 1) < 96) {
      calc_wave(t, (uint8_t)(step_[t] - 1));
    }
  }
}

/* MBPlayer_calc_wave: тон, частота и заголовок для ноты note (0..95). */
void Player::calc_wave(int t, uint8_t note) {
  const uint8_t patch = ch_[t][10];
  if (patch == 175) {                         /* MBPlayer_calc_drm: ударные GM */
    if (note < 36) {
      rom_patch(t, note, patch);              /* ниже C4 — как патч */
      return;
    }
    uint8_t* ch = ch_[t];
    const uint8_t drum = note >= 0x57 ? 0x56 : note;
    const uint32_t p = MWM_GMDRM_C4 + 5u * (drum - 36);
    ch[7] = tab(p);
    ch[6] = 0;
    const uint16_t freq = tab_word(p + 1);
    ch[13] = tab(p + 3);
    ch[14] = tab(p + 4);
    finish_freq(t, freq);
    return;
  }
  if (patch >= 176) {
    own_wave(t, note);
    return;
  }
  rom_patch(t, note, patch);
}

/* Патч ПЗУ: части по высоте [граница][тон][тон 9 | нота тона << 1][частоты]. */
void Player::rom_patch(int t, uint8_t note, uint8_t patch) {
  uint8_t* ch = ch_[t];
  uint32_t p = tab_word(MWM_PATCH_TABLE + 2u * patch);
  const uint8_t flags = tab(p);
  ch[13] = tab(p + 1);                        /* байты заголовка */
  ch[14] = tab(p + 2);
  p += 3;
  uint8_t a = note;
  if ((flags & 1) != 0) {
    a = (uint8_t)(a + transpose_);
  }
  uint8_t bound = 0;
  for (int i = 0; i < SEARCH_LIMIT && a >= tab(p); ++i) {
    bound = tab(p);
    p += 5;
  }
  const uint8_t played = a;
  ch[7] = tab(p + 1);
  const uint8_t tone = tab(p + 2);
  ch[6] = tone & 1;
  a = (uint8_t)((tone >> 1) + played - bound);
  ch[0] = a;                                  /* последняя нота */
  const uint16_t table = tab_word(p + 3);
  ch[16] = (uint8_t)table;
  ch[17] = (uint8_t)(table >> 8);
  const uint8_t octave = tab(MWM_TABDIV12 + 2u * a);
  const uint8_t index = tab(MWM_TABDIV12 + 2u * a + 1);
  const uint16_t number = tab_word((uint16_t)(table + index));
  /* sla e; ld a,d; rla; add a,c */
  const uint8_t low = (uint8_t)(number << 1);
  const uint8_t high = (uint8_t)((((number >> 8) << 1) | ((number >> 7) & 1)) + octave);
  finish_freq(t, (uint16_t)((high << 8) | low));
}

/* Своя волна набора: записи по 25 байт [перенос][части: граница, тон, нота]. */
void Player::own_wave(int t, uint8_t note) {
  uint8_t* ch = ch_[t];
  const uint8_t index = (uint8_t)(ch[10] - 176);
  /* Как в Z80: 8 * index восьмибитно (для index от 32 запись уезжает). */
  uint32_t p = 3u * (uint8_t)(index * 8) + index;
  const uint8_t flags = kit_byte(p++);
  uint8_t a = note;
  if ((flags & 1) != 0) {
    a = (uint8_t)(transpose_ + note);
  }
  uint8_t bound = 0;
  for (int i = 0; i < SEARCH_LIMIT && a >= kit_byte(p); ++i) {
    bound = kit_byte(p);
    p += 3;
  }
  const uint8_t played = a;
  const uint8_t tone = kit_byte(p + 1);
  ch[7] = (uint8_t)(tone + 0x80);             /* тоны ОЗУ — от 384 */
  ch[6] = 1;
  a = (uint8_t)(kit_byte(p + 2) + played - bound);
  ch[0] = a;
  ch[13] = 0;
  ch[14] = 0;
  const uint8_t kind = (tone < TONES_SIZE ? kit_->tones[tone] : 0) & 6;
  const uint16_t table = kind == 0 ? MWM_FRQ_AMIGA : (kind == 2 ? MWM_FRQ_441KHZ : MWM_FRQ_TURBO);
  ch[16] = (uint8_t)table;
  ch[17] = (uint8_t)(table >> 8);
  finish_freq(t, tab_word(table + 2u * a));
}

/* loc_0_4704: расстройка (дважды, со знаком), перенос F-номера в октаву. */
void Player::finish_freq(int t, uint16_t freq) {
  uint8_t* ch = ch_[t];
  const uint8_t detune = ch[5];
  uint16_t hl;
  if ((detune & 0x80) != 0) {
    hl = (uint16_t)(0xFF00 | detune);
    hl = (uint16_t)(hl + hl + freq);
  } else {
    hl = (uint16_t)(freq + detune);
    hl = (uint16_t)(hl + (0x0800 | detune));
  }
  hl &= 0xF7FF;
  ch[8] = (uint8_t)hl;
  ch[9] = (uint8_t)(hl >> 8);
}

/* MBPlayer_play_pitch_wdo: сдвиг высоты (mode 1) или модуляция (2..4). */
void Player::pitch(int t, uint8_t mode) {
  uint8_t* ch = ch_[t];
  uint16_t delta;
  if (mode == 1) {
    delta = (uint16_t)(ch[3] | (ch[4] << 8));
  } else {                                    /* MBPlayer_play_mod_wdo */
    uint32_t p = ch[3] | (ch[4] << 8);
    const uint8_t value = info(p);
    delta = (uint16_t)(((value & 0x40) != 0 ? 0xFF00 : 0) | (uint8_t)(value << 2));
    ++p;
    if (info(p) == 10) {
      p = XMODTAB - 16 + 16u * (uint8_t)(mode - 1);
    }
    ch[3] = (uint8_t)p;
    ch[4] = (uint8_t)(p >> 8);
  }
  uint16_t freq = (uint16_t)((ch[18] | (ch[19] << 8)) + delta);
  uint8_t high = (uint8_t)(freq >> 8);
  if ((high & 0x08) != 0) {
    if ((delta & 0x8000) == 0) {
      high = (uint8_t)(high + 8);
    } else {
      high &= 0xF7;
    }
  }
  ch[18] = (uint8_t)freq;
  ch[19] = high;
  wave_out(ch[1], (uint8_t)freq | ch[6]);
  wave_out((uint8_t)(ch[1] + 24), high);
}

/* MBPlayer_play_wonevt: байты заголовка патча, громкость, ключ. */
void Player::note_on(int t) {
  uint8_t* ch = ch_[t];
  const uint8_t c = (uint8_t)(CHANNELS - 1 - t);
  uint32_t p = ch[13] | (ch[14] << 8);
  if (p != 0) {
    wave_out((uint8_t)(0x80 + c), tab(p++));
    for (int i = 0; i < 32; ++i) {
      const uint8_t reg = tab(p);
      if (reg == 0xFF) {
        break;
      }
      wave_out((uint8_t)(reg + c), tab(p + 1));
      p += 2;
    }
  }
  wave_out((uint8_t)(0x50 + c), ch[15] | 1);
  wave_out((uint8_t)(0x68 + c), 0x80 | ch[11]);
}

/* MBPlayer_play_woffevt */
void Player::note_off(int t) {
  const uint8_t reg = (uint8_t)(0x67 + CHANNELS - t);
  ch_[t][2] = 0;
  wave_out(reg, regs_[reg] & 0x7F);
}

/* MBPlayer_play_wchgdmp */
void Player::damp(int t) {
  const uint8_t reg = (uint8_t)(0x67 + CHANNELS - t);
  ch_[t][2] = 0;
  wave_out(reg, regs_[reg] | 0x40);
}

/* MBPlayer_play_wwavevt2: патч и громкость волны number (1..48). */
void Player::wave_change(int t, uint8_t number) {
  uint8_t* ch = ch_[t];
  ch[2] = 0;
  ch[10] = info(XWAVNRS - 1 + number);
  ch[15] = (uint8_t)((info(XWAVVOLS - 1 + number) * 4) | (ch[15] & 1));
}

/* MBPlayer_play_wchgvol2: value * 4 — ослабление (бит 0 — «уровень сразу»). */
void Player::volume(int t, uint8_t value) {
  uint8_t* ch = ch_[t];
  ch[15] = (uint8_t)((ch[15] & 1) | (uint8_t)(value * 4));
  wave_out((uint8_t)(0x4F + CHANNELS - t), ch[15]);
}

/* MBPlayer_play_wchgste2 */
void Player::stereo(int t, uint8_t value) {
  uint8_t* ch = ch_[t];
  const uint8_t pan = value & 0x0F;
  ch[11] = (uint8_t)((ch[11] & 0xF0) | pan);
  const uint8_t reg = (uint8_t)(0x67 + CHANNELS - t);
  ch[2] = 0;
  wave_out(reg, (uint8_t)((regs_[reg] & 0xF0) | pan));
}

/* MBPlayer_play_wlnk: нота сдвигается на event − 202 без нового ключа. */
void Player::link(int t, uint8_t event) {
  uint8_t* ch = ch_[t];
  ch[2] = 0;
  const uint8_t a = (uint8_t)(event - 202 + ch[0]);
  ch[0] = a;
  /* Нот на канале ещё не было — таблицы частот нет (у Z80 указатель 0: читается
   * ПЗУ машины); здесь — нули. */
  const uint16_t table = (uint16_t)(ch[16] | (ch[17] << 8));
  uint16_t freq;
  if ((ch[6] & 1) != 0 && (ch[7] & 0x80) != 0) {
    freq = table != 0 ? tab_word(table + 2u * a) : 0;   /* своя волна */
  } else {
    const uint8_t octave = tab(MWM_TABDIV12 + 2u * a);
    const uint8_t index = tab(MWM_TABDIV12 + 2u * a + 1);
    const uint16_t number = table != 0 ? tab_word((uint16_t)(table + index)) : 0;
    const uint8_t low = (uint8_t)(number << 1);
    const uint8_t high = (uint8_t)((((number >> 8) << 1) | ((number >> 7) & 1)) + octave);
    freq = (uint16_t)((high << 8) | low);
  }
  const uint16_t detune = (uint16_t)(((ch[5] & 0x80) != 0 ? 0xFF00 : 0) | ch[5]);
  freq = (uint16_t)(freq + detune + detune);
  ch[18] = (uint8_t)freq;
  ch[19] = (uint8_t)(freq >> 8);
  const uint8_t b = (uint8_t)(CHANNELS - t);
  wave_out((uint8_t)(0x1F + b), (uint8_t)freq | ch[6]);
  wave_out((uint8_t)(0x37 + b), (uint8_t)(freq >> 8) | ch[12]);
}

}  // namespace mwm
