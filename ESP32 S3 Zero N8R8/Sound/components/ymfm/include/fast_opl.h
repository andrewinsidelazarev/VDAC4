// fast_opl.h — FM-часть микросхем OPL (YM3526, YM3812, YMF262, FM YMF278B)
// для ESP32-S3: та же арифметика, что у ymfm (ymfm_fm.ipp, ymfm_opl.cpp —
// BSD-3, Aaron Giles), отсчёт в отсчёт, но без объектов на каждый оператор:
// каналы считаются блоками отсчётов в плотном цикле, состояние — в массивах.
// Звук совпадает с ymfm бит в бит (tests/test_fast_opl.py); на ПК быстрее в
// несколько раз — на ESP это разница между «не успевает» и «успевает».
//
// Нет таймеров, IRQ и CSM (в записях музыки их нет), чтения регистров.
//
// Copyright (c) 2021, Aaron Giles (ymfm); VDAC4, 2026. BSD-3-Clause, см.
// components/ymfm/LICENSE.
#ifndef FAST_OPL_H
#define FAST_OPL_H

#include <stdint.h>

namespace fast_opl {

class engine {
 public:
  // revision: 1 — YM3526 (OPL), 2 — YM3812 (OPL2), 3 — YMF262 (OPL3),
  // 4 — FM-часть YMF278B (OPL4). waveforms — общая таблица волн (8×1024
  // uint16, init_waveforms), её держит вызывающий.
  engine(int revision, const uint16_t* waveforms);

  // Таблица волн: 8 волн по 1024 значения.
  static void init_waveforms(uint16_t* table);
  static const uint32_t WAVEFORM_TABLE_SIZE = 8 * 1024;

  void reset();
  // Запись регистра: index 0..0x1FF (OPL3+: старший бит — вторая половина).
  void write(uint32_t index, uint8_t data);
  // newflag/new2flag — для разбора адресов портов вызывающим.
  uint32_t newflag() const { return revision_ >= 3 ? (regs_[0x105] & 1) : 0; }
  uint32_t new2flag() const { return revision_ >= 4 ? ((regs_[0x105] >> 1) & 1) : 0; }

  // count отсчётов. OPL/OPL2: out[0] — моно, после ЦАП YM3014 (как
  // ym3812::generate). OPL3: out[0..3] — выходы A..D после clamp16. OPL4:
  // out[0..3] без ограничения (смешивает ymf278b), а extra[i] = 1 — перед
  // отсчётом i лишний такт без выхода (пересчёт 49,5 кГц в 44,1 кГц).
  void generate(int32_t* const* out, uint32_t count, const uint8_t* extra = nullptr);

  // Индикатор канала: ослабление самой громкой несущей, 0..0x3FF.
  uint32_t channel_attenuation(uint32_t channel) const;
  uint32_t channels() const { return channels_; }

 private:
  struct op_t {
    uint32_t phase;           // 10.10
    uint32_t phase_step;      // без вибрато
    uint32_t block_freq;
    uint32_t multiple;
    const uint16_t* waveform;
    uint16_t env;             // ослабление огибающей 0..0x3FF
    uint16_t total_level;     // TL*8 + KSL
    uint16_t eg_sustain;
    uint8_t eg_rate[5];       // по состоянию: DEPRESS, ATTACK, DECAY, SUSTAIN, RELEASE
    uint8_t state;
    uint8_t key_state;
    uint8_t keyon_live;
    uint8_t key_dropped;      // клавишу отпустили и снова нажали раньше такта: журналы DRO, IMF, VGM
                              // пишут оба в одну миллисекунду, а настоящей микросхеме между записями
                              // нужно не меньше 23 мкс (больше такта) — такт должен увидеть отпускание
    uint8_t am;
    uint8_t pm;
    uint16_t opoffs;
    uint16_t choffs;
  };
  struct channel_t {
    int16_t feedback[2];
    int16_t feedback_in;
    uint8_t op[4];            // номера операторов, 0xFF — нет
    uint16_t choffs;
  };

  void prepare();
  void assign_operators();
  void cache_operator(op_t& op);
  void clock_keystate(op_t& op, uint32_t keystate);
  void keyonoff(uint32_t channel, uint32_t states, uint32_t type);
  void lfo_block(uint32_t clocks);
  void channel_block(uint32_t index, int32_t* const* out, uint32_t count, const uint8_t* extra);
  void rhythm_block(int32_t* const* out, uint32_t count, const uint8_t* extra);

  int revision_;
  uint32_t channels_;
  uint32_t operators_;
  const uint16_t* waveforms_;
  uint8_t regs_[0x200];
  op_t op_[36];
  channel_t ch_[18];
  uint32_t env_counter_;      // как m_env_counter у ymfm: +4 за такт
  uint32_t noise_lfsr_;
  uint16_t lfo_am_counter_;
  uint16_t lfo_pm_counter_;
  uint8_t lfo_am_;
  bool modified_;
  uint32_t prepare_count_;
  uint32_t active_;           // каналы со звуком (по последнему prepare)

  // Блок: LFO и шум по тактам (такт k блока).
  static const uint32_t BLOCK = 64;
  uint8_t am_[2 * BLOCK];
  int8_t pm_[2 * BLOCK];
  uint8_t noise_[2 * BLOCK];
};

// Память волн OPL4 (адрес 22 бита): ПЗУ с 0, ОЗУ с 0x200000. Указатели
// обновляет владелец; запись в ОЗУ через порт — его функция write.
struct pcm_memory {
  const uint8_t* rom;
  uint32_t rom_size;
  uint8_t* ram;
  uint32_t ram_size;
  void (*write)(void* context, uint32_t address, uint8_t data);
  void* context;
};

// Таблицы волн YMF278B (pcm_engine ymfm), бит в бит, блоками по каналу.
class pcm {
 public:
  explicit pcm(const pcm_memory* memory);
  void reset();
  void write(uint32_t index, uint8_t data);
  // count отсчётов в out[0..3] (выходы 0/1 — DO2, 2/3 — DO1), с нуля.
  void generate(int32_t* const* out, uint32_t count);
  uint32_t channel_attenuation(uint32_t channel) const;
  uint32_t mix_fm_l() const { return regs_[0xf8] & 7; }
  uint32_t mix_fm_r() const { return (regs_[0xf8] >> 3) & 7; }
  uint32_t mix_pcm_l() const { return regs_[0xf9] & 7; }
  uint32_t mix_pcm_r() const { return (regs_[0xf9] >> 3) & 7; }

 private:
  struct channel_t {
    uint32_t baseaddr;
    uint32_t endpos;
    uint32_t looppos;
    uint32_t curpos;
    uint32_t nextpos;
    uint32_t lfo_counter;
    uint32_t total_level;     // .10, плавно к цели
    uint16_t env;
    uint8_t state;
    uint8_t format;
    uint8_t key_state;
    // кэш регистров (cache_channel_data)
    uint32_t step;
    uint32_t target_level;
    uint32_t pan_left;
    uint32_t pan_right;
    uint32_t eg_sustain;
    uint8_t eg_rate[6];
    uint8_t lfo_step;
    uint8_t am_depth;
    uint8_t pm_depth;
  };

  uint8_t read(uint32_t address) const;
  void cache_channel(uint32_t index);
  void prepare();
  void load_wavetable(uint32_t index);
  void channel_block(uint32_t index, int32_t* const* out, uint32_t count);

  const pcm_memory* memory_;
  uint8_t regs_[0x100];
  channel_t ch_[24];
  uint32_t env_counter_;
  bool modified_;
  uint32_t prepare_count_;
};

}  // namespace fast_opl

#endif  // FAST_OPL_H
