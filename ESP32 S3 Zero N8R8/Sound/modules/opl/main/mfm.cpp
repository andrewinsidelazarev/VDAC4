/* mfm.cpp — проигрыватель MoonBlaster FM (см. mfm.h). Имена в комментариях —
 * метки mfm_player.asm; iy — смещение строки таблицы канала в памяти
 * проигрывателя, m(iy + N) — (IY+N). Строка FM — 13 байт: +0 порт адреса
 * (C4/C6), +1 нота, +2 регистр A0 канала, +3 регистр TL несущей, +4 сдвиг
 * высоты (1) или модуляция (2..4), +5/+6 его скорость или указатель, +7
 * расстройка, +9/+0A Bx и Ax ноты, +0B/+0C записанные Ax и Bx. Строка волны —
 * 18 байт, как у MWM. */
#include "mfm.h"

#include <string.h>

namespace mfm {

namespace {

constexpr uint32_t INFO = 718;              /* в память проигрывателя */
constexpr uint32_t PTW_FM = 13;
constexpr uint32_t PTW_WAVE = 0x12;
constexpr uint32_t LOAD_WAIT_US = 100;      /* ожидание флага LD перед ключами волн */
constexpr int SEARCH_LIMIT = 64;

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

bool parse(const uint8_t* file, uint32_t size, Song* song) {
  memset(song, 0, sizeof(*song));
  if (size < 6 + SETTINGS || memcmp(file, "MBMS\x10\x01", 6) != 0) {
    return false;
  }
  song->info = file + 6;
  uint32_t pos = 6 + SETTINGS;
  const uint32_t count = song->info[0] + 1u;
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
  song->header = SETTINGS + count + patterns;
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
  copy_name(song->name, song->info + INFO, 50);
  return true;
}

uint32_t timer_count(const Song* song) {
  const uint8_t hz = song->info[MFM_XHZEQUAL - MFM_XLENG];
  return hz == 0 ? 208 : (hz == 1 ? 248 : hz);
}

/* --- память и порты ------------------------------------------------------------ */

uint8_t& Player::m(uint32_t address) {
  address &= 0xFFFF;
  if (address >= MFM_BASE && address < MFM_END) {
    return mem_[address - MFM_BASE];
  }
  scratch_ = 0;                             /* вне таблиц: читается 0, запись — мимо */
  return scratch_;
}

uint8_t Player::rd(uint32_t address) { return m(address); }

uint16_t Player::rd16(uint32_t address) { return (uint16_t)(rd(address) | (rd(address + 1) << 8)); }

/* Порт адреса C4 — банк 0, C6 — банк 1; данные у YMF278B — один порт. */
void Player::fm_select(uint8_t port, uint8_t reg) {
  fm_latch_ = (uint16_t)((port == 0xC6 ? 0x100 : 0) | reg);
}

void Player::fm_data(uint8_t value) {
  fm_[fm_latch_] = value;
  out_->fm(out_->context, fm_latch_, value);
}

void Player::fm_out(uint8_t port, uint8_t reg, uint8_t value) {
  fm_select(port, reg);
  fm_data(value);
}

void Player::wave_select(uint8_t reg) { wave_latch_ = reg; }

void Player::wave_data(uint8_t value) {
  wave_[wave_latch_] = value;
  out_->wave(out_->context, wave_latch_, value);
}

void Player::wave_out(uint8_t reg, uint8_t value) {
  wave_select(reg);
  wave_data(value);
}

/* --- начало и конец ------------------------------------------------------------- */

/* MBPlayer_start_music */
void Player::start(const Song* song, const Output* out, bool loop) {
  song_ = song;
  out_ = out;
  loop_ = loop;
  looped_ = false;
  finished_ = false;
  memcpy(mem_, mfm_image, sizeof(mem_));
  memset(fm_, 0, sizeof(fm_));
  memset(wave_, 0, sizeof(wave_));
  fm_latch_ = 0;
  wave_latch_ = 0;
  bank_ = 0;
  offset_ = 0;
  m(MFM_STEP_BUFFER - 2) = 0;               /* MBPlayer_status и следом за ним */
  m(MFM_STEP_BUFFER - 1) = 0;
  m(MFM_STEP_BUFFER) = 0;
  m(MFM_BUSY) = 0xFF;
  m(MFM_TIMER + 1) = 0;
  m(MFM_STEP) = 15;
  m(MFM_POS) = 0xFF;
  for (uint32_t i = 0; i < INFO; ++i) {
    m(MFM_XLENG + i) = song->info[i];
  }
  init_opl4();
  m(MFM_TIMER) = (uint8_t)(m(MFM_SPEED) - 2);
}

/* MBPlayer_init_opl4 */
void Player::init_opl4() {
  fm_out(0xC6, 5, 3);                       /* NEW2, NEW */
  fm_out(0xC4, 8, 0x40);
  fm_out(0xC6, 4, m(MFM_MASK_4OP + m(MFM_COUNT_4OP)));
  wave_out(2, 0x10);
  m(MFM_COUNT_2OP) = (uint8_t)(0x12 - 2 * m(MFM_COUNT_4OP));
  init_channels();
  m(MFM_SPEED) = m(MFM_XTEMPO);
  m(MFM_TRANSPOSE) = 0;
}

/* sub_0_4264: голоса, стерео и расстройка каналов в начале песни. */
void Player::init_channels() {
  uint32_t ix = MFM_XBEGIN;
  const uint8_t count4 = m(MFM_COUNT_4OP);
  if (count4 != 0) {
    uint32_t iy = MFM_TABLE_4OP;
    uint8_t b = count4;
    do {
      m(iy + 7) = m(ix - 0x49);
      instrument_4op(iy, m(ix), b);
      stereo_4op(iy, m(ix - 0x64));
      ++ix;
      iy += PTW_FM;
    } while (--b != 0);
  }
  {
    uint32_t iy = MFM_TABLE_2OP;
    uint8_t b = m(MFM_COUNT_2OP);
    do {
      m(iy + 7) = m(ix - 0x49);
      instrument_2op(iy, m(ix), b);
      stereo_2op(iy, m(ix - 0x64));
      ++ix;
      iy += PTW_FM;
    } while (--b != 0);
  }
  uint32_t iy = MFM_TABLE_WAVE;
  ix = MFM_XBEGIN_WAVE;
  for (uint8_t b = 6; b != 0; --b) {
    m(iy + 5) = (uint8_t)(m(ix - 0x49) * 2);
    const uint8_t number = m(ix);
    wave_change(iy, number);
    m(iy + 0x0F) = m(MFM_XWAVVOLS - 1 + number);
    wave_volume(iy, b, m(iy + 0x0F));
    wave_stereo(iy, b, m(ix - 0x64));
    ++ix;
    iy += PTW_WAVE;
  }
  fm_out(0xC4, 0xBD, m(MFM_XBD));
}

/* sub_0_435E: Ax и Bx канала в ноль. */
void Player::fm_zero(uint32_t iy) {
  uint8_t b = m(iy + 2);
  fm_out(m(iy), b, 0);
  b |= 0x10;
  fm_out(m(iy), b, 0);
}

/* MBPlayer_stop_music */
void Player::stop() {
  m(MFM_BUSY) = 0;
  const uint8_t count4 = m(MFM_COUNT_4OP);
  if (count4 != 0) {
    uint32_t iy = MFM_TABLE_4OP;
    uint8_t b = count4;
    do {
      fm_zero(iy);
      iy += PTW_FM;
    } while (--b != 0);
  }
  {
    uint32_t iy = MFM_TABLE_2OP;
    uint8_t b = m(MFM_COUNT_2OP);
    do {
      fm_zero(iy);
      iy += PTW_FM;
    } while (--b != 0);
  }
  uint32_t iy = MFM_TABLE_WAVE;
  for (uint8_t b = 6; b != 0; --b) {
    wave_note_off(iy, b);
    wave_damp(iy, b);
    iy += PTW_WAVE;
  }
}

/* --- тик ----------------------------------------------------------------------- */

/* MBPlayer_play_music */
bool Player::tick() {
  if (finished_) {
    return false;
  }
  pitch_tick();
  const uint8_t speed = m(MFM_SPEED);
  m(MFM_TIMER)++;
  if (speed != m(MFM_TIMER)) {               /* MBPlayer_play_int_sec */
    if ((uint8_t)(speed - 1) == m(MFM_TIMER)) {
      read_step();
    }
    return !finished_;
  }
  m(MFM_TIMER) = 0;
  play_wtones();
  uint32_t hl = MFM_STEP_BUFFER;
  const uint8_t count4 = m(MFM_COUNT_4OP);
  if (count4 != 0) {
    uint32_t iy = MFM_TABLE_4OP;
    uint8_t b = count4;
    do {
      const uint8_t a = m(hl);
      if (a != 0) {
        event_4op(iy, a, b);
      }
      iy += PTW_FM;
      ++hl;
    } while (--b != 0);
  }
  {
    uint32_t iy = MFM_TABLE_2OP;
    uint8_t b = m(MFM_COUNT_2OP);
    do {
      const uint8_t a = m(hl);
      if (a != 0) {
        event_2op(iy, a, b);
      }
      iy += PTW_FM;
      ++hl;
    } while (--b != 0);
  }
  hl = (uint16_t)(hl + m(MFM_COUNT_4OP));
  if (out_->wait != nullptr) {
    out_->wait(out_->context, LOAD_WAIT_US);  /* Z80 ждёт флага LD */
  }
  uint32_t iy = MFM_TABLE_WAVE;
  for (uint8_t b = 6; b != 0; --b) {
    const uint8_t a = m(hl);
    if (a != 0) {
      event_wave(iy, a, b);
    }
    iy += PTW_WAVE;
    ++hl;
  }
  const uint8_t command = m(hl);
  if (command != 0) {
    if (command < 0x18) {
      m(MFM_SPEED) = (uint8_t)(0x19 - command);
    } else if (command == 0x18) {
      m(MFM_STEP) = 15;                       /* конец узора */
    } else if (command >= 0x1C && command < 0x4C) {
      m(MFM_TRANSPOSE) = (uint8_t)(command - 52);
    }
  }
  return true;
}

/* Шаг: позиция, распаковка, частоты нот FM и волн. */
void Player::read_step() {
  m(MFM_STEP) = (uint8_t)((m(MFM_STEP) + 1) & 15);
  if (m(MFM_STEP) == 0) {
    next_position();
    if (finished_) {
      return;
    }
  }
  const uint8_t head = song_->byte(bank_, offset_++);
  if (head == 0xFF) {
    for (uint32_t i = 0; i < 25; ++i) {
      m(MFM_STEP_BUFFER + i) = 0;
    }
  } else {
    m(MFM_STEP_BUFFER) = head;
    const uint32_t masks = offset_;
    offset_ += 3;
    uint32_t k = 1;
    for (int group = 0; group < 3; ++group) {
      uint8_t mask = song_->byte(bank_, masks + group);
      for (int bit = 0; bit < 8; ++bit) {
        m(MFM_STEP_BUFFER + k++) = (mask & 0x80) != 0 ? song_->byte(bank_, offset_++) : 0;
        mask = (uint8_t)(mask << 1);
      }
    }
  }
  uint32_t hl = MFM_STEP_BUFFER;
  const uint8_t count4 = m(MFM_COUNT_4OP);
  if (count4 != 0) {
    calc_fm(MFM_TABLE_4OP, count4, hl);
    hl += count4;
  }
  const uint8_t count2 = m(MFM_COUNT_2OP);
  calc_fm(MFM_TABLE_2OP, count2, hl);
  hl += count2 != 0 ? count2 : 256;
  hl = (uint16_t)(hl + m(MFM_COUNT_4OP));
  uint32_t iy = MFM_TABLE_WAVE;
  for (uint8_t b = 6; b != 0; --b) {
    const uint8_t a = (uint8_t)(m(hl) - 1);
    if (a < 0x60) {
      wave_calc(iy, a);
    }
    iy += PTW_WAVE;
    ++hl;
  }
}

/* MBPlayer_play_nextpos */
void Player::next_position() {
  uint8_t a = (uint8_t)(m(MFM_POS) + 1);
  if (!(a < (uint8_t)(m(MFM_XLENG) + 1))) {
    a = m(MFM_XLOOP);
    if (a == 255 || !loop_) {
      stop();                                 /* MBPlayer_play_nextstop */
      finished_ = true;
      return;
    }
    looped_ = true;
    if (a > m(MFM_XLENG)) {
      a = 0;
    }
  }
  m(MFM_POS) = a;
  const uint8_t pattern = song_->positions[a];
  m(MFM_PATTERN) = pattern;
  const uint16_t address = (uint16_t)(song_->patterns[2 * pattern] | (song_->patterns[2 * pattern + 1] << 8));
  bank_ = address >> 14;
  offset_ = address & 0x3FFF;
}

/* sub_0_4530: F-номер и блок новых нот FM (с переносом и расстройкой). */
void Player::calc_fm(uint32_t iy, uint8_t count, uint32_t hl) {
  const uint8_t transpose = m(MFM_TRANSPOSE);
  uint8_t b = count;
  do {
    uint8_t a = (uint8_t)(m(hl) - 1);
    if (a < 0x60) {
      m(iy + 4) = 0;
      a = (uint8_t)(a + transpose);
      m(iy + 1) = a;
      const uint32_t p = MFM_FNUM + (uint8_t)(a * 2);
      uint16_t de = (uint16_t)((m(p) << 8) | m(p - 1));
      de = (uint16_t)(de + 1);
      de = (uint16_t)((de & 0xFF00) | (uint8_t)(de + m(iy + 7)));
      de = (uint16_t)(de - 1);
      m(iy + 9) = (uint8_t)(de >> 8);
      m(iy + 0x0A) = (uint8_t)de;
    }
    iy += PTW_FM;
    ++hl;
  } while (--b != 0);
}

/* loc_0_4585: тон, частота и заголовок волны для ноты (0..95). */
void Player::wave_calc(uint32_t iy, uint8_t note) {
  m(iy + 2) = 0;
  const uint8_t patch = m(iy + 0x0A);
  if (patch != 0xAF) {
    rom_patch(iy, note, patch);
    return;
  }
  if (note < 0x24) {                          /* ударные GM ниже C3 — как патч */
    rom_patch(iy, note, patch);
    return;
  }
  const uint8_t drum = note >= 0x53 ? 0x52 : note;
  const uint32_t p = MFM_GMDRM_C4 + (uint8_t)(5 * (drum - 0x24));
  m(iy + 7) = rd(p);
  m(iy + 6) = 0;
  const uint16_t freq = rd16(p + 1);
  m(iy + 0x0D) = rd(p + 3);
  m(iy + 0x0E) = rd(p + 4);
  wave_freq(iy, freq);
}

/* loc_0_4599: патч ПЗУ — части по высоте. */
void Player::rom_patch(uint32_t iy, uint8_t note, uint8_t patch) {
  uint32_t p = rd16(MFM_PATCH_TABLE + 2u * patch);
  const uint8_t flags = rd(p);
  m(iy + 0x0D) = rd(p + 1);
  m(iy + 0x0E) = rd(p + 2);
  p += 3;
  uint8_t a = note;
  if ((flags & 1) != 0) {
    a = (uint8_t)(note + m(MFM_TRANSPOSE));
  }
  uint8_t bound = 0;
  for (int i = 0; i < SEARCH_LIMIT && a >= rd(p); ++i) {
    bound = rd(p);
    p += 5;
  }
  const uint8_t played = a;
  m(iy + 7) = rd(p + 1);
  const uint8_t tone = rd(p + 2);
  m(iy + 6) = tone & 1;
  a = (uint8_t)((tone >> 1) + played - bound);
  m(iy) = a;
  const uint16_t table = rd16(p + 3);
  m(iy + 0x10) = (uint8_t)table;
  m(iy + 0x11) = (uint8_t)(table >> 8);
  const uint8_t octave = rd(MFM_TABDIV12 + 2u * a);
  const uint8_t index = rd(MFM_TABDIV12 + 2u * a + 1);
  const uint16_t number = rd16((uint16_t)(table + index));
  const uint8_t low = (uint8_t)(number << 1);
  const uint8_t high = (uint8_t)((((number >> 8) << 1) | ((number >> 7) & 1)) + octave);
  wave_freq(iy, (uint16_t)((high << 8) | low));
}

/* loc_0_460A: расстройка дважды со знаком. */
void Player::wave_freq(uint32_t iy, uint16_t freq) {
  const uint8_t detune = m(iy + 5);
  uint16_t hl = (uint16_t)(((detune & 0x80) != 0 ? 0xFF00 : 0) | detune);
  hl = (uint16_t)(hl + hl + freq);
  m(iy + 8) = (uint8_t)hl;
  m(iy + 9) = (uint8_t)(hl >> 8);
}

/* sub_0_4A9A: сдвиги высоты и модуляция каждого тика. */
void Player::pitch_tick() {
  const uint8_t count4 = m(MFM_COUNT_4OP);
  if (count4 != 0) {
    uint32_t iy = MFM_TABLE_4OP;
    uint8_t b = count4;
    do {
      if (m(iy + 4) != 0) {
        fm_pitch(iy, m(iy + 4));
      }
      iy += PTW_FM;
    } while (--b != 0);
  }
  {
    uint32_t iy = MFM_TABLE_2OP;
    uint8_t b = m(MFM_COUNT_2OP);
    do {
      if (m(iy + 4) != 0) {
        fm_pitch(iy, m(iy + 4));
      }
      iy += PTW_FM;
    } while (--b != 0);
  }
  uint32_t iy = MFM_TABLE_WAVE;
  for (uint8_t b = 6; b != 0; --b) {
    if (m(iy + 2) != 0) {
      wave_pitch(iy, m(iy + 2));
    }
    iy += PTW_WAVE;
  }
}

/* loc_0_4AE5: FM — сдвиг (1) или модуляция; F-номер ниже 512 — блок вниз. */
void Player::fm_pitch(uint32_t iy, uint8_t mode) {
  uint16_t hl = (uint16_t)(m(iy + 5) | (m(iy + 6) << 8));
  if (mode != 1) {                            /* loc_0_4B25 */
    const uint8_t c = (uint8_t)(mode - 1);
    uint8_t e = rd(hl);
    const uint8_t d = (e & 0x80) != 0 ? 0xFF : 0;
    e = (uint8_t)(e << 1);
    ++hl;
    if (rd(hl) == 0x0A) {
      hl = (uint16_t)(MFM_XMODTAB_FM + (uint8_t)(c * 16));
    }
    m(iy + 5) = (uint8_t)hl;
    m(iy + 6) = (uint8_t)(hl >> 8);
    hl = (uint16_t)((d << 8) | e);
  }
  const uint16_t de = (uint16_t)(m(iy + 0x0B) | (m(iy + 0x0C) << 8));
  const bool down = (hl & 0x8000) != 0;
  hl = (uint16_t)(hl + de);
  uint8_t h = (uint8_t)(hl >> 8);
  const uint8_t l = (uint8_t)hl;
  if (!down) {
    h = (uint8_t)(((de >> 8) & 2) | h);
  } else if ((h & 2) == 0) {
    h = (uint8_t)(h - 2);
  }
  const uint8_t port = m(iy);
  uint8_t reg = m(iy + 2);
  fm_select(port, reg);
  m(iy + 0x0B) = l;
  fm_data(l);
  reg |= 0x10;
  fm_select(port, reg);
  m(iy + 0x0C) = h;
  fm_data(h);
}

/* loc_0_4B4C: волна — как у MWM. */
void Player::wave_pitch(uint32_t iy, uint8_t mode) {
  uint16_t hl = (uint16_t)(m(iy + 3) | (m(iy + 4) << 8));
  uint16_t delta;
  if (mode == 1) {
    delta = hl;
  } else {
    const uint8_t c = (uint8_t)(mode - 1);
    const uint8_t value = rd(hl);
    delta = (uint16_t)(((value & 0x40) != 0 ? 0xFF00 : 0) | (uint8_t)(value << 2));
    ++hl;
    if (rd(hl) == 0x0A) {
      hl = (uint16_t)(MFM_XMODTAB_FM + (uint8_t)(c * 16));
    }
    m(iy + 3) = (uint8_t)hl;
    m(iy + 4) = (uint8_t)(hl >> 8);
  }
  const uint16_t freq = (uint16_t)((m(iy + 8) | (m(iy + 9) << 8)) + delta);
  uint8_t high = (uint8_t)(freq >> 8);
  if ((high & 0x08) != 0) {
    if ((delta & 0x8000) == 0) {
      high = (uint8_t)(high + 8);
    } else {
      high &= 0xF7;
    }
  }
  m(iy + 8) = (uint8_t)freq;
  m(iy + 9) = high;
  const uint8_t reg = m(iy + 1);
  wave_out(reg, (uint8_t)freq | m(iy + 6));
  wave_out((uint8_t)(reg + 0x18), high);
}

/* MBPlayer_play_wtones: новые ноты волн — ключ прочь, тишина, частота, тон, октава. */
void Player::play_wtones() {
  uint32_t hl = MFM_STEP_WAVE;
  uint32_t iy = MFM_TABLE_WAVE;
  for (uint8_t b = 6; b != 0; --b) {
    if ((uint8_t)(m(hl) - 1) < 0x60) {
      wave_out((uint8_t)(0x67 + b), 0);
      wave_out((uint8_t)(0x4F + b), 0xFF);
      wave_out((uint8_t)(0x1F + b), m(iy + 8) | m(iy + 6));
      wave_out((uint8_t)(0x07 + b), m(iy + 7));
      wave_out((uint8_t)(0x37 + b), m(iy + 9));
    }
    ++hl;
    iy += PTW_WAVE;
  }
}

/* --- события FM ------------------------------------------------------------------ */

void Player::event_4op(uint32_t iy, uint8_t a, uint8_t b) {
  if (a < 97) {
    note_on_fm(iy);
  } else if (a == 97) {
    note_off_fm(iy);
  } else if (a < 122) {
    instrument_4op(iy, (uint8_t)(a - 0x61), b);
  } else if (a < 186) {
    /* громкость 4-операторного: loc_0_476C — ничего */
  } else if (a < 189) {
    m(iy + 4) = 0;
    stereo_4op(iy, (uint8_t)(a - 0xB9));
  } else if (a < 208) {
    link_fm(iy, a);
  } else if (a < 227) {                       /* loc_0_47F5: сдвиг высоты */
    const uint8_t speed = (uint8_t)(a - 0xD9);
    m(iy + 4) = 1;
    m(iy + 5) = (uint8_t)(speed << 1);
    if ((speed & 0x80) == 0) {
      m(iy + 6) = 0;
    } else {
      m(iy + 6) = 0xFF;
      normalize_down(iy);
    }
  } else if (a < 240) {
    /* яркость 4-операторного: loc_0_4849 — ничего */
  } else if (a < 247) {
    m(iy + 7) = (uint8_t)((a - 0xF3) * 2);    /* loc_0_484A: расстройка */
  } else if (a < 250) {                       /* loc_0_4851: модуляция */
    const uint8_t mode = (uint8_t)(a - 0xF5);
    m(iy + 4) = mode;
    const uint16_t p = (uint16_t)(MFM_XMODTAB_FM2 + (uint8_t)((mode - 2) * 16));
    m(iy + 5) = (uint8_t)p;
    m(iy + 6) = (uint8_t)(p >> 8);
    normalize_down(iy);
  }
}

void Player::event_2op(uint32_t iy, uint8_t a, uint8_t b) {
  if (a < 97) {
    note_on_fm(iy);
  } else if (a == 97) {
    note_off_fm(iy);
  } else if (a < 122) {
    instrument_2op(iy, (uint8_t)(a - 0x61), b);
  } else if (a < 186) {                       /* loc_0_4758: громкость */
    const uint8_t level = (uint8_t)(a - 0x7A);
    fm_select(m(iy), m(iy + 3));
    fm_data((uint8_t)((fm_[fm_latch_] & 0xC0) + level));
  } else if (a < 189) {
    m(iy + 4) = 0;
    stereo_2op(iy, (uint8_t)(a - 0xB9));
  } else if (a < 208) {
    link_fm(iy, a);
  } else if (a < 227) {
    const uint8_t speed = (uint8_t)(a - 0xD9);
    m(iy + 4) = 1;
    m(iy + 5) = (uint8_t)(speed << 1);
    if ((speed & 0x80) == 0) {
      m(iy + 6) = 0;
    } else {
      m(iy + 6) = 0xFF;
      normalize_down(iy);
    }
  } else if (a < 240) {                       /* loc_0_482D: яркость (TL модулятора) */
    const uint8_t step = (uint8_t)(a - 0xE9);
    fm_select(m(iy), (uint8_t)(m(iy + 3) - 3));
    const uint8_t e = fm_[fm_latch_];
    fm_data((uint8_t)((e & 0x3F) + step + (e & 0xC0)));
  } else if (a < 247) {
    m(iy + 7) = (uint8_t)((a - 0xF3) * 2);
  } else if (a < 250) {
    const uint8_t mode = (uint8_t)(a - 0xF5);
    m(iy + 4) = mode;
    const uint16_t p = (uint16_t)(MFM_XMODTAB_FM2 + (uint8_t)((mode - 2) * 16));
    m(iy + 5) = (uint8_t)p;
    m(iy + 6) = (uint8_t)(p >> 8);
    normalize_down(iy);
  }
}

/* MBPlayer_play_wonevt_1: ключ прочь, F-номер, ключ. */
void Player::note_on_fm(uint32_t iy) {
  const uint8_t port = m(iy);
  const uint8_t a0 = m(iy + 2);
  const uint8_t b0 = (uint8_t)(a0 | 0x10);
  uint8_t value = m(iy + 9);
  fm_select(port, b0);
  fm_data(value);
  fm_select(port, a0);
  const uint8_t number = m(iy + 0x0A);
  fm_data(number);
  m(iy + 0x0B) = number;
  fm_select(port, b0);
  value |= 0x20;
  m(iy + 0x0C) = value;
  fm_data(value);
}

/* loc_0_467D */
void Player::note_off_fm(uint32_t iy) {
  m(iy + 4) = 0;
  fm_select(m(iy), (uint8_t)(m(iy + 2) | 0x10));
  fm_data(fm_[fm_latch_] & 0xDF);
}

/* Стереобиты Cx при смене голоса: (r & 30h) ^ 30h, ноль — оба канала. */
static uint8_t keep_stereo(uint8_t r) {
  const uint8_t x = (uint8_t)((r & 0x30) ^ 0x30);
  return x != 0 ? x : 0x30;
}

/* loc_0_4695: голос 2-операторного канала (11 байт). */
void Player::instrument_2op(uint32_t iy, uint8_t a, uint8_t b) {
  m(iy + 4) = 0;
  const uint8_t offset = m(MFM_INSTR_2OP + a);
  uint32_t voice = MFM_VOICES_2OP + offset;
  const uint8_t index = (uint8_t)(m(MFM_COUNT_2OP) - b);
  uint32_t ops = MFM_OPS2 + (uint8_t)(index * 4);
  const uint8_t port = m(iy);
  uint8_t reg = m(ops);
  for (int i = 0; i < 4; ++i) {
    fm_select(port, reg);
    reg = (uint8_t)(reg + 3);
    fm_data(rd(voice++));
    fm_select(port, reg);
    reg = (uint8_t)(reg + 0x1D);
    fm_data(rd(voice++));
  }
  ++ops;
  fm_select(port, rd(ops++));
  fm_data(rd(voice++));
  fm_select(port, rd(ops++));
  fm_data(rd(voice++));
  fm_select(port, rd(ops));
  fm_data(keep_stereo(fm_[fm_latch_]) | rd(voice));
}

/* loc_0_46FB: голос 4-операторного канала (22 байта). */
void Player::instrument_4op(uint32_t iy, uint8_t a, uint8_t b) {
  m(iy + 4) = 0;
  const uint8_t offset = m(MFM_INSTR_4OP + (uint8_t)(a * 2));
  uint32_t voice = MFM_VOICES_4OP + offset;
  const uint8_t index = (uint8_t)(m(MFM_COUNT_4OP) - b);
  uint32_t ops = MFM_OPS4 + m(MFM_TIMES_11 + (uint8_t)(index * 2));
  const uint8_t port = m(iy);
  for (int i = 0; i < 20; ++i) {
    fm_select(port, rd(ops++));
    fm_data(rd(voice++));
  }
  fm_select(port, rd(ops++));
  const uint8_t stereo = keep_stereo(fm_[fm_latch_]);
  fm_data(rd(voice) | stereo);
  fm_select(port, rd(ops));
  ++voice;
  fm_data(rd(voice) | stereo);
}

/* loc_0_4773: стерео 2-операторного (биты 4..5 Cx). */
void Player::stereo_2op(uint32_t iy, uint8_t a) {
  const uint8_t bits = (uint8_t)(a << 4);
  fm_select(m(iy), (uint8_t)(m(iy + 2) + 0x20));
  fm_data((uint8_t)((fm_[fm_latch_] & 0x0F) + bits));
}

/* loc_0_4791: стерео 4-операторного — обе половины. */
void Player::stereo_4op(uint32_t iy, uint8_t a) {
  const uint8_t bits = (uint8_t)(a << 4);
  const uint8_t port = m(iy);
  const uint8_t reg = (uint8_t)(m(iy + 2) + 0x20);
  fm_select(port, reg);
  fm_data((uint8_t)((fm_[fm_latch_] & 0x0F) + bits));
  fm_select(port, (uint8_t)(reg + 3));
  fm_data((uint8_t)((fm_[fm_latch_] & 0x0F) + bits));
}

/* loc_0_47B8: нота сдвигается на a − 198 без нового ключа. */
void Player::link_fm(uint32_t iy, uint8_t a) {
  a = (uint8_t)(a - 0xC6 + m(iy + 1));
  m(iy + 1) = a;
  const uint32_t p = MFM_FNUM + (uint8_t)(a * 2);
  uint16_t de = (uint16_t)((m(p) << 8) | m(p - 1));
  de = (uint16_t)(de + 1);
  de = (uint16_t)((de & 0xFF00) | (uint8_t)(de + m(iy + 7)));
  de = (uint16_t)(de - 1);
  const uint8_t e = (uint8_t)de;
  uint8_t d = (uint8_t)(de >> 8);
  m(iy + 0x0B) = e;
  const uint8_t port = m(iy);
  uint8_t reg = m(iy + 2);
  fm_select(port, reg);
  m(iy + 4) = 0;
  fm_data(e);
  reg |= 0x10;
  fm_select(port, reg);
  d |= 0x20;
  m(iy + 0x0C) = d;
  fm_data(d);
}

/* Сдвиг вниз и модуляция: F-номер ниже 512 — вдвое больше, блок на один ниже. */
void Player::normalize_down(uint32_t iy) {
  uint8_t h = m(iy + 0x0C);
  if ((h & 2) != 0) {
    return;
  }
  const uint8_t block = (uint8_t)((h & 0xFC) - 4);
  h &= (uint8_t)~0x20;
  const uint16_t hl = (uint16_t)(((h << 8) | m(iy + 0x0B)) * 2);
  m(iy + 0x0B) = (uint8_t)hl;
  m(iy + 0x0C) = (uint8_t)(((hl >> 8) & 3) | block);
}

/* --- события волн ---------------------------------------------------------------- */

void Player::event_wave(uint32_t iy, uint8_t a, uint8_t b) {
  if (a < 97) {
    wave_note_on(iy, b);
  } else if (a == 97) {
    wave_note_off(iy, b);
  } else if (a < 146) {
    wave_change(iy, (uint8_t)(a - 0x61));
  } else if (a < 178) {
    wave_volume(iy, b, (uint8_t)((((a - 146) ^ 0x1F) * 2) & 0xFF));
  } else if (a < 193) {
    wave_stereo(iy, b, (uint8_t)(a - 0xB9));
  } else if (a < 212) {
    wave_link(iy, b, a);
  } else if (a < 231) {                       /* MBPlayer_play_wchgpit */
    const uint8_t speed = (uint8_t)((a - 221) * 4);
    m(iy + 2) = 1;
    m(iy + 3) = speed;
    m(iy + 4) = (speed & 0x80) != 0 ? 0xFF : 0;
  } else if (a < 238) {
    m(iy + 5) = (uint8_t)((a - 234) * 4);     /* MBPlayer_play_wchgdet */
  } else if (a < 241) {                       /* MBPlayer_play_wchgmod */
    const uint8_t mode = (uint8_t)(a - 0xEC);
    m(iy + 2) = mode;
    const uint16_t p = (uint16_t)(MFM_XMODTAB + (uint8_t)(mode * 16));
    m(iy + 3) = (uint8_t)p;
    m(iy + 4) = (uint8_t)(p >> 8);
  } else if (a < 243) {
    wave_damp(iy, b);
  }
}

/* MBPlayer_play_wonevt */
void Player::wave_note_on(uint32_t iy, uint8_t b) {
  uint32_t p = (uint32_t)(m(iy + 0x0D) | (m(iy + 0x0E) << 8));
  const uint8_t c = (uint8_t)(b - 1);
  wave_out((uint8_t)(0x80 + c), rd(p++));
  for (int i = 0; i < 32; ++i) {
    const uint8_t reg = rd(p);
    if (reg == 0xFF) {
      break;
    }
    wave_out((uint8_t)(reg + c), rd(p + 1));
    p += 2;
  }
  wave_out((uint8_t)(0x50 + c), m(iy + 0x0F) | 1);
  wave_out((uint8_t)(0x68 + c), 0x80 | m(iy + 0x0B));
}

/* MBPlayer_play_woffevt */
void Player::wave_note_off(uint32_t iy, uint8_t b) {
  wave_select((uint8_t)(0x67 + b));
  m(iy + 2) = 0;
  wave_data(wave_[wave_latch_] & 0x7F);
}

/* MBPlayer_play_wchgdmp */
void Player::wave_damp(uint32_t iy, uint8_t b) {
  wave_select((uint8_t)(0x67 + b));
  m(iy + 2) = 0;
  wave_data(wave_[wave_latch_] | 0x40);
}

/* MBPlayer_play_wwavevt2 */
void Player::wave_change(uint32_t iy, uint8_t number) {
  m(iy + 0x0A) = m(MFM_XWAVNRS - 1 + number);
  m(iy + 0x0F) = (uint8_t)((uint8_t)(m(MFM_XWAVVOLS - 1 + number) * 4) | (m(iy + 0x0F) & 1));
  m(iy + 2) = 0;
}

/* MBPlayer_play_wchgvol2 */
void Player::wave_volume(uint32_t iy, uint8_t b, uint8_t value) {
  const uint8_t level = (uint8_t)(value * 4);
  wave_select((uint8_t)(0x4F + b));
  m(iy + 0x0F) = (uint8_t)((m(iy + 0x0F) & 1) | level);
  wave_data(m(iy + 0x0F));
  m(iy + 2) = 0;
}

/* MBPlayer_play_wchgste2 */
void Player::wave_stereo(uint32_t iy, uint8_t b, uint8_t value) {
  const uint8_t pan = value & 0x0F;
  m(iy + 0x0B) = (uint8_t)((m(iy + 0x0B) & 0xF0) | pan);
  wave_select((uint8_t)(0x67 + b));
  m(iy + 2) = 0;
  wave_data((uint8_t)((wave_[wave_latch_] & 0xF0) | pan));
}

/* MBPlayer_play_wlnk */
void Player::wave_link(uint32_t iy, uint8_t b, uint8_t a) {
  m(iy + 2) = 0;
  a = (uint8_t)(a - 202 + m(iy));
  m(iy) = a;
  const uint8_t octave = rd(MFM_TABDIV12 + 2u * a);
  const uint8_t index = rd(MFM_TABDIV12 + 2u * a + 1);
  const uint16_t table = (uint16_t)(m(iy + 0x10) | (m(iy + 0x11) << 8));
  const uint16_t number = rd16((uint16_t)(table + index));
  const uint8_t low = (uint8_t)(number << 1);
  const uint8_t high = (uint8_t)((((number >> 8) << 1) | ((number >> 7) & 1)) + octave);
  uint16_t freq = (uint16_t)((high << 8) | low);
  const uint8_t detune = m(iy + 5);
  const uint16_t signed_detune = (uint16_t)(((detune & 0x80) != 0 ? 0xFF00 : 0) | detune);
  freq = (uint16_t)(freq + signed_detune + signed_detune);
  m(iy + 8) = (uint8_t)freq;
  m(iy + 9) = (uint8_t)(freq >> 8);
  wave_out((uint8_t)(0x1F + b), (uint8_t)freq | m(iy + 6));
  wave_out((uint8_t)(0x37 + b), (uint8_t)(freq >> 8) | m(iy + 0x0C));
}

}  // namespace mfm
