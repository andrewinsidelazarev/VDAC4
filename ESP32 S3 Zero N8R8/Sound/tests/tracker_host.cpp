// Стенд компонента tracker для test_tracker_host.py: модуль MOD из двух
// паттернов собирается в памяти так же, как его прислал бы Z80, и играет до
// конца песни тем же путём, что AudioPlayer::startModule/moduleStep. Потом —
// эффекты XM на однo-канальных XM, собранных здесь же: частота и панорама
// канала по тикам.
// Вывод — строки "ключ значение".
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <array>
#include <cmath>
#include <vector>

#include "tracker.h"

namespace {

void* hostAlloc(size_t size) { return malloc(size); }

void putBe16(std::vector<uint8_t>& file, size_t offset, uint16_t value) {
  file[offset] = static_cast<uint8_t>(value >> 8);
  file[offset + 1] = static_cast<uint8_t>(value);
}

// Протрекер M.K.: четыре канала, один отсчёт (меандр 64 байта, петля целиком),
// два паттерна по 64 строки. Нота C-3 (период 428) в начале каждого паттерна.
std::vector<uint8_t> makeMod() {
  const size_t patterns = 2;
  const size_t sampleBytes = 64;
  std::vector<uint8_t> file(1084 + patterns * 1024 + sampleBytes, 0);
  memcpy(file.data(), "vdac4 test tune", 15);
  const size_t sample = 20;               // заголовок отсчёта 1
  memcpy(file.data() + sample, "square", 6);
  putBe16(file, sample + 22, sampleBytes / 2);   // длина, слов
  file[sample + 24] = 0;                  // подстройка
  file[sample + 25] = 64;                 // громкость
  putBe16(file, sample + 26, 0);          // начало петли
  putBe16(file, sample + 28, sampleBytes / 2);   // длина петли
  for (int index = 1; index < 31; ++index) {
    putBe16(file, 20 + 30 * index + 28, 1);      // пустые отсчёты: петля 1 слово
  }
  file[950] = static_cast<uint8_t>(patterns);    // длина песни
  file[951] = 127;
  file[952] = 0;
  file[953] = 1;
  memcpy(file.data() + 1080, "M.K.", 4);
  for (size_t pattern = 0; pattern < patterns; ++pattern) {
    uint8_t* row0 = file.data() + 1084 + pattern * 1024;
    // [отсчёт hi | период hi][период lo][отсчёт lo | эффект][параметр]
    row0[0] = 0x00 | (428 >> 8);
    row0[1] = 428 & 0xFF;
    row0[2] = 0x10;
  }
  uint8_t* data = file.data() + 1084 + patterns * 1024;
  for (size_t index = 0; index < sampleBytes; ++index) {
    data[index] = index < sampleBytes / 2 ? 0x40 : 0xC0;
  }
  return file;
}

// XM: один канал, один инструмент (меандр 32 отсчёта, петля), один узор;
// строки — [нота, инструмент, громкость, эффект, параметр], без упаковки.
using Row = std::array<uint8_t, 5>;

std::vector<uint8_t> makeXm(const std::vector<Row>& rows) {
  std::vector<uint8_t> file;
  auto bytes = [&](const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    file.insert(file.end(), p, p + size);
  };
  auto u8 = [&](uint8_t value) { file.push_back(value); };
  auto u16 = [&](uint16_t value) { u8(static_cast<uint8_t>(value)); u8(static_cast<uint8_t>(value >> 8)); };
  auto u32 = [&](uint32_t value) { u16(static_cast<uint16_t>(value)); u16(static_cast<uint16_t>(value >> 16)); };
  auto zeros = [&](size_t count) { file.insert(file.end(), count, 0); };
  bytes("Extended Module: ", 17);
  bytes("vdac4 effects       ", 20);
  u8(0x1A);
  bytes("FastTracker v2.00   ", 20);
  u16(0x0104);
  u32(276);
  u16(1);                                 // длина песни
  u16(0);                                 // повтор
  u16(1);                                 // каналов
  u16(1);                                 // узоров
  u16(1);                                 // инструментов
  u16(1);                                 // линейные частоты
  u16(6);                                 // темп
  u16(125);                               // BPM
  zeros(256);                             // таблица порядка
  u32(9);
  u8(0);
  u16(static_cast<uint16_t>(rows.size()));
  u16(static_cast<uint16_t>(rows.size() * 5));
  for (const Row& row : rows) {
    bytes(row.data(), row.size());
  }
  const size_t instrument = file.size();
  u32(263);
  zeros(22);
  u8(0);
  u16(1);                                 // отсчётов
  u32(40);
  zeros(263 - (file.size() - instrument));
  u32(32);                                // длина
  u32(0);                                 // начало петли
  u32(32);                                // длина петли
  u8(64);                                 // громкость
  u8(0);                                  // подстройка
  u8(1);                                  // петля вперёд, 8 бит
  u8(128);                                // панорама
  u8(0);                                  // относительная нота
  u8(0);
  zeros(22);
  int8_t previous = 0;
  for (int index = 0; index < 32; ++index) {
    const int8_t value = index < 16 ? 0x40 : -0x40;
    u8(static_cast<uint8_t>(value - previous));   // отсчёты XM — разностями
    previous = value;
  }
  return file;
}

// Сыграть ticks тиков (125 BPM: 882 кадра на тик) и вывести после каждого
// частоту и панораму канала 1, а в конце — BPM и темп.
void playTicks(const char* name, const std::vector<Row>& rows, int ticks) {
  std::vector<uint8_t> file = makeXm(rows);
  xm_context_t* context = nullptr;
  if (xm_create_context_safe(&context, file.data(), file.size(), 44100, hostAlloc) < 0 ||
      context == nullptr) {
    printf("%s_load failed\n", name);
    return;
  }
  std::vector<float> mix(882 * 2);
  printf("%s_frequency", name);
  std::vector<float> panning;
  for (int tick = 0; tick < ticks; ++tick) {
    xm_render(context, mix.data(), 882);
    printf(" %.3f", xm_get_frequency_of_channel(context, 1));
    panning.push_back(xm_get_panning_of_channel(context, 1));
  }
  printf("\n%s_panning", name);
  for (float value : panning) {
    printf(" %.3f", value);
  }
  uint16_t bpm = 0;
  uint16_t tempo = 0;
  xm_get_playing_speed(context, &bpm, &tempo);
  printf("\n%s_speed %u %u\n", name, static_cast<unsigned>(bpm), static_cast<unsigned>(tempo));
  xm_free_context(context);
}

// Эффекты XM: тон-портаменто C-4 -> C-5 с глиссандо (E31) и без (E30);
// Y (панбрелло OpenMPT) и Z (макрос MIDI) — не скорость и не темп S3M.
void xmEffects() {
  const uint8_t c4 = 49, c5 = 61;
  for (int gliss = 0; gliss < 2; ++gliss) {
    std::vector<Row> rows(16, Row{0, 0, 0, 0, 0});
    rows[0] = Row{c4, 1, 0, 0x0E, static_cast<uint8_t>(0x30 | gliss)};
    rows[1] = Row{c5, 0, 0, 0x03, 0x08};
    for (int row = 2; row < 8; ++row) {
      rows[row] = Row{0, 0, 0, 0x03, 0x00};
    }
    playTicks(gliss ? "gliss_on" : "gliss_off", rows, 48);
  }
  std::vector<Row> rows(16, Row{0, 0, 0, 0, 0});
  rows[0] = Row{c4, 1, 0, 0, 0};
  for (int row = 1; row < 6; ++row) {
    rows[row] = Row{0, 0, 0, 34, 0x48};   // Y48: панбрелло, скорость 4, глубина 8
  }
  rows[6] = Row{0, 0, 0, 35, 0x90};       // Z90: макрос без действия
  playTicks("ymz", rows, 48);

  // Zxx — резонансный фильтр: меандр C-7 (2 кГц, обертоны до 20 кГц) через
  // срез Z20 (~600 Гц) заметно тише; Z30 с резонансом Z8F — конечные числа;
  // Z7F без резонанса — фильтра нет.
  const struct { const char* name; uint8_t first; uint8_t second; } filters[] = {
    {"filter_off", 0, 0}, {"filter_low", 0x20, 0}, {"filter_resonant", 0x30, 0x8F},
    {"filter_open", 0x7F, 0x80},
  };
  for (const auto& filter : filters) {
    std::vector<Row> tone(16, Row{0, 0, 0, 0, 0});
    tone[0] = Row{85, 1, 0, static_cast<uint8_t>(filter.first ? 35 : 0), filter.first};
    if (filter.second) {
      tone[1] = Row{0, 0, 0, 35, filter.second};
    }
    std::vector<uint8_t> file = makeXm(tone);
    xm_context_t* context = nullptr;
    xm_create_context_safe(&context, file.data(), file.size(), 44100, hostAlloc);
    std::vector<float> mix(882 * 2);
    double energy = 0;
    bool finite = true;
    for (int tick = 0; tick < 24; ++tick) {
      xm_render(context, mix.data(), 882);
      if (tick < 12) {
        continue;                         // после Z8F (вторая строка) и разгона фильтра
      }
      for (float value : mix) {
        finite = finite && std::isfinite(value);
        energy += static_cast<double>(value) * value;
      }
    }
    printf("%s %.5f %d\n", filter.name, sqrt(energy / (12.0 * 882 * 2)), finite ? 1 : 0);
    xm_free_context(context);
  }
}

}  // namespace

int main() {
  std::vector<uint8_t> file = makeMod();
  printf("sane %d\n", mod_check_sanity_preload(file.data(), file.size()) == 0);
  xm_context_t* context = nullptr;
  size_t needed = 0;
  const int result = mod_create_context_safe(&context, file.data(), file.size(),
                                             44100, hostAlloc, &needed);
  printf("result %d\n", result);
  if (result < 0 || context == nullptr) {
    return 1;
  }
  printf("name %s\n", xm_get_module_name(context));
  printf("length %u\n", static_cast<unsigned>(xm_get_module_length(context)));
  printf("channels %u\n", static_cast<unsigned>(xm_get_number_of_channels(context)));
  xm_set_max_loop_count(context, 1);
  // Длительность прогоном — до звука: после него песня играет с начала.
  uint32_t orderMs[256];
  const uint32_t scanned = xm_scan_song(context, orderMs, 3600000);

  const size_t frames = 512;
  std::vector<float> mix(frames * 2);
  double energy = 0;
  float peak = 0;
  size_t rendered = 0;
  unsigned second = 0;
  while (xm_get_loop_count(context) == 0 && rendered < 44100u * 60u) {
    xm_render(context, mix.data(), frames);
    for (float value : mix) {
      energy += static_cast<double>(value) * value;
      peak = fabsf(value) > peak ? fabsf(value) : peak;
    }
    rendered += frames;
    uint8_t index = 0;
    xm_get_position(context, &index, nullptr, nullptr, nullptr);
    if (index == 1 && second == 0) {
      second = static_cast<unsigned>(rendered);
    }
  }
  printf("frames %u\n", static_cast<unsigned>(rendered));
  printf("scanned %u\n", static_cast<unsigned>(scanned));
  printf("order1 %u\n", static_cast<unsigned>(orderMs[1]));
  printf("second %u\n", second);
  printf("rms %.4f\n", sqrt(energy / (rendered * 2.0)));
  printf("peak %.4f\n", peak);

  // Перемотка: на вторую позицию, как по команде AUDIO_SEEK.
  xm_free_context(context);
  context = nullptr;
  mod_create_context_safe(&context, file.data(), file.size(), 44100, hostAlloc,
                          &needed);
  xm_render(context, mix.data(), frames);
  xm_seek(context, 1, 0, 0);
  xm_render(context, mix.data(), frames);
  uint8_t index = 0;
  uint8_t row = 0;
  xm_get_position(context, &index, nullptr, &row, nullptr);
  printf("seek %u %u\n", static_cast<unsigned>(index), static_cast<unsigned>(row));
  xm_free_context(context);

  // Не модуль: загрузчик MOD его не принимает.
  std::vector<uint8_t> junk(4000, 0x55);
  printf("junk %d\n", mod_check_sanity_preload(junk.data(), junk.size()) == 0);
  xmEffects();
  return 0;
}
