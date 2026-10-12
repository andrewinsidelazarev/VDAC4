/* OPL.ELF — декодер музыки микросхем Yamaha семейства OPL для плеера VDAC4.
 *
 * Микросхемы — эмулятор ymfm (components/ymfm, BSD-3): YM3526 (OPL), Y8950
 * (MSX-AUDIO, с ADPCM), YM3812 (OPL2), YMF262 (OPL3), YMF278B (OPL4: FM как у
 * OPL3 и 24 канала таблиц волн), YM2413 (OPLL) и VRC7. Форматы — записи
 * регистров со временем:
 *   VGM и VGZ (VGM в gzip) — любые из этих микросхем, по одной или по две;
 *     записи микросхем других семейств пропускаются;
 *   DRO (DOSBox Raw OPL, версии 1 и 2) — OPL2, два OPL2 (левый и правый), OPL3;
 *   IMF (id Software, типы 0 и 1) — OPL2: 560 записей в секунду, у .WLF — 700
 *     (у IMF нет подписи: его узнают по расширению, file_type плеера);
 *   MWM (MoonBlaster Wave, MSX MoonSound) — 24 канала таблиц волн OPL4: не
 *     запись, а песня; её играет перевод Z80-проигрывателя (mwm.cpp) по тикам
 *     таймера OPL4. Свои волны — набор MWK (имя — в песне): ресурс «ИМЯ.MWK»,
 *     его плагин присылает из папки песни до открытия; нет — они молчат;
 *   MFM (MoonBlaster FM) — 18 каналов FM OPL4 и 6 дорожек волн ПЗУ: перевод
 *     второго Z80-проигрывателя (mfm.cpp). Записи тика обоих проигрывателей
 *     идут в микросхему с шагом, как у Z80 (очередь со временем).
 *
 * Таблицы волн OPL4 — ПЗУ YRW801 (2 МБ) — ресурс YRW801.ROM из /WC/ESP/ (его
 * присылает плагин); без него звучат только волны в ОЗУ, которые VGM привозит
 * сам (блоки данных 0x87). Файл целиком лежит у плеера до close (keeps_file):
 * команды читаются прямо из него, VGZ распаковывается в свой буфер.
 *
 * Звук — с частотой первой микросхемы файла (OPL2 и OPL3 — 49 716 Гц, OPL4 —
 * 44 100 Гц), в 44 100 Гц его пересчитывает ядро; остальные микросхемы файла
 * подстраиваются к ней линейной интерполяцией. VGM с повтором играется до
 * конца и ещё FADE_MS по повтору с затуханием. Перемотка — команды с начала без
 * звука до места (за SETTLE_MS до него — со звуком в никуда: огибающие нот
 * успевают встать).
 *
 * Громкость: OPL, OPL2, MSX-AUDIO и OPLL выводят канал на −18 дБ, и музыка для
 * них выходит на 15–25 дБ тише записей OPL3 — им +12 дБ (выше 0,75 — мягкое
 * ограничение). Песня без повтора после конца команд звучит ещё до TAIL_MS:
 * ноты отзвучивают отпускание (DRO и IMF кончаются сразу за последней
 * записью); стихло раньше — конец. Длительность для окна — до конца команд:
 * хвост идёт сверх неё (место на шкале у конца стоит).
 * С 1.5 после смешения — тот же нормализатор, что в TRACKER.ELF (zm_level.h):
 * цель −12 LUFS, сначала прежний уровень, затем плавно 0,5 дБ/с; диапазон
 * −18…+18 дБ. Замер до ограничителя и служебного затухания в конце повтора.
 * Предварительного прохода звука нет. Перемотка сохраняет найденное усиление.
 *
 * Уровни каналов для окна — по ослаблению огибающей несущих операторов (FM) и
 * каналов таблиц волн (OPL4); показываются только каналы, где в песне есть
 * ноты (до 32).
 */
#include <math.h>
#include <new>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "em_inflate.h"
#include "fast_opl.h"
#include "mfm.h"
#include "mwm.h"
#include "ymfm_opl.h"
#include "zm_app.h"
#include "zm_source.h"
#include "zm_level.h"

namespace {

constexpr uint32_t VGM_RATE = 44100;
constexpr uint32_t FADE_MS = 8000;         /* VGM с повтором: затухание по нему */
constexpr uint32_t TAIL_MS = 2000;         /* без повтора: отзвук нот после конца команд */
constexpr float QUIET_GAIN = 4.0f;         /* OPL, OPL2, MSX-AUDIO, OPLL: +12 дБ */
constexpr uint32_t SETTLE_MS = 60;         /* перемотка: огибающие встают без звука */
constexpr uint32_t BLOCK = 64;             /* кадров между проверками событий */
constexpr uint32_t PARALLEL_MIN = 32;      /* кусок OPL4 короче — волны тут же: обмен с ядром 1 дороже */
constexpr int MAX_CHIPS = 4;
constexpr int METERS = 32;                 /* VIEW_CHANNELS плеера */
constexpr uint32_t OPL4_ROM_MAX = 0x200000;   /* ПЗУ: 0..0x1FFFFF */
constexpr uint32_t OPL4_RAM_MAX = 0x200000;   /* ОЗУ: с 0x200000 */
constexpr uint32_t ADPCM_RAM_MAX = 0x40000;   /* Y8950: 256 КБ */
constexpr uint32_t INFLATED_MAX = 16u * 1024u * 1024u;
constexpr uint32_t END = UINT32_MAX;       /* step: команды кончились */
constexpr float LEVEL_MIN_DB = -18.0f;    /* громкие OPL тоже можно ослаблять */
#define ROM_NAME "YRW801.ROM"

const zm_host_t* host;
zm_source_t table;
bool worker = false;                       /* помощник на ядре 1 есть (PLAYER 1.3) */
uint64_t wave_cycles = 0;                  /* такты волн OPL4 на ядре 1 за песню */

/* Работа помощника: волны OPL4 куска (fast_opl::pcm::generate) на ядре 1. */
struct WaveJob {
  fast_opl::pcm* pcm;
  int32_t* out[4];
  uint32_t count;
};

uint32_t cycle_count();

void wave_job(void* arg) {
  WaveJob* job = static_cast<WaveJob*>(arg);
  const uint32_t start = cycle_count();
  job->pcm->generate(job->out, job->count);
  wave_cycles += cycle_count() - start;
}

/* Такты процессора: сколько стоит синтез (в журнал при close). */
uint32_t cycle_count() {
#if defined(__XTENSA__)
  uint32_t count;
  __asm__ __volatile__("rsr %0, ccount" : "=a"(count));
  return count;
#else
  return 0;
#endif
}

/* Состояние микросхем — во внутренней памяти (его читает каждый отсчёт). */
void* fast_alloc(size_t size) {
  void* memory = host->alloc(size, ZM_MEM_INTERNAL);
  return memory != nullptr ? memory : host->alloc(size, ZM_MEM_PSRAM);
}

uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Строка в буфер size с нулём на конце (обрезается). */
void copy_text(char* to, size_t size, const char* from) {
  size_t n = strlen(from);
  if (n >= size) {
    n = size - 1;
  }
  memcpy(to, from, n);
  to[n] = 0;
}
uint32_t le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* --- микросхемы ------------------------------------------------------------ */

enum ChipType : uint8_t {
  CHIP_YM3526, CHIP_Y8950, CHIP_YM3812, CHIP_YMF262, CHIP_YMF278B, CHIP_YM2413, CHIP_VRC7,
  CHIP_TYPES
};

struct ChipKind {
  const char* name;
  const char* family;
  uint8_t channels;          /* каналов для индикаторов */
};

const ChipKind kinds[CHIP_TYPES] = {
    {"YM3526", "OPL", 9},   {"Y8950", "MSX-AUDIO", 9}, {"YM3812", "OPL2", 9},
    {"YMF262", "OPL3", 18}, {"YMF278B", "OPL4", 18 + 24}, {"YM2413", "OPLL", 9},
    {"VRC7", "OPLL", 6},
};

/* Микросхема: память волн и выход в стерео блоками своей частоты. */
class Chip : public ymfm::ymfm_interface {
 public:
  ChipType type = CHIP_YM3812;
  uint32_t rate = 0;
  int8_t pan = 0;                 /* −1 — только левый, 1 — только правый */
  /* OPL4: ПЗУ (0..0x1FFFFF) и ОЗУ (с 0x200000); Y8950: ОЗУ ADPCM. */
  const uint8_t* rom = nullptr;
  uint32_t rom_size = 0;
  uint8_t* own_rom = nullptr;     /* ПЗУ из блоков VGM (ресурса нет) */
  uint8_t* ram = nullptr;
  uint32_t ram_size = 0;
  uint32_t ram_want = 0;
  bool ram_failed = false;
  /* Частота другая, чем у вывода: шаг в 1/65536 своего отсчёта на кадр. */
  uint32_t step = 0x10000;
  uint32_t phase = 0;
  int32_t previous[2] = {0, 0};
  int32_t current[2] = {0, 0};

  virtual ~Chip() {}
  virtual void reset() {}
  virtual void write(uint8_t port, uint8_t reg, uint8_t data) {}
  /* count (до BLOCK) своих отсчётов: левый и правый. */
  virtual void render(int32_t* left, int32_t* right, uint32_t count) {}
  virtual uint8_t level(uint32_t channel) { return 0; }
  /* ПЗУ или ОЗУ заведены или сменились (быстрым таблицам волн OPL4 — указатели). */
  virtual void memory_changed() {}

  /* Кадр вывода другой частоты: интерполяция между двумя своими отсчётами. */
  void frame(int32_t* stereo) {
    phase += step;
    while (phase >= 0x10000) {
      previous[0] = current[0];
      previous[1] = current[1];
      render(&current[0], &current[1], 1);
      phase -= 0x10000;
    }
    const int64_t t = phase;
    stereo[0] = previous[0] + (int32_t)(((int64_t)(current[0] - previous[0]) * t) >> 16);
    stereo[1] = previous[1] + (int32_t)(((int64_t)(current[1] - previous[1]) * t) >> 16);
  }

  bool ensure_ram() {
    if (ram != nullptr) {
      return true;
    }
    if (ram_failed) {
      return false;
    }
    const uint32_t limit = type == CHIP_YMF278B ? OPL4_RAM_MAX : ADPCM_RAM_MAX;
    uint32_t size = ram_want != 0 && ram_want < limit ? ram_want : limit;
    ram = (uint8_t*)host->alloc(size, ZM_MEM_PSRAM);
    if (ram == nullptr) {
      ram_failed = true;
      host->log("opl: no %u bytes for the %s memory", (unsigned)size, kinds[type].name);
      return false;
    }
    memset(ram, 0, size);
    ram_size = size;
    memory_changed();
    return true;
  }

  /* Блок данных VGM: [длина памяти LE32][начало LE32][байты]. */
  void load_block(bool to_rom, const uint8_t* block, uint32_t size) {
    if (size < 8) {
      return;
    }
    const uint32_t total = le32(block);
    const uint32_t start = le32(block + 4);
    const uint8_t* bytes = block + 8;
    uint32_t length = size - 8;
    if (to_rom) {
      if (rom != nullptr && own_rom == nullptr) {
        return;                       /* есть ресурс YRW801.ROM: блок — то же ПЗУ */
      }
      if (own_rom == nullptr) {
        const uint32_t want = total != 0 && total < OPL4_ROM_MAX ? total : OPL4_ROM_MAX;
        own_rom = (uint8_t*)host->alloc(want, ZM_MEM_PSRAM);
        if (own_rom == nullptr) {
          return;
        }
        memset(own_rom, 0, want);
        rom = own_rom;
        rom_size = want;
        memory_changed();
      }
      if (start < rom_size) {
        memcpy(own_rom + start, bytes, length < rom_size - start ? length : rom_size - start);
      }
      return;
    }
    if (ram == nullptr && ram_want == 0) {
      ram_want = total;
    }
    if (!ensure_ram() || start >= ram_size) {
      return;
    }
    memcpy(ram + start, bytes, length < ram_size - start ? length : ram_size - start);
  }

  uint8_t ymfm_external_read(ymfm::access_class access, uint32_t address) override {
    if (type == CHIP_YMF278B && access == ymfm::ACCESS_PCM) {
      address &= 0x3FFFFF;
      if (address < 0x200000) {
        return address < rom_size ? rom[address] : 0;
      }
      address -= 0x200000;
      return address < ram_size ? ram[address] : 0;
    }
    if (type == CHIP_Y8950 && access == ymfm::ACCESS_ADPCM_B) {
      return address < ram_size ? ram[address] : 0;
    }
    return 0;
  }

  void ymfm_external_write(ymfm::access_class access, uint32_t address, uint8_t data) override {
    if (type == CHIP_YMF278B && access == ymfm::ACCESS_PCM) {
      address &= 0x3FFFFF;
      if (address >= 0x200000 && ensure_ram() && address - 0x200000 < ram_size) {
        ram[address - 0x200000] = data;
      }
    } else if (type == CHIP_Y8950 && access == ymfm::ACCESS_ADPCM_B) {
      if (ensure_ram() && address < ram_size) {
        ram[address] = data;
      }
    }
  }
};

/* Ослабление 0..0x3FF (0,094 дБ на единицу) — в уровень 0..255, 48 дБ. */
uint8_t meter(uint32_t attenuation) {
  return attenuation >= 0x200 ? 0 : (uint8_t)((0x200 - attenuation) * 255 / 0x200);
}

/* Уровень канала FM ymfm: самый громкий из несущих операторов. */
template <class Engine>
uint8_t fm_level(Engine& fm, uint32_t index) {
  auto* channel = fm.debug_channel(index);
  if (channel == nullptr || channel->debug_operator(0) == nullptr) {
    return 0;
  }
  const uint32_t algorithm = fm.regs().ch_algorithm(channel->choffs());
  uint32_t carriers;
  if (fm.regs().rhythm_enable() && index >= 6 && index <= 8) {
    carriers = 3;                                  /* ударные: оба оператора звучат */
  } else if (channel->is4op()) {
    static const uint8_t four[4] = {0x8, 0x9, 0xA, 0xD};   /* FM-FM, AM-FM, FM-AM, AM-AM */
    carriers = four[algorithm & 3];
  } else {
    carriers = (algorithm & 1) ? 3 : 2;
  }
  uint32_t best = 0x3FF;
  for (uint32_t op = 0; op < 4; ++op) {
    auto* oper = channel->debug_operator(op);
    if (oper == nullptr || ((carriers >> op) & 1) == 0) {
      continue;
    }
    auto& cache = oper->debug_cache();
    const uint32_t attenuation = (uint32_t)(oper->debug_eg_attenuation() >> cache.eg_shift) + cache.total_level;
    best = attenuation < best ? attenuation : best;
  }
  return meter(best);
}

/* Микросхема целиком в ymfm (Y8950, YM2413, VRC7; в сборке сравнения — эталон). */
template <class T, ChipType TYPE>
class YmfmChip final : public Chip {
  struct Core : T {
    explicit Core(ymfm::ymfm_interface& intf) : T(intf) {}
    decltype(auto) fm() { return (this->m_fm); }
    decltype(auto) pcm() { return (this->m_pcm); }
  };
  Core chip_;

 public:
  explicit YmfmChip(uint32_t clock) : chip_(*this) {
    type = TYPE;
    rate = chip_.sample_rate(clock);
    chip_.reset();
  }

  void reset() override { chip_.reset(); }

  void write(uint8_t port, uint8_t reg, uint8_t data) override {
    chip_.write(port * 2u, reg);
    chip_.write(port * 2u + 1u, data);
  }

  void render(int32_t* left, int32_t* right, uint32_t count) override {
    for (uint32_t i = 0; i < count; ++i) {
      typename T::output_data out;
      chip_.generate(&out, 1);
      if constexpr (TYPE == CHIP_YMF262) {
        left[i] = out.data[0];                 /* A и B: как у Sound Blaster */
        right[i] = out.data[1];
      } else if constexpr (TYPE == CHIP_YMF278B) {
        left[i] = out.data[4];                 /* DO2: FM и волны, как у MoonSound */
        right[i] = out.data[5];
      } else if constexpr (TYPE == CHIP_YM2413 || TYPE == CHIP_VRC7) {
        left[i] = right[i] = out.data[0] + out.data[1];   /* мелодия и ударные */
      } else {
        left[i] = right[i] = out.data[0];
      }
    }
  }

  uint8_t level(uint32_t channel) override {
    if constexpr (TYPE == CHIP_YMF278B) {
      if (channel >= 18) {
        auto* wave = chip_.pcm().debug_channel(channel - 18);
        return wave != nullptr ? meter(wave->debug_attenuation()) : 0;
      }
    }
    return fm_level(chip_.fm(), channel);
  }
};

template <class C>
Chip* new_chip(uint32_t clock) {
  void* memory = fast_alloc(sizeof(C));
  return memory != nullptr ? new (memory) C(clock) : nullptr;
}

void delete_chip(Chip* chip) {
  if (chip != nullptr) {
    host->free(chip->own_rom);
    host->free(chip->ram);
    chip->~Chip();
    host->free(chip);
  }
}

#if defined(OPL_COMPARE)
/* Сборка сравнения (стенд на ПК): быстрый движок и ymfm на одних записях. */
uint64_t compare_samples;
uint64_t compare_mismatches;

void compare(Chip* reference, const int32_t* left, const int32_t* right, uint32_t count) {
  int32_t l[BLOCK], r[BLOCK];
  reference->render(l, r, count);
  for (uint32_t i = 0; i < count; ++i) {
    if (l[i] != left[i] || r[i] != right[i]) {
      if (compare_mismatches < 5) {
        host->log("opl: compare %s: sample %llu: fast %d %d, ymfm %d %d", kinds[reference->type].name,
                  (unsigned long long)(compare_samples + i), (int)left[i], (int)right[i], (int)l[i], (int)r[i]);
      }
      ++compare_mismatches;
    }
  }
  compare_samples += count;
}
#endif

/* OPL, OPL2, OPL3 — быстрый движок (components/ymfm/fast_opl), звук как у ymfm. */
template <ChipType TYPE>
class FastChip final : public Chip {
  static constexpr int revision = TYPE == CHIP_YM3526 ? 1 : (TYPE == CHIP_YM3812 ? 2 : 3);
  fast_opl::engine engine_;
#if defined(OPL_COMPARE)
  Chip* reference_;
#endif

 public:
  FastChip(uint32_t clock, const uint16_t* waveforms) : engine_(revision, waveforms) {
    type = TYPE;
    rate = clock / (revision >= 3 ? 288 : 72);
#if defined(OPL_COMPARE)
    if constexpr (TYPE == CHIP_YM3526) reference_ = new_chip<YmfmChip<ymfm::ym3526, TYPE>>(clock);
    else if constexpr (TYPE == CHIP_YM3812) reference_ = new_chip<YmfmChip<ymfm::ym3812, TYPE>>(clock);
    else reference_ = new_chip<YmfmChip<ymfm::ymf262, TYPE>>(clock);
#endif
  }

#if defined(OPL_COMPARE)
  ~FastChip() override { delete_chip(reference_); }
#endif

  void reset() override {
    engine_.reset();
#if defined(OPL_COMPARE)
    reference_->reset();
#endif
  }

  void write(uint8_t port, uint8_t reg, uint8_t data) override {
    uint32_t index = reg;
    if (revision >= 3 && port != 0) {
      /* Как ymf262::write_address_hi: без NEW верхняя половина — только 0x105. */
      index |= 0x100;
      if (engine_.newflag() == 0 && index != 0x105) {
        index &= 0xff;
      }
    }
    engine_.write(index, data);
#if defined(OPL_COMPARE)
    reference_->write(port, reg, data);
#endif
  }

  void render(int32_t* left, int32_t* right, uint32_t count) override {
    if constexpr (revision >= 3) {
      int32_t c[BLOCK], d[BLOCK];
      int32_t* out[4] = {left, right, c, d};    /* A и B: как у Sound Blaster */
      engine_.generate(out, count);
    } else {
      int32_t* out[4] = {left, nullptr, nullptr, nullptr};
      engine_.generate(out, count);
      memcpy(right, left, count * sizeof(int32_t));
    }
#if defined(OPL_COMPARE)
    compare(reference_, left, right, count);
#endif
  }

  uint8_t level(uint32_t channel) override { return meter(engine_.channel_attenuation(channel)); }
};

/* OPL4: FM и таблицы волн — быстрые движки (fast_opl), смешение как у
 * ymf278b::generate (FM идёт с 49,5 кГц: лишний такт каждые 171/21 отсчёта). */
class Opl4Chip final : public Chip {
  fast_opl::engine fm_;
  fast_opl::pcm_memory memory_;
  fast_opl::pcm pcm_;
  uint32_t fm_pos_ = 0;
#if defined(OPL_COMPARE)
  Chip* reference_;
#endif

  static void memory_write(void* context, uint32_t address, uint8_t data) {
    static_cast<Opl4Chip*>(context)->ymfm_external_write(ymfm::ACCESS_PCM, address, data);
  }

 public:
  Opl4Chip(uint32_t clock, const uint16_t* waveforms)
      : fm_(4, waveforms), memory_{nullptr, 0, nullptr, 0, memory_write, this}, pcm_(&memory_) {
    type = CHIP_YMF278B;
    rate = clock / 768;
    pcm_.reset();
#if defined(OPL_COMPARE)
    reference_ = new_chip<YmfmChip<ymfm::ymf278b, CHIP_YMF278B>>(clock);
#endif
  }

#if defined(OPL_COMPARE)
  ~Opl4Chip() override {
    reference_->rom = nullptr;                 /* память у эталона общая с нами */
    reference_->ram = nullptr;
    delete_chip(reference_);
  }
#endif

  /* ПЗУ или ОЗУ сменились: таблицам волн — новые указатели. */
  void memory_changed() override {
    memory_.rom = rom;
    memory_.rom_size = rom_size;
    memory_.ram = ram;
    memory_.ram_size = ram_size;
#if defined(OPL_COMPARE)
    reference_->rom = rom;
    reference_->rom_size = rom_size;
    reference_->ram = ram;
    reference_->ram_size = ram_size;
    reference_->ram_failed = true;            /* своё ОЗУ эталону не заводить */
#endif
  }

  void reset() override {
    fm_.reset();
    pcm_.reset();
#if defined(OPL_COMPARE)
    reference_->reset();
#endif
  }

  void write(uint8_t port, uint8_t reg, uint8_t data) override {
#if defined(OPL_COMPARE)
    /* Эталон пишет в ту же память: ему — первым, пока у нас она та же. */
    if (port == 2 && reg == 6) {
      memory_changed();
    }
    reference_->write(port, reg, data);
#endif
    if (port == 2) {
      if (fm_.new2flag() != 0) {                 /* write_data_pcm: до NEW2 — мимо */
        pcm_.write(reg, data);
      }
    } else {
      uint32_t index = reg;
      if (port == 1) {
        index |= 0x100;
        if (fm_.newflag() == 0 && index != 0x105) {
          index &= 0xff;
        }
      }
      fm_.write(index, data);
    }
  }

  void render(int32_t* left, int32_t* right, uint32_t count) override {
    static const int16_t mix_scale[8] = {0x7fa, 0x5a4, 0x3fd, 0x2d2, 0x1fe, 0x169, 0xff, 0};
    int32_t a[BLOCK], b[BLOCK], c[BLOCK], d[BLOCK];
    int32_t w0[BLOCK], w1[BLOCK], w2[BLOCK], w3[BLOCK];
    uint8_t extra[BLOCK] = {};
    for (uint32_t i = 0; i < count; ++i) {
      fm_pos_ += 192 - 171;
      extra[i] = fm_pos_ >= 171;
      if (extra[i]) {
        fm_pos_ -= 171;
      }
    }
    int32_t* fm_out[4] = {a, b, c, d};
    int32_t* wave_out[4] = {w0, w1, w2, w3};
    /* Помощник на ядре 1: волны куска — у него, FM — здесь, одновременно. Записи
     * в регистры идут между кусками (помощник уже свободен), поэтому звук тот
     * же отсчёт в отсчёт, что и на одном ядре. */
    WaveJob job = {&pcm_, {w0, w1, w2, w3}, count};
    const bool parallel = worker && count >= PARALLEL_MIN && host->worker_start(wave_job, &job) == ZM_OK;
    fm_.generate(fm_out, count, extra);
    if (parallel) {
      host->worker_wait();
    } else {
      pcm_.generate(wave_out, count);
    }
    const int32_t pcm_l = mix_scale[pcm_.mix_pcm_l()];
    const int32_t pcm_r = mix_scale[pcm_.mix_pcm_r()];
    const int32_t fm_l = mix_scale[pcm_.mix_fm_l()];
    const int32_t fm_r = mix_scale[pcm_.mix_fm_r()];
    for (uint32_t i = 0; i < count; ++i) {
      /* DO2: FM 0+1 и волны 0+1 (выход MoonSound). */
      const int32_t l = (a[i] * fm_l + w0[i] * pcm_l) >> 11;
      const int32_t r = (b[i] * fm_r + w1[i] * pcm_r) >> 11;
      left[i] = l < -32768 ? -32768 : (l > 32767 ? 32767 : l);
      right[i] = r < -32768 ? -32768 : (r > 32767 ? 32767 : r);
    }
#if defined(OPL_COMPARE)
    memory_changed();
    compare(reference_, left, right, count);
#endif
  }

  uint8_t level(uint32_t channel) override {
    if (channel >= 18) {
      return meter(pcm_.channel_attenuation(channel - 18));
    }
    return meter(fm_.channel_attenuation(channel));
  }
};

template <class C>
Chip* new_fast_chip(uint32_t clock, const uint16_t* waveforms) {
  void* memory = fast_alloc(sizeof(C));
  return memory != nullptr ? new (memory) C(clock, waveforms) : nullptr;
}

Chip* make_chip(ChipType type, uint32_t clock, const uint16_t* waveforms) {
  switch (type) {
    case CHIP_YM3526: return new_fast_chip<FastChip<CHIP_YM3526>>(clock, waveforms);
    case CHIP_YM3812: return new_fast_chip<FastChip<CHIP_YM3812>>(clock, waveforms);
    case CHIP_YMF262: return new_fast_chip<FastChip<CHIP_YMF262>>(clock, waveforms);
    case CHIP_YMF278B: return new_fast_chip<Opl4Chip>(clock, waveforms);
    case CHIP_Y8950: return new_chip<YmfmChip<ymfm::y8950, CHIP_Y8950>>(clock);
    case CHIP_YM2413: return new_chip<YmfmChip<ymfm::ym2413, CHIP_YM2413>>(clock);
    case CHIP_VRC7: return new_chip<YmfmChip<ymfm::ds1001, CHIP_VRC7>>(clock);
    default: return nullptr;
  }
}

/* --- песня ------------------------------------------------------------------ */

enum Format : uint8_t { FORMAT_VGM, FORMAT_DRO, FORMAT_IMF, FORMAT_MWM, FORMAT_MFM };

struct Opl;

/* Время песен MoonBlaster: 16 единиц на кадр 44,1 кГц, тик — 57 единиц на
 * отсчёт таймера 1 OPL4. Записи одного тика у Z80 идут не разом: между двумя
 * — около 17 мкс (12 единиц), так что микросхема успевает увидеть «ключ прочь»
 * перед новым ключом; эмулятор получает их с тем же шагом. */
constexpr uint32_t MOON_RATE = VGM_RATE * 16;
constexpr uint32_t MOON_WRITE = 12;
constexpr uint32_t MOON_QUEUE = 1024;

struct TimedWrite {
  uint16_t at;                    /* единиц от начала тика */
  uint8_t port;
  uint8_t reg;
  uint8_t value;
};

/* MWM и MFM: песня, проигрыватель и записи текущего тика со временем. */
struct MoonState {
  mwm::Song wave_song;
  mwm::Kit kit;
  mwm::Player wave_player;
  mwm::Output wave_out;
  mfm::Song fm_song;
  mfm::Player fm_player;
  mfm::Output fm_out;
  Opl* opl;
  bool fm;                        /* MFM */
  uint32_t wait;                  /* единиц на тик */
  bool timed;                     /* записи — в очередь (в тике), иначе сразу */
  uint32_t clock;                 /* время следующей записи в тике */
  uint32_t count, next;
  TimedWrite queue[MOON_QUEUE];
};

struct Meter {
  uint8_t chip;
  uint8_t channel;
};

struct Opl {
  Format format;
  const uint8_t* data;            /* файл (VGZ — распакованный) */
  uint32_t size;
  uint8_t* inflated;
  uint32_t start, end;            /* команды */
  uint32_t loop;                  /* VGM: смещение повтора, 0 — нет */
  uint32_t tick_rate;             /* единиц времени песни в секунду */
  uint8_t dro_version;
  uint8_t dro_short, dro_long;    /* DRO 2: коды задержек */
  uint8_t dro_codes;              /* длина таблицы регистров */
  const uint8_t* dro_map;
  uint8_t dro_bank;               /* DRO 1: выбранная половина / микросхема */
  MoonState* moon;

  Chip* chips[MAX_CHIPS];
  int chip_count;
  uint16_t* waveforms;            /* таблица волн быстрого движка (общая) */
  Chip* by_type[CHIP_TYPES][2];   /* для команд VGM */
  uint32_t rate;                  /* частота вывода — первой микросхемы */
  float gain;
  zm_level_t level;               /* общая с TRACKER нормализация громкости */

  /* Ход. */
  uint32_t pos;                   /* следующая команда */
  uint64_t tick;                  /* её время */
  uint64_t frames;                /* кадров от начала песни */
  bool stream_end;
  bool scanning;                  /* проход open: ноты каналов, длительность */
  bool loop_seen;
  uint64_t total_ticks;           /* время до конца команд (первый проход) */
  uint64_t loop_ticks;            /* время места повтора */
  uint64_t fade_frame, end_frame;
  uint64_t song_frame;            /* конец для окна: команды (у повтора — и затухание) */
  uint64_t scan_end;              /* end_frame по проходу open (хвост его укорачивает) */
  uint32_t silent;                /* кадров тишины подряд в хвосте после конца команд */

  /* Индикаторы: каналы с нотами. */
  uint64_t used[MAX_CHIPS];
  Meter meters[METERS];
  uint8_t meter_count;

  uint32_t cycles_max;
  uint64_t cycles;
  uint64_t cycled_frames;
  char format_text[24];
  char title[64];
  char artist[64];
  char album[64];
  uint16_t year;
};

uint64_t frame_at(const Opl* o, uint64_t tick) { return tick * o->rate / o->tick_rate; }

/* Запись регистра микросхемы: в проходе open — только заметить ноту. */
void chip_write(Opl* o, Chip* chip, uint8_t port, uint8_t reg, uint8_t data) {
  if (chip == nullptr) {
    return;
  }
  if (!o->scanning) {
    chip->write(port, reg, data);
    return;
  }
  int index = 0;
  while (index < o->chip_count && o->chips[index] != chip) {
    ++index;
  }
  uint64_t& used = o->used[index];
  if (chip->type == CHIP_YM2413 || chip->type == CHIP_VRC7) {
    if (reg >= 0x20 && reg <= 0x28 && (data & 0x10) != 0) {
      used |= 1ull << (reg - 0x20);
    } else if (reg == 0x0E && (data & 0x20) != 0 && (data & 0x1F) != 0) {
      used |= 7ull << 6;
    }
    return;
  }
  if (port == 2) {                                   /* OPL4: таблицы волн */
    if (reg >= 0x68 && reg <= 0x7F && (data & 0x80) != 0) {
      used |= 1ull << (18 + reg - 0x68);
    }
    return;
  }
  if (reg >= 0xB0 && reg <= 0xB8 && (data & 0x20) != 0) {
    used |= 1ull << (port * 9 + reg - 0xB0);
  } else if (reg == 0xBD && port == 0 && (data & 0x20) != 0 && (data & 0x1F) != 0) {
    used |= 7ull << 6;                               /* ударные: каналы 6..8 */
  }
}

/* VGM: блок данных 0x67. */
void vgm_block(Opl* o, uint8_t type, const uint8_t* block, uint32_t size, int second) {
  if (o->scanning) {
    return;
  }
  Chip* chip = nullptr;
  bool to_rom = false;
  if (type == 0x84 || type == 0x87) {
    chip = o->by_type[CHIP_YMF278B][second];
    to_rom = type == 0x84;
  } else if (type == 0x88) {
    chip = o->by_type[CHIP_Y8950][second];
  }
  if (chip != nullptr) {
    chip->load_block(to_rom, block, size);
  }
}

/* Команды VGM до ожидания: его длина или END. */
uint32_t vgm_step(Opl* o) {
  const uint8_t* d = o->data;
  for (;;) {
    const uint32_t p = o->pos;
    if (p >= o->end) {
      return END;
    }
    if (o->scanning && p == o->loop && !o->loop_seen) {
      o->loop_ticks = o->tick;          /* время места повтора */
      o->loop_seen = true;
    }
    const uint8_t command = d[p];
    uint32_t length = 1;
    uint32_t wait = 0;
    auto have = [&](uint32_t n) { return p + n <= o->end; };
    switch (command) {
      case 0x51: case 0xA1:
        length = 3;
        if (have(3)) chip_write(o, o->by_type[CHIP_YM2413][command >> 7] != nullptr
                                       ? o->by_type[CHIP_YM2413][command >> 7]
                                       : o->by_type[CHIP_VRC7][command >> 7],
                                0, d[p + 1], d[p + 2]);
        break;
      case 0x5A: case 0xAA:
        length = 3;
        if (have(3)) chip_write(o, o->by_type[CHIP_YM3812][command >> 7], 0, d[p + 1], d[p + 2]);
        break;
      case 0x5B: case 0xAB:
        length = 3;
        if (have(3)) chip_write(o, o->by_type[CHIP_YM3526][command >> 7], 0, d[p + 1], d[p + 2]);
        break;
      case 0x5C: case 0xAC:
        length = 3;
        if (have(3)) chip_write(o, o->by_type[CHIP_Y8950][command >> 7], 0, d[p + 1], d[p + 2]);
        break;
      case 0x5E: case 0x5F: case 0xAE: case 0xAF:
        length = 3;
        if (have(3)) chip_write(o, o->by_type[CHIP_YMF262][command >> 7], command & 1, d[p + 1], d[p + 2]);
        break;
      case 0xD0:
        length = 4;
        if (have(4) && (d[p + 1] & 0x7F) <= 2) {
          chip_write(o, o->by_type[CHIP_YMF278B][d[p + 1] >> 7], d[p + 1] & 0x7F, d[p + 2], d[p + 3]);
        }
        break;
      case 0x61:
        length = 3;
        if (have(3)) wait = le16(d + p + 1);
        break;
      case 0x62: wait = 735; break;
      case 0x63: wait = 882; break;
      case 0x66:
        if (o->loop != 0 && !o->scanning) {
          o->pos = o->loop;            /* повтор */
          continue;
        }
        return END;
      case 0x67: {
        if (!have(7) || d[p + 1] != 0x66) {
          return END;
        }
        uint32_t size = le32(d + p + 3);
        const int second = (size >> 31) & 1;
        size &= 0x7FFFFFFF;
        if (size > o->end - p - 7) {
          return END;
        }
        vgm_block(o, d[p + 2], d + p + 7, size, second);
        length = 7 + size;
        break;
      }
      case 0x68: length = 12; break;
      case 0x90: case 0x91: case 0x95: length = 5; break;
      case 0x92: length = 6; break;
      case 0x93: length = 11; break;
      case 0x94: length = 2; break;
      case 0xE0: length = 5; break;
      default:
        if (command >= 0x70 && command <= 0x7F) {
          wait = (command & 15) + 1u;
        } else if (command >= 0x80 && command <= 0x8F) {
          wait = command & 15;          /* YM2612 ЦАП: ждать */
        } else if (command >= 0x30 && command <= 0x3F) {
          length = 2;
        } else if (command >= 0x40 && command <= 0x4E) {
          length = 3;
        } else if (command == 0x4F || command == 0x50) {
          length = 2;
        } else if (command >= 0x51 && command <= 0x5F) {
          length = 3;                   /* другие микросхемы */
        } else if (command >= 0xA0 && command <= 0xBF) {
          length = 3;
        } else if (command >= 0xC0 && command <= 0xDF) {
          length = 4;
        } else if (command >= 0xE1) {
          length = 5;
        }
        break;
    }
    if (p + length > o->end) {
      return END;
    }
    o->pos = p + length;
    if (wait != 0) {
      return wait;
    }
  }
}

/* DRO: команды до задержки (мс). */
uint32_t dro_step(Opl* o) {
  const uint8_t* d = o->data;
  Chip* first = o->chips[0];
  Chip* second = o->chip_count > 1 ? o->chips[1] : nullptr;
  const bool opl3 = first->type == CHIP_YMF262;
  for (;;) {
    const uint32_t p = o->pos;
    if (p + 2 > o->end) {
      return END;
    }
    uint8_t code = d[p];
    const uint8_t value = d[p + 1];
    if (o->dro_version == 2) {
      o->pos = p + 2;
      if (code == o->dro_short) {
        return value + 1u;
      }
      if (code == o->dro_long) {
        return (value + 1u) << 8;
      }
      const uint8_t bank = code >> 7;
      code &= 0x7F;
      if (code >= o->dro_codes) {
        continue;
      }
      if (opl3) {
        chip_write(o, first, bank, o->dro_map[code], value);
      } else {
        chip_write(o, bank != 0 ? second : first, 0, o->dro_map[code], value);
      }
      continue;
    }
    /* DRO 0 и 1: 0/1 — задержка, 2/3 — половина, 4 — следующий байт — регистр.
     * Начальные регистры DOSBox пишет парами без кода 4: (01 20) в самом начале —
     * регистр 1, а не задержка; 4 перед регистром от 5 — тоже просто регистр 4. */
    if (code == 0x01 && p == o->start) {
      code = 0x05;                             /* как обычная пара */
    }
    switch (code) {
      case 0x00:
        o->pos = p + 2;
        return value + 1u;
      case 0x01:
        if (p + 3 > o->end) {
          return END;
        }
        o->pos = p + 3;
        return le16(d + p + 1) + 1u;
      case 0x02: case 0x03:
        o->dro_bank = code - 2;
        o->pos = p + 1;
        continue;
      case 0x04:
        if (value >= 0x05) {
          o->pos = p + 2;                      /* регистр 4 */
          break;
        }
        if (p + 3 > o->end) {
          return END;
        }
        code = d[p + 1];
        o->pos = p + 3;
        break;
      case 0x05:
        code = d[p];
        o->pos = p + 2;
        break;
      default:
        o->pos = p + 2;
        break;
    }
    const uint8_t data = d[o->pos - 1];
    if (opl3) {
      chip_write(o, first, o->dro_bank, code, data);
    } else {
      chip_write(o, o->dro_bank != 0 ? second : first, 0, code, data);
    }
  }
}

/* IMF: [регистр][значение][задержка LE16] — запись, потом ожидание. */
uint32_t imf_step(Opl* o) {
  for (;;) {
    const uint32_t p = o->pos;
    if (p + 4 > o->end) {
      return END;
    }
    const uint8_t* r = o->data + p;
    o->pos = p + 4;
    chip_write(o, o->chips[0], 0, r[0], r[1]);
    const uint32_t wait = le16(r + 2);
    if (wait != 0) {
      return wait;
    }
  }
}

/* MoonBlaster: запись проигрывателя — в очередь тика со своим временем (вне
 * тика — сразу; в проходе open chip_write только замечает ноту). */
void moon_write(MoonState* s, uint8_t port, uint8_t reg, uint8_t value) {
  Opl* o = s->opl;
  if (!s->timed) {
    chip_write(o, o->chips[0], port, reg, value);
    return;
  }
  if (s->count == MOON_QUEUE) {                 /* тик длиннее очереди: прежние — сейчас */
    for (; s->next < s->count; ++s->next) {
      const TimedWrite& w = s->queue[s->next];
      chip_write(o, o->chips[0], w.port, w.reg, w.value);
    }
    s->count = s->next = 0;
  }
  TimedWrite& w = s->queue[s->count++];
  w.at = (uint16_t)(s->clock < s->wait ? s->clock : s->wait - 1);
  w.port = port;
  w.reg = reg;
  w.value = value;
  s->clock += MOON_WRITE;
}

void moon_wave(void* context, uint8_t reg, uint8_t value) {
  moon_write(static_cast<MoonState*>(context), 2, reg, value);
}

void moon_fm(void* context, uint16_t reg, uint8_t value) {
  moon_write(static_cast<MoonState*>(context), (uint8_t)(reg >> 8), (uint8_t)reg, value);
}

void moon_wait(void* context, uint32_t microseconds) {
  MoonState* s = static_cast<MoonState*>(context);
  s->clock += microseconds * (MOON_RATE / 1000) / 1000;
}

/* MWM, MFM: начало песни (записи init — сразу); loop — после конца с места повтора. */
void moon_begin(Opl* o, bool loop) {
  MoonState* s = o->moon;
  s->timed = false;
  s->count = s->next = 0;
  if (s->fm) {
    s->fm_player.start(&s->fm_song, &s->fm_out, loop);
  } else {
    s->wave_player.start(&s->wave_song, &s->kit, &s->wave_out, loop);
  }
}

/* Записи тика с одним временем — в микросхему; ожидание до следующих (или до
 * следующего тика). */
uint32_t moon_apply(Opl* o, MoonState* s) {
  const uint16_t at = s->queue[s->next].at;
  while (s->next < s->count && s->queue[s->next].at == at) {
    const TimedWrite& w = s->queue[s->next++];
    chip_write(o, o->chips[0], w.port, w.reg, w.value);
  }
  return (s->next < s->count ? s->queue[s->next].at : s->wait) - at;
}

/* MWM, MFM: тик таймера OPL4 (60 или 50 Гц) — записи по очереди со временем. */
uint32_t moon_step(Opl* o) {
  MoonState* s = o->moon;
  if (s->next < s->count) {
    return moon_apply(o, s);
  }
  s->count = s->next = 0;
  s->clock = 0;
  s->timed = true;
  const bool more = s->fm ? s->fm_player.tick() : s->wave_player.tick();
  s->timed = false;
  if (!more) {
    for (; s->next < s->count; ++s->next) {     /* стоп — сразу */
      const TimedWrite& w = s->queue[s->next];
      chip_write(o, o->chips[0], w.port, w.reg, w.value);
    }
    return END;
  }
  if (s->count == 0) {
    return s->wait;
  }
  if (s->queue[0].at != 0) {
    return s->queue[0].at;
  }
  return moon_apply(o, s);
}

uint32_t step(Opl* o) {
  switch (o->format) {
    case FORMAT_VGM: return vgm_step(o);
    case FORMAT_DRO: return dro_step(o);
    case FORMAT_MWM: case FORMAT_MFM: return moon_step(o);
    default: return imf_step(o);
  }
}

void restart(Opl* o) {
  for (int i = 0; i < o->chip_count; ++i) {
    o->chips[i]->reset();
    o->chips[i]->phase = 0;
  }
  o->pos = o->start;
  o->tick = 0;
  o->frames = 0;
  o->stream_end = false;
  o->silent = 0;
  o->end_frame = o->scan_end;     /* хвост прежнего прохода укоротил конец */
  o->dro_bank = 0;
  if (o->format == FORMAT_MWM || o->format == FORMAT_MFM) {
    moon_begin(o, o->loop != 0);
  }
}

/* count (до BLOCK) кадров от всех микросхем. */
void render_frames(Opl* o, float* out, uint32_t count) {
  int32_t mix_left[BLOCK], mix_right[BLOCK];
  int32_t left[BLOCK], right[BLOCK];
  memset(mix_left, 0, count * sizeof(int32_t));
  memset(mix_right, 0, count * sizeof(int32_t));
  for (int c = 0; c < o->chip_count; ++c) {
    Chip* chip = o->chips[c];
    if (chip->step == 0x10000) {
      chip->render(left, right, count);
    } else {
      for (uint32_t i = 0; i < count; ++i) {
        int32_t s[2];
        chip->frame(s);
        left[i] = s[0];
        right[i] = s[1];
      }
    }
    if (chip->pan <= 0) {
      for (uint32_t i = 0; i < count; ++i) mix_left[i] += left[i];
    }
    if (chip->pan >= 0) {
      for (uint32_t i = 0; i < count; ++i) mix_right[i] += right[i];
    }
  }
  const float scale = o->gain * (1.0f / 32768.0f);
  for (uint32_t i = 0; i < count; ++i) {
    out[2 * i] = (float)mix_left[i] * scale;
    out[2 * i + 1] = (float)mix_right[i] * scale;
  }
}

/* До max кадров звука (out может быть NULL — звук в никуда). */
uint32_t produce(Opl* o, float* out, uint32_t max) {
  uint32_t done = 0;
  float scratch[2 * BLOCK];
  while (done < max && o->frames < o->end_frame) {
    while (!o->stream_end && frame_at(o, o->tick) <= o->frames) {
      const uint32_t wait = step(o);
      if (wait == END) {
        o->stream_end = true;
        if (o->loop == 0) {
          /* Команды кончились: ещё хвост, пока ноты отзвучат (не дольше TAIL_MS). */
          const uint64_t tail = o->frames + (uint64_t)TAIL_MS * o->rate / 1000;
          if (tail < o->end_frame) {
            o->end_frame = tail;
          }
          o->silent = 0;
        }
        break;
      }
      o->tick += wait;
    }
    uint64_t next = o->stream_end ? o->end_frame : frame_at(o, o->tick);
    if (next > o->end_frame) {
      next = o->end_frame;
    }
    uint64_t count = next > o->frames ? next - o->frames : 0;
    if (count == 0) {
      if (o->frames >= o->end_frame) {
        break;
      }
      count = 1;
    }
    if (count > max - done) {
      count = max - done;
    }
    if (count > BLOCK) {
      count = BLOCK;
    }
    float* target = out != nullptr ? out + 2 * done : scratch;
    render_frames(o, target, (uint32_t)count);
    if (o->stream_end && o->loop == 0) {
      /* Хвост: 50 мс полной тишины — ноты отзвучали, конец. */
      bool quiet = true;
      for (uint32_t i = 0; i < 2 * count && quiet; ++i) {
        quiet = target[i] > -1e-5f && target[i] < 1e-5f;
      }
      o->silent = quiet ? o->silent + (uint32_t)count : 0;
      if (o->silent >= o->rate / 20) {
        o->end_frame = o->frames + count;
      }
    }
    o->frames += count;
    done += (uint32_t)count;
  }
  return done;
}

/* --- разбор файлов ------------------------------------------------------------ */

/* Строка UTF-16LE (GD3) — в UTF-8; возвращает начало следующей. */
const uint8_t* utf16_text(const uint8_t* p, const uint8_t* end, char* out, size_t size) {
  size_t used = 0;
  while (p + 2 <= end) {
    uint32_t code = le16(p);
    p += 2;
    if (code == 0) {
      break;
    }
    if (code >= 0xD800 && code <= 0xDBFF && p + 2 <= end) {
      const uint32_t low = le16(p);
      if (low >= 0xDC00 && low <= 0xDFFF) {
        p += 2;
        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
      }
    }
    if (code < 0x20) {
      code = ' ';
    }
    char bytes[4];
    size_t n;
    if (code < 0x80) {
      bytes[0] = (char)code;
      n = 1;
    } else if (code < 0x800) {
      bytes[0] = (char)(0xC0 | (code >> 6));
      bytes[1] = (char)(0x80 | (code & 0x3F));
      n = 2;
    } else if (code < 0x10000) {
      bytes[0] = (char)(0xE0 | (code >> 12));
      bytes[1] = (char)(0x80 | ((code >> 6) & 0x3F));
      bytes[2] = (char)(0x80 | (code & 0x3F));
      n = 3;
    } else {
      bytes[0] = (char)(0xF0 | (code >> 18));
      bytes[1] = (char)(0x80 | ((code >> 12) & 0x3F));
      bytes[2] = (char)(0x80 | ((code >> 6) & 0x3F));
      bytes[3] = (char)(0x80 | (code & 0x3F));
      n = 4;
    }
    if (used + n < size) {
      memcpy(out + used, bytes, n);
      used += n;
    }
  }
  while (used > 0 && out[used - 1] == ' ') {
    --used;
  }
  out[used] = 0;
  return p;
}

/* Строка ASCII с нулём из файла (IMF): видимые знаки, прочее — '?'. */
const uint8_t* ascii_text(const uint8_t* p, const uint8_t* end, char* out, size_t size) {
  size_t used = 0;
  while (p < end && *p != 0) {
    const uint8_t c = *p++;
    if (used + 1 < size) {
      out[used++] = c >= 0x20 && c < 0x7F ? (char)c : '?';
    }
  }
  if (p < end) {
    ++p;
  }
  while (used > 0 && out[used - 1] == ' ') {
    --used;
  }
  out[used] = 0;
  return p;
}

/* Метки GD3: названия (англ., иначе япон.), игра, автор, год. */
void vgm_tags(Opl* o, uint32_t at) {
  if (at + 12 > o->size || memcmp(o->data + at, "Gd3 ", 4) != 0) {
    return;
  }
  uint32_t length = le32(o->data + at + 8);
  if (length > o->size - at - 12) {
    length = o->size - at - 12;
  }
  const uint8_t* p = o->data + at + 12;
  const uint8_t* end = p + length;
  /* Строки по порядку: название, игра, система, автор (каждое англ. и япон.),
   * дата, кто снял, примечания. */
  char* targets[8] = {o->title, nullptr, o->album, nullptr, nullptr, nullptr, o->artist, nullptr};
  char text[64];
  for (int i = 0; i < 8 && p < end; ++i) {
    if (targets[i] != nullptr) {
      p = utf16_text(p, end, targets[i], sizeof(o->title));
    } else {
      p = utf16_text(p, end, text, sizeof(text));
      if ((i & 1) != 0 && targets[i - 1] != nullptr && targets[i - 1][0] == 0) {
        memcpy(targets[i - 1], text, sizeof(text));   /* японское — вместо пустого */
      }
    }
  }
  if (p < end) {
    /* Дата: «1993/06/18», «18.06.1993», «1993» — первые четыре цифры подряд. */
    utf16_text(p, end, text, sizeof(text));
    for (const char* c = text; c[0] != 0; ++c) {
      if (c[0] >= '0' && c[0] <= '9' && c[1] >= '0' && c[1] <= '9' && c[2] >= '0' &&
          c[2] <= '9' && c[3] >= '0' && c[3] <= '9') {
        o->year = (uint16_t)((c[0] - '0') * 1000 + (c[1] - '0') * 100 + (c[2] - '0') * 10 + (c[3] - '0'));
        break;
      }
    }
  }
}

bool add_chip(Opl* o, ChipType type, uint32_t clock, int index) {
  if (o->chip_count >= MAX_CHIPS || clock == 0) {
    return false;
  }
  if (o->waveforms == nullptr) {
    o->waveforms = (uint16_t*)fast_alloc(fast_opl::engine::WAVEFORM_TABLE_SIZE * sizeof(uint16_t));
    if (o->waveforms == nullptr) {
      return false;
    }
    fast_opl::engine::init_waveforms(o->waveforms);
  }
  Chip* chip = make_chip(type, clock, o->waveforms);
  if (chip == nullptr) {
    return false;
  }
  /* Частота вывода микросхемы — такт, делённый на её делитель (72, 288, 768): у такта
   * вроде 1 Гц из битого VGM она нулевая, и длительность и позиция делились бы на ноль.
   * Настоящие — десятки килогерц. */
  if (chip->rate < 1000 || chip->rate > 200000) {
    host->log("opl: chip clock %u Hz gives %u Hz output: refused", (unsigned)clock, (unsigned)chip->rate);
    delete_chip(chip);
    return false;
  }
  o->chips[o->chip_count++] = chip;
  o->by_type[type][index] = chip;
  return true;
}

bool open_vgm(Opl* o) {
  const uint8_t* h = o->data;
  if (o->size < 0x40) {
    return false;
  }
  const uint32_t version = le32(h + 8);
  uint32_t start = 0x40;
  if (version >= 0x150 && le32(h + 0x34) != 0) {
    start = 0x34 + le32(h + 0x34);
  }
  uint32_t end = le32(h + 4) + 4;
  if (end > o->size || end < start) {
    end = o->size;
  }
  if (start >= end) {
    return false;
  }
  /* Поле заголовка — только если лежит до начала команд. */
  auto field = [&](uint32_t offset) -> uint32_t { return offset + 4 <= start ? le32(h + offset) : 0; };
  struct { uint32_t offset; ChipType type; } clocks[] = {
      {0x60, CHIP_YMF278B}, {0x5C, CHIP_YMF262}, {0x58, CHIP_Y8950},
      {0x50, CHIP_YM3812},  {0x54, CHIP_YM3526}, {0x10, CHIP_YM2413},
  };
  for (const auto& item : clocks) {
    uint32_t clock = item.offset == 0x10 ? le32(h + 0x10) : field(item.offset);
    if (item.offset >= 0x50 && version < 0x151) {
      clock = 0;
    }
    ChipType type = item.type;
    if (type == CHIP_YM2413 && (clock & 0x80000000u) != 0) {
      type = CHIP_VRC7;
    }
    const bool dual = (clock & 0x40000000u) != 0;
    clock &= 0x3FFFFFFF;
    if (clock == 0) {
      continue;
    }
    add_chip(o, type, clock, 0);
    if (dual) {
      add_chip(o, type, clock, 1);
    }
  }
  if (o->chip_count == 0) {
    host->log("opl: VGM without OPL chips");
    return false;
  }
  /* Два OPL или OPL2 — стерео Sound Blaster Pro: первый слева, второй справа. */
  for (int t = CHIP_YM3526; t <= CHIP_YM3812; ++t) {
    if (o->by_type[t][1] != nullptr) {
      o->by_type[t][0]->pan = -1;
      o->by_type[t][1]->pan = 1;
    }
  }
  o->format = FORMAT_VGM;
  o->start = start;
  o->end = end;
  o->tick_rate = VGM_RATE;
  const uint32_t loop = le32(h + 0x1C);
  if (loop != 0 && loop + 0x1C >= start && loop + 0x1C < end) {
    o->loop = loop + 0x1C;
  }
  /* Громкость (как у VGMPlay): 2^(m/32); m до 0xC0 — как есть, 0xC1 — −64,
   * выше — отрицательное. */
  if (version >= 0x160 && start > 0x7C) {
    int modifier = h[0x7C];
    if (modifier == 0xC1) {
      modifier = -0x40;
    } else if (modifier > 0xC1) {
      modifier -= 0x100;
    }
    o->gain *= powf(2.0f, (float)modifier / 32.0f);
  }
  const uint32_t gd3 = le32(h + 0x14);
  if (gd3 != 0) {
    vgm_tags(o, gd3 + 0x14);
  }
  return true;
}

/* Метки DRO за данными: FF FF 1A название, 1B автор, 1C описание. */
bool dro_tags(Opl* o, uint32_t at) {
  const uint8_t* h = o->data;
  if (at + 3 > o->size || h[at] != 0xFF || h[at + 1] != 0xFF || h[at + 2] != 0x1A) {
    return false;
  }
  const uint8_t* p = ascii_text(h + at + 3, h + o->size, o->title, sizeof(o->title));
  if (p < h + o->size && *p == 0x1B) {
    ascii_text(p + 1, h + o->size, o->artist, sizeof(o->artist));
  }
  return true;
}

/* Начало данных DRO 0 и 1 по их длине (как у AdPlug): тип микросхемы — байт
 * или четыре, за данными могут быть метки. 0 — не сходится. */
uint32_t dro_start(Opl* o, bool old, uint32_t length) {
  const uint32_t one = old ? 0x11 : 0x15;
  if (length < 3 || length > o->size) {
    return 0;
  }
  if (o->size - length == one) {
    return one;
  }
  if (!old && o->size - length == 0x18) {
    return 0x18;
  }
  if (!old && length + 0x18 < o->size && dro_tags(o, length + 0x18)) {
    return 0x18;
  }
  if (length + one < o->size && dro_tags(o, length + one)) {
    return one;
  }
  return 0;
}

bool open_dro(Opl* o) {
  const uint8_t* h = o->data;
  if (o->size < 0x18) {
    return false;
  }
  const uint32_t version = le32(h + 8);
  uint8_t hardware = 0;
  ChipType type = CHIP_YM3812;
  bool dual = false;
  uint32_t start = 0;
  if (version == 0x10000 && (start = dro_start(o, false, le32(h + 0x10))) != 0) {
    hardware = h[0x14];                        /* DRO 1 */
  } else if (version != 2 && (start = dro_start(o, true, le32(h + 0x0C))) != 0) {
    hardware = h[0x10];                        /* DRO 0: без номера версии */
  }
  if (start != 0) {
    o->start = start;
    o->end = start + le32(h + (start == 0x11 ? 0x0C : 0x10));
    type = hardware == 1 ? CHIP_YMF262 : CHIP_YM3812;
    dual = hardware == 2;
    o->dro_version = 1;
  } else if (version == 2) {                   /* DRO 2 */
    if (o->size < 0x1A || h[0x15] != 0 || h[0x16] != 0) {
      host->log("opl: DRO 2 format %u, compression %u not supported", h[0x15], h[0x16]);
      return false;
    }
    hardware = h[0x14];
    o->dro_short = h[0x17];
    o->dro_long = h[0x18];
    o->dro_codes = h[0x19] > 128 ? 128 : h[0x19];
    o->dro_map = h + 0x1A;
    o->start = 0x1A + h[0x19];
    const uint32_t pairs = le32(h + 0x0C);
    o->end = o->start <= o->size && pairs <= (o->size - o->start) / 2 ? o->start + pairs * 2 : o->size;
    dro_tags(o, o->end);
    type = hardware == 2 ? CHIP_YMF262 : CHIP_YM3812;
    dual = hardware == 1;
    o->dro_version = 2;
  } else {
    host->log("opl: DRO version %08X not supported", (unsigned)version);
    return false;
  }
  if (o->start > o->end) {
    return false;
  }
  const uint32_t clock = type == CHIP_YMF262 ? 14318180 : 3579545;
  if (!add_chip(o, type, clock, 0) || (dual && !add_chip(o, type, clock, 1))) {
    return false;
  }
  if (dual) {
    o->chips[0]->pan = -1;
    o->chips[1]->pan = 1;
  }
  o->format = FORMAT_DRO;
  o->tick_rate = 1000;
  return true;
}

bool open_imf(Opl* o, const char* file_type) {
  const uint8_t* h = o->data;
  uint32_t at = 0;
  uint32_t extra = 0;
  if (o->size >= 6 && memcmp(h, "ADLIB", 5) == 0 && h[5] == 1) {
    /* IMF с заголовком: название, игра, байт, длина LE32. */
    const uint8_t* p = ascii_text(h + 6, h + o->size, o->title, sizeof(o->title));
    p = ascii_text(p, h + o->size, o->album, sizeof(o->album));
    at = (uint32_t)(p - h) + 1;
    extra = 2;
  }
  if (at + 2 + extra > o->size) {
    return false;
  }
  const uint32_t length = extra ? le32(h + at) : le16(h + at);
  if (length == 0) {
    o->start = at;                             /* тип 0: только записи */
    o->end = o->size;
  } else {
    o->start = at + 2 + extra;
    o->end = length <= o->size - o->start ? o->start + length : o->size;
    /* Хвост Адама Нильсена: 0x1A, название, автор, примечание. */
    if (o->end < o->size && h[o->end] == 0x1A) {
      const uint8_t* p = ascii_text(h + o->end + 1, h + o->size, o->title, sizeof(o->title));
      ascii_text(p, h + o->size, o->artist, sizeof(o->artist));
    }
  }
  o->end = o->start + (o->end - o->start) / 4 * 4;
  if (!add_chip(o, CHIP_YM3812, 3579545, 0)) {
    return false;
  }
  o->format = FORMAT_IMF;
  o->tick_rate = (file_type != nullptr && strcmp(file_type, "WLF") == 0) ? 700 : 560;
  return true;
}

/* MWM: песня, набор волн (ресурс «ИМЯ.MWK») — в ОЗУ OPL4, тик — таймер 1. */
/* MWM, MFM: состояние в PSRAM (таблицы проигрывателя читаются 60 раз в секунду). */
MoonState* moon_state(Opl* o, bool fm) {
  MoonState* s = (MoonState*)host->alloc(sizeof(MoonState), ZM_MEM_PSRAM);
  if (s == nullptr) {
    s = (MoonState*)host->alloc(sizeof(MoonState), ZM_MEM_INTERNAL);
  }
  if (s == nullptr) {
    host->log("opl: no %u bytes for the MoonBlaster player", (unsigned)sizeof(MoonState));
    return nullptr;
  }
  memset(s, 0, sizeof(*s));
  o->moon = s;
  s->opl = o;
  s->fm = fm;
  s->wave_out.context = s;
  s->wave_out.wave = moon_wave;
  s->wave_out.fm = moon_fm;
  s->wave_out.wait = moon_wait;
  s->fm_out.context = s;
  s->fm_out.wave = moon_wave;
  s->fm_out.fm = moon_fm;
  s->fm_out.wait = moon_wait;
  return s;
}

/* Название — 50 знаков; автора часто отбивают пробелами до конца поля
 * («Darkness          Near Dark 1997»): после трёх пробелов подряд — исполнитель. */
void moon_title(Opl* o, const char* name) {
  copy_text(o->title, sizeof(o->title), name);
  char* gap = strstr(o->title, "   ");
  if (gap != nullptr && gap != o->title) {
    const char* author = gap;
    while (*author == ' ') {
      ++author;
    }
    if (*author != 0) {
      copy_text(o->artist, sizeof(o->artist), author);
      *gap = 0;
    }
  }
}

/* MWM: песня, набор волн (ресурс «ИМЯ.MWK») — в ОЗУ OPL4, тик — таймер 1. */
bool open_mwm(Opl* o) {
  MoonState* s = moon_state(o, false);
  if (s == nullptr) {
    return false;
  }
  if (!mwm::parse(o->data, o->size, &s->wave_song)) {
    host->log("opl: MWM of %u bytes is damaged", (unsigned)o->size);
    return false;
  }
  if (!add_chip(o, CHIP_YMF278B, 33868800, 0)) {
    return false;
  }
  const uint8_t* kit = nullptr;
  size_t kit_size = 0;
  if (s->wave_song.kit[0] != 0) {
    char name[16];
    snprintf(name, sizeof(name), "%s.MWK", s->wave_song.kit);
    if (host->resource == nullptr || host->resource(name, &kit, &kit_size) != ZM_OK) {
      host->log("opl: wave kit %s not loaded (the plugin sends it from the song's folder): "
                "its waves stay silent", name);
      kit = nullptr;
      snprintf(o->album, sizeof(o->album), "Kit %s missing", name);
    } else if (mwm::load_kit(kit, (uint32_t)kit_size, nullptr, nullptr, 0) == 0) {
      host->log("opl: wave kit %s of %u bytes is damaged", name, (unsigned)kit_size);
      kit = nullptr;
      snprintf(o->album, sizeof(o->album), "Kit %s damaged", name);
    } else {
      snprintf(o->album, sizeof(o->album), "Kit %s", name);
    }
  }
  Chip* chip = o->chips[0];
  chip->ram_want = mwm::load_kit(kit, (uint32_t)kit_size, nullptr, nullptr, 0);
  if (!chip->ensure_ram() ||
      mwm::load_kit(kit, (uint32_t)kit_size, &s->kit, chip->ram, chip->ram_size) == 0) {
    return false;
  }
  moon_title(o, s->wave_song.name);
  o->format = FORMAT_MWM;
  o->start = 0;
  o->end = o->size;
  o->tick_rate = MOON_RATE;
  s->wait = 57 * mwm::timer_count(&s->wave_song);
  const uint8_t length = s->wave_song.info[0];
  const uint8_t loop = s->wave_song.info[1];
  o->loop = loop != 255 && loop <= length ? 1 : 0;
  return true;
}

/* MFM: FM OPL4 (голоса — в песне) и 6 дорожек волн ПЗУ; тик — 60 или 50 Гц. */
bool open_mfm(Opl* o) {
  MoonState* s = moon_state(o, true);
  if (s == nullptr) {
    return false;
  }
  if (!mfm::parse(o->data, o->size, &s->fm_song)) {
    host->log("opl: MFM of %u bytes is damaged", (unsigned)o->size);
    return false;
  }
  if (!add_chip(o, CHIP_YMF278B, 33868800, 0)) {
    return false;
  }
  moon_title(o, s->fm_song.name);
  o->format = FORMAT_MFM;
  o->start = 0;
  o->end = o->size;
  o->tick_rate = MOON_RATE;
  s->wait = 57 * mfm::timer_count(&s->fm_song);
  const uint8_t length = s->fm_song.info[0];
  const uint8_t loop = s->fm_song.info[1];
  o->loop = loop != 255 && loop <= length ? 1 : 0;
  return true;
}

/* Проход open: длительность, место повтора, каналы с нотами. */
void scan(Opl* o) {
  o->scanning = true;
  o->loop_seen = false;
  o->pos = o->start;
  o->tick = 0;
  if (o->format == FORMAT_MWM || o->format == FORMAT_MFM) {
    moon_begin(o, false);                      /* до конца позиций, без повтора */
    o->loop_seen = true;                       /* повтор MWM — позиция, а не место в файле */
  }
  for (;;) {
    const uint32_t wait = step(o);
    if (wait == END) {
      break;
    }
    o->tick += wait;
  }
  o->total_ticks = o->tick;
  o->scanning = false;
  /* Повтор короче 0,1 с — не повтор (иначе вечное ожидание без нот). */
  if (o->loop != 0 && (!o->loop_seen || o->total_ticks - o->loop_ticks < o->tick_rate / 10)) {
    o->loop = 0;
  }
  const uint64_t total = frame_at(o, o->total_ticks);
  if (o->loop != 0) {
    o->fade_frame = total;
    o->end_frame = total + (uint64_t)FADE_MS * o->rate / 1000;
    o->song_frame = o->end_frame;
  } else {
    o->fade_frame = 0;
    o->end_frame = total + (uint64_t)TAIL_MS * o->rate / 1000;   /* и хвост отзвука */
    o->song_frame = total;
  }
  o->scan_end = o->end_frame;
  /* Индикаторы: каналы с нотами по порядку микросхем (нот нет — все каналы). */
  o->meter_count = 0;
  for (int c = 0; c < o->chip_count && o->meter_count < METERS; ++c) {
    const uint32_t channels = kinds[o->chips[c]->type].channels;
    uint64_t used = o->used[c];
    if (used == 0) {
      used = (channels >= 64) ? ~0ull : ((1ull << channels) - 1);
    }
    for (uint32_t ch = 0; ch < channels && o->meter_count < METERS; ++ch) {
      if ((used >> ch) & 1) {
        o->meters[o->meter_count].chip = (uint8_t)c;
        o->meters[o->meter_count].channel = (uint8_t)ch;
        o->meter_count++;
      }
    }
  }
}

/* «YMF262 (OPL3)», «2x YM3812 (OPL2)», «YMF278B+YM2413». */
void describe(Opl* o) {
  char text[24];
  size_t used = 0;
  int types = 0;
  for (int t = 0; t < CHIP_TYPES; ++t) {
    types += o->by_type[t][0] != nullptr;
  }
  for (int c = 0; c < o->chip_count; ++c) {
    const ChipType type = o->chips[c]->type;
    if (o->by_type[type][0] != o->chips[c]) {
      continue;                                 /* вторая того же типа */
    }
    const bool dual = o->by_type[type][1] != nullptr;
    char part[24];
    size_t n = 0;
    if (used != 0) part[n++] = '+';
    if (dual) {
      memcpy(part + n, "2x ", 3);
      n += 3;
    }
    const size_t name = strlen(kinds[type].name);
    memcpy(part + n, kinds[type].name, name);
    n += name;
    if (types == 1) {
      const size_t family = strlen(kinds[type].family);
      part[n++] = ' ';
      part[n++] = '(';
      memcpy(part + n, kinds[type].family, family);
      n += family;
      part[n++] = ')';
    }
    if (used + n < sizeof(text)) {
      memcpy(text + used, part, n);
      used += n;
    }
  }
  text[used] = 0;
  memcpy(o->format_text, "OPL ", 4);
  copy_text(o->format_text + 4, sizeof(o->format_text) - 4, text);
}

/* --- таблица декодера --------------------------------------------------------- */

const char* file_type() {
  if (host->size >= offsetof(zm_host_t, file_type) + sizeof(host->file_type) &&
      host->file_type != nullptr) {
    return host->file_type;
  }
  return "";
}

int probe(const uint8_t* head, size_t length) {
  if (length >= 4 && memcmp(head, "Vgm ", 4) == 0) {
    return 100;
  }
  if (length >= 8 && memcmp(head, "DBRAWOPL", 8) == 0) {
    return 100;
  }
  if (length >= 6 && (memcmp(head, "MBMS\x10\x08", 6) == 0 || memcmp(head, "MBMS\x10\x01", 6) == 0)) {
    return 100;                                  /* MoonBlaster Wave, MoonBlaster FM */
  }
  if (length >= 2 && head[0] == 0x1F && head[1] == 0x8B) {
    return 50;                                   /* gzip: VGZ */
  }
  return 0;
}

void close_source(void* instance);

void* open_source(uint32_t file_size, const uint8_t* file) {
  if (file == nullptr || file_size == 0) {
    return nullptr;
  }
  Opl* o = (Opl*)host->alloc(sizeof(Opl), ZM_MEM_INTERNAL);
  if (o == nullptr) {
    o = (Opl*)host->alloc(sizeof(Opl), ZM_MEM_PSRAM);
  }
  worker = host->size >= offsetof(zm_host_t, worker_wait) + sizeof(host->worker_wait) &&
           host->worker_start != nullptr && host->worker_wait != nullptr;
  wave_cycles = 0;
  if (o == nullptr) {
    return nullptr;
  }
  memset(o, 0, sizeof(*o));
  o->data = file;
  o->size = file_size;
  o->gain = 1.0f;
  if (file_size >= 18 && file[0] == 0x1F && file[1] == 0x8B) {
    /* VGZ: длина распакованного — в последних четырёх байтах gzip. */
    const uint32_t inflated = le32(file + file_size - 4);
    if (inflated < 0x40 || inflated > INFLATED_MAX) {
      host->log("opl: gzip of %u bytes: bad length %u", (unsigned)file_size, (unsigned)inflated);
      close_source(o);
      return nullptr;
    }
    o->inflated = (uint8_t*)host->alloc(inflated, ZM_MEM_PSRAM);
    if (o->inflated == nullptr) {
      host->log("opl: no %u bytes to unpack the VGZ", (unsigned)inflated);
      close_source(o);
      return nullptr;
    }
    const size_t got = em_inflate(file, file_size, o->inflated, inflated);
    if (got != inflated) {
      host->log("opl: VGZ of %u bytes is damaged", (unsigned)file_size);
      close_source(o);
      return nullptr;
    }
    o->data = o->inflated;
    o->size = inflated;
  }
  bool ok;
  const char* type = file_type();
  if (o->size >= 4 && memcmp(o->data, "Vgm ", 4) == 0) {
    ok = open_vgm(o);
  } else if (o->size >= 8 && memcmp(o->data, "DBRAWOPL", 8) == 0) {
    ok = open_dro(o);
  } else if (o->size >= 6 && memcmp(o->data, "MBMS\x10\x08", 6) == 0) {
    ok = open_mwm(o);
  } else if (o->size >= 6 && memcmp(o->data, "MBMS\x10\x01", 6) == 0) {
    ok = open_mfm(o);
  } else if (strcmp(type, "IMF") == 0 || strcmp(type, "WLF") == 0 ||
             (o->size >= 5 && memcmp(o->data, "ADLIB", 5) == 0)) {
    ok = open_imf(o, type);
  } else {
    host->log("opl: %u bytes are not VGM, DRO, IMF, MWM or MFM", (unsigned)o->size);
    ok = false;
  }
  if (!ok) {
    close_source(o);
    return nullptr;
  }
  /* Частота вывода — первой микросхемы; остальные к ней подстраиваются. */
  o->rate = o->chips[0]->rate;
  /* OPL3 бывает уже громким: до замера нет стартовых +9 дБ трекера. */
  zm_level_init(&o->level, o->rate, 0.0f, LEVEL_MIN_DB);
  switch (o->chips[0]->type) {
    case CHIP_YM3526: case CHIP_Y8950: case CHIP_YM3812: case CHIP_YM2413: case CHIP_VRC7:
      o->gain *= QUIET_GAIN;
      break;
    default:
      break;
  }
  for (int c = 0; c < o->chip_count; ++c) {
    Chip* chip = o->chips[c];
    chip->step = chip->rate == o->rate ? 0x10000
                                       : (uint32_t)(((uint64_t)chip->rate << 16) / o->rate);
  }
  /* ПЗУ таблиц волн OPL4 — ресурс (его присылает плагин). */
  for (int i = 0; i < 2; ++i) {
    Chip* opl4 = o->by_type[CHIP_YMF278B][i];
    if (opl4 == nullptr) {
      continue;
    }
    const uint8_t* rom = nullptr;
    size_t rom_size = 0;
    if (host->resource != nullptr && host->resource(ROM_NAME, &rom, &rom_size) == ZM_OK) {
      opl4->rom = rom;
      opl4->rom_size = (uint32_t)(rom_size < OPL4_ROM_MAX ? rom_size : OPL4_ROM_MAX);
      opl4->memory_changed();
    } else if (i == 0) {
      host->log("opl: " ROM_NAME " not loaded yet (the plugin sends it from /WC/ESP/): "
                "OPL4 plays only the waves of its RAM");
    }
  }
  scan(o);
  describe(o);
  restart(o);
  host->log("opl: %s, %u Hz, %u ms%s, %u channels with notes", o->format_text, (unsigned)o->rate,
            (unsigned)(o->song_frame * 1000 / o->rate), o->loop != 0 ? " with a fade" : "",
            (unsigned)o->meter_count);
  return o;
}

uint32_t position_ms(const Opl* o) { return (uint32_t)(o->frames * 1000 / o->rate); }

int render(void* instance, float* out, uint32_t max_frames, uint32_t* frames) {
  Opl* o = (Opl*)instance;
  *frames = 0;
  if (o->frames >= o->end_frame) {
    return ZM_RENDER_END;
  }
  const uint32_t start = cycle_count();
  const uint64_t first_frame = o->frames;
  const uint32_t done = produce(o, out, max_frames);
  if (done == 0) {
    return ZM_RENDER_END;
  }
  zm_level_process(&o->level, out, done);
  /* Служебное затухание — после нормализации: не попадает в замер и не
   * компенсируется усилением/ограничителем. produce(NULL) при seek его не трогает. */
  if (o->fade_frame != 0) {
    for (uint32_t i = 0; i < done; ++i) {
      const uint64_t at = first_frame + i;
      if (at >= o->fade_frame) {
        const float k = 1.0f - (float)(at - o->fade_frame) / (float)(o->end_frame - o->fade_frame);
        out[2 * i] *= k;
        out[2 * i + 1] *= k;
      }
    }
  }
  const uint32_t spent = cycle_count() - start;
  o->cycles += spent;
  o->cycled_frames += done;
  if (spent / done > o->cycles_max) {
    o->cycles_max = spent / done;
  }
  *frames = done;
  return ZM_RENDER_OK;
}

/* Перемотка на delta мс: команды с начала без звука. */
int seek(void* instance, int32_t delta, uint32_t* file_offset) {
  Opl* o = (Opl*)instance;
  *file_offset = UINT32_MAX;
  int64_t target = (int64_t)position_ms(o) + delta;
  const int64_t duration = (int64_t)(o->end_frame * 1000 / o->rate);
  if (target > duration) {
    target = duration;
  }
  if (target < 0) {
    target = 0;
  }
  const uint64_t frame = (uint64_t)target * o->rate / 1000;
  const uint64_t settle = (uint64_t)SETTLE_MS * o->rate / 1000;
  const uint64_t quiet = frame > settle ? frame - settle : 0;
  restart(o);
  while (!o->stream_end && frame_at(o, o->tick) < quiet) {
    const uint32_t wait = step(o);
    if (wait == END) {
      o->stream_end = true;
      break;
    }
    o->tick += wait;
  }
  o->frames = quiet < o->end_frame ? quiet : o->end_frame;
  while (o->frames < frame && o->frames < o->end_frame) {
    const uint64_t left = frame - o->frames;
    if (produce(o, nullptr, (uint32_t)(left < BLOCK ? left : BLOCK)) == 0) {
      break;
    }
  }
  zm_level_discontinuity(&o->level);
  return ZM_OK;
}

void info(const void* instance, zm_info_t* out) {
  const Opl* o = (const Opl*)instance;
  memset(out, 0, sizeof(*out));
  copy_text(out->format, sizeof(out->format), o->format_text);
  out->rate = o->rate;
  out->channels = 2;
  out->duration_ms = (uint32_t)(o->song_frame * 1000 / o->rate);
  out->position_ms = position_ms(o);
  copy_text(out->title, sizeof(out->title), o->title);
  copy_text(out->artist, sizeof(out->artist), o->artist);
  copy_text(out->album, sizeof(out->album), o->album);
  out->year = o->year;
  out->voices = o->meter_count;
}

int levels(const void* instance, uint8_t* out, int max) {
  const Opl* o = (const Opl*)instance;
  int count = o->meter_count < max ? o->meter_count : max;
  for (int i = 0; i < count; ++i) {
    out[i] = o->chips[o->meters[i].chip]->level(o->meters[i].channel);
  }
  return count;
}

void close_source(void* instance) {
  Opl* o = (Opl*)instance;
  if (o->level.ready && o->level.blocks >= ZM_LEVEL_WARMUP_STEPS) {
    const int song = (int)lroundf(o->level.song_lufs * 10.0f);
    const int gain = (int)lroundf(o->level.gain_db * 10.0f);
    host->log("opl: level %s%d.%d LUFS before gain, gain %s%d.%d dB (target %d LUFS)",
              song < 0 ? "-" : "", abs(song) / 10, abs(song) % 10, gain < 0 ? "-" : "+",
              abs(gain) / 10, abs(gain) % 10, (int)ZM_LEVEL_TARGET);
  }
  if (o->cycled_frames != 0) {
    /* Сколько стоит синтез: кадр в реальном времени — 240 МГц / частота. */
    host->log("opl: %s, cycles per frame %u average, %u worst (real time: %u)", o->format_text,
              (unsigned)(o->cycles / o->cycled_frames), (unsigned)o->cycles_max,
              (unsigned)(240000000u / (o->rate ? o->rate : 1)));
    if (wave_cycles != 0) {
      host->log("opl: waves on core 1: %u cycles per frame average",
                (unsigned)(wave_cycles / o->cycled_frames));
    }
  }
  for (int i = 0; i < o->chip_count; ++i) {
    delete_chip(o->chips[i]);
  }
  host->free(o->waveforms);
  host->free(o->inflated);
  host->free(o->moon);
  host->free(o);
}

}  // namespace

#if defined(OPL_COMPARE)
/* Стенд сравнения: сколько отсчётов сверено и сколько не сошлось. */
extern "C" void opl_compare_stats(unsigned long long* samples, unsigned long long* mismatches) {
  *samples = compare_samples;
  *mismatches = compare_mismatches;
}
#endif

/* Без библиотеки C++ в модуле: память каналов ymfm — у плеера. */
#if defined(__XTENSA__)
void* operator new(size_t size) { return fast_alloc(size); }
void* operator new[](size_t size) { return fast_alloc(size); }
void operator delete(void* pointer) noexcept { if (pointer != nullptr) host->free(pointer); }
void operator delete[](void* pointer) noexcept { if (pointer != nullptr) host->free(pointer); }
void operator delete(void* pointer, size_t) noexcept { if (pointer != nullptr) host->free(pointer); }
void operator delete[](void* pointer, size_t) noexcept { if (pointer != nullptr) host->free(pointer); }
extern "C" void __cxa_pure_virtual() {
  for (;;) {
  }
}
namespace std {
/* На неё ссылается std::string отладочной печати ymfm (log_keyon); не зовётся. */
void __throw_length_error(const char*) {
  for (;;) {
  }
}
}  // namespace std
#endif

extern "C" int main(int argc, char* argv[]) {
  if (argc < 3) {
    return 0;
  }
  host = (const zm_host_t*)argv[1];
  table.size = sizeof(table);
  table.abi_major = ZM_SOURCE_ABI_MAJOR;
  table.abi_minor = ZM_SOURCE_ABI_MINOR;
  table.name = "opl";
  table.version = "1.5";
  table.extensions = "VGM VGZ DRO IMF WLF MWM MFM";
  table.input = ZM_INPUT_WHOLE;
  table.keeps_file = 1;
  table.seek_unit = ZM_SEEK_MS;
  table.resources = ROM_NAME;
  table.probe = probe;
  table.open = open_source;
  table.render = render;
  table.seek = seek;
  table.info = info;
  table.levels = levels;
  table.close = close_source;
  *(const zm_source_t**)argv[2] = &table;
  return ZM_SOURCE_ABI_MAJOR;
}
