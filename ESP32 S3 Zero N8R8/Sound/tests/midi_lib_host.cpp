// Разбор MIDI (TinyMidiLoader) и синтезатор SoundFont2 (TinySoundFont) MIDI.ELF — на ПК.
// Вызов: midi_lib_host — проверки исправлений аудита MIDI (MIDI.ELF 1.6), по одной на
// строку вывода «ok <проверка>» или «FAIL <проверка>: <что не так>». Банк SF2 собирается
// здесь же (sf2_build): маленький, с нужными зонами. Память библиотек — через свой
// распределитель: он считает живые блоки (утечки), самый большой блок и умеет отказать на
// N-м выделении (нехватка памяти). Порча банка проверяется на образе вплотную к странице
// без доступа: чтение за концом роняет программу.
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <string>
#include <vector>

namespace {

// --- распределитель ---------------------------------------------------------------
long g_live = 0;               // живых блоков
size_t g_largest = 0;          // самый большой запрошенный блок
long g_counter = 0;            // выделений с последнего сброса
long g_fail_at = -1;           // это выделение откажет (−1 — нет)
long g_reallocs = 0;           // realloc живого блока (рост массива)

struct Header { size_t size; size_t pad; };

void* lib_malloc(size_t size) {
  const long n = g_counter++;
  if (g_fail_at >= 0 && n == g_fail_at) return nullptr;
  if (size > g_largest) g_largest = size;
  Header* h = static_cast<Header*>(std::malloc(sizeof(Header) + (size ? size : 1)));
  if (!h) return nullptr;
  h->size = size;
  ++g_live;
  return h + 1;
}

void lib_free(void* pointer) {
  if (!pointer) return;
  --g_live;
  std::free(static_cast<Header*>(pointer) - 1);
}

void* lib_realloc(void* pointer, size_t size) {
  if (!pointer) return lib_malloc(size);
  ++g_reallocs;
  const long n = g_counter++;
  if (g_fail_at >= 0 && n == g_fail_at) return nullptr;
  if (size > g_largest) g_largest = size;
  Header* h = static_cast<Header*>(std::realloc(static_cast<Header*>(pointer) - 1, sizeof(Header) + (size ? size : 1)));
  if (!h) return nullptr;
  h->size = size;
  return h + 1;
}

void alloc_reset() { g_counter = 0; g_fail_at = -1; g_largest = 0; g_reallocs = 0; }

}  // namespace

#define TSF_MALLOC(size) lib_malloc(size)
#define TSF_REALLOC(pointer, size) lib_realloc(pointer, size)
#define TSF_FREE(pointer) lib_free(pointer)
#define TSF_NO_STDIO
#define TSF_IMPLEMENTATION
#include "tsf.h"

#define TML_MALLOC(size) lib_malloc(size)
#define TML_REALLOC(pointer, size) lib_realloc(pointer, size)
#define TML_FREE(pointer) lib_free(pointer)
#define TML_NO_STDIO
#define TML_IMPLEMENTATION
#include "tml.h"

namespace {

int g_failures = 0;
std::string g_why;

void report(const char* name, bool ok) {
  if (ok) {
    std::printf("ok %s\n", name);
  } else {
    std::printf("FAIL %s: %s\n", name, g_why.c_str());
    ++g_failures;
  }
  g_why.clear();
}

bool check(bool condition, const std::string& why) {
  if (!condition && g_why.empty()) g_why = why;
  return condition;
}

// --- SMF ----------------------------------------------------------------------------
using Bytes = std::vector<uint8_t>;

void put32be(Bytes& out, uint32_t v) {
  out.push_back(uint8_t(v >> 24)); out.push_back(uint8_t(v >> 16)); out.push_back(uint8_t(v >> 8)); out.push_back(uint8_t(v));
}

void vlq(Bytes& out, uint32_t v) {
  uint8_t tmp[5]; int n = 0;
  tmp[n++] = uint8_t(v & 0x7F);
  while (v >>= 7) tmp[n++] = uint8_t(0x80 | (v & 0x7F));
  while (n) out.push_back(tmp[--n]);
}

Bytes smf(uint16_t format, uint16_t division, const std::vector<Bytes>& tracks, uint32_t header_length = 6) {
  Bytes out = {'M', 'T', 'h', 'd'};
  put32be(out, header_length);
  out.push_back(uint8_t(format >> 8)); out.push_back(uint8_t(format));
  out.push_back(uint8_t(tracks.size() >> 8)); out.push_back(uint8_t(tracks.size()));
  out.push_back(uint8_t(division >> 8)); out.push_back(uint8_t(division));
  for (const Bytes& body : tracks) {
    out.insert(out.end(), {'M', 'T', 'r', 'k'});
    put32be(out, uint32_t(body.size()));
    out.insert(out.end(), body.begin(), body.end());
  }
  return out;
}

void event(Bytes& track, uint32_t delta, std::initializer_list<uint8_t> data) {
  vlq(track, delta);
  track.insert(track.end(), data);
}

void eot(Bytes& track) { event(track, 0, {0xFF, 0x2F, 0x00}); }

struct Event { unsigned time; int type, channel, key; };

std::vector<Event> events_of(tml_message* song) {
  std::vector<Event> out;
  for (tml_message* m = song; m; m = m->next) out.push_back({m->time, m->type, m->channel, m->key});
  return out;
}

tml_message* load(const Bytes& file) { return tml_load_memory(file.data(), int(file.size())); }

// Формат 1: первое по времени событие — у дорожки 1, а дорожка 0 ещё не прочитана.
bool tml_format1_merge_keeps_every_track() {
  Bytes t0, t1;
  event(t0, 480, {0x90, 60, 100});                         // дорожка 0: нота на тике 480
  event(t0, 480, {0x80, 60, 0});
  eot(t0);
  event(t1, 0, {0x91, 64, 100});                           // дорожка 1: нота на тике 0
  event(t1, 960, {0x81, 64, 0});
  eot(t1);
  long live = g_live;
  tml_message* song = load(smf(1, 480, {t0, t1}));
  std::vector<Event> e = events_of(song);
  bool ok = check(song != nullptr, "песня не загрузилась") &&
            check(e.size() == 4, "событий " + std::to_string(e.size()) + ", ждали 4") &&
            check(e[0].type == TML_NOTE_ON && e[0].channel == 1 && e[0].time == 0, "первое — нота канала 1 в 0 мс") &&
            check(e[1].type == TML_NOTE_ON && e[1].channel == 0 && e[1].time == 500, "нота дорожки 0 — в 500 мс") &&
            check(e[2].time == 1000 && e[3].time == 1000, "отпускания — в 1000 мс");
  tml_free(song);
  return ok && check(g_live == live, "утечка памяти");
}

// Темп 500000 мкс заново на каждом тике: 9600 тиков при PPQN 480 — ровно 10 с.
bool tml_tempo_changes_do_not_drift() {
  Bytes t;
  for (int i = 0; i < 9600; ++i) event(t, i ? 1 : 0, {0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20});   // тики 0..9599
  event(t, 1, {0x90, 60, 100});                                                                 // тик 9600
  eot(t);
  tml_message* song = load(smf(0, 480, {t}));
  unsigned note = 0;
  for (tml_message* m = song; m; m = m->next) if (m->type == TML_NOTE_ON) note = m->time;
  tml_free(song);
  return check(song != nullptr, "не загрузилась") && check(note == 10000, "нота в " + std::to_string(note) + " мс, ждали 10000");
}

// Пауза 0x100000 тиков при PPQN 32767 и темпе 120 — 16 с, а не ноль.
bool tml_long_delta_is_kept() {
  Bytes t;
  event(t, 0, {0x90, 60, 100});
  event(t, 0x100000, {0x80, 60, 0});
  eot(t);
  tml_message* song = load(smf(0, 32767, {t}));
  std::vector<Event> e = events_of(song);
  tml_free(song);
  return check(e.size() == 2, "событий не 2") && check(e[1].time >= 15999 && e[1].time <= 16001, "отпускание в " + std::to_string(e.empty() ? 0 : e.back().time) + " мс");
}

// Заголовок: деление 0, формат 2, длина заголовка не 6 (и файл короче её) — отказ.
bool tml_bad_headers_are_refused() {
  Bytes t;
  event(t, 0, {0x90, 60, 100});
  eot(t);
  bool ok = true;
  ok &= check(load(smf(0, 0, {t})) == nullptr, "деление 0 принято");
  ok &= check(load(smf(2, 480, {t})) == nullptr, "формат 2 принят");
  ok &= check(load(smf(0, 480, {t}, 0x106)) == nullptr, "заголовок длиной 0x106 принят");
  Bytes longer = smf(0, 480, {t}, 8);                       // заголовок 8 байтов: два лишних
  longer.insert(longer.begin() + 14, {0xAB, 0xCD});
  tml_message* song = load(longer);
  ok &= check(song != nullptr && song->type == TML_NOTE_ON, "заголовок в 8 байтов не прочитан");
  tml_free(song);
  return ok;
}

// Дорожка с длиной больше файла: память под неё не выделяется, прочие дорожки — есть.
bool tml_huge_track_is_not_allocated() {
  Bytes t;
  event(t, 0, {0x90, 60, 100});
  eot(t);
  Bytes file = smf(1, 480, {t});
  file.insert(file.end(), {'M', 'T', 'r', 'k', 0x7F, 0xFF, 0xFF, 0xF0, 0x00, 0x90});
  file[11] = 2;                                            // дорожек две
  alloc_reset();
  long live = g_live;
  tml_message* song = load(file);
  bool ok = check(song != nullptr && song->type == TML_NOTE_ON, "первая дорожка потеряна") &&
            check(g_largest < 65536, "выделено " + std::to_string(g_largest) + " байтов");
  tml_free(song);
  return ok && check(g_live == live, "утечка");
}

// Running status: после мета-события байт данных — порча (дорожка кончается), а не
// «мета-событие без статуса»; между дорожками он не переходит.
bool tml_running_status_rules() {
  Bytes t0, t1;
  event(t0, 0, {0x90, 60, 100});
  event(t0, 0, {0xFF, 0x01, 0x01, 'A'});                   // текст
  event(t0, 10, {0x3C, 0x00});                             // данные без статуса — испорчено
  event(t0, 10, {0x80, 60, 0});
  eot(t0);
  event(t1, 5, {0x40, 0x64});                              // дорожка начинается с данных
  eot(t1);
  tml_message* song = load(smf(1, 480, {t0, t1}));
  std::vector<Event> e = events_of(song);
  tml_free(song);
  return check(e.size() == 1 && e[0].type == TML_NOTE_ON, "событий " + std::to_string(e.size()) + ", ждали одно нажатие");
}

// Чужой блок между дорожками пропускается.
bool tml_unknown_chunk_is_skipped() {
  Bytes t0, t1;
  event(t0, 0, {0x90, 60, 100}); eot(t0);
  event(t1, 0, {0x91, 62, 100}); eot(t1);
  Bytes file = smf(1, 480, {t0});
  file.insert(file.end(), {'X', 'F', 'I', 'H', 0, 0, 0, 3, 1, 2, 3});
  file.insert(file.end(), {'M', 'T', 'r', 'k'});
  put32be(file, uint32_t(t1.size()));
  file.insert(file.end(), t1.begin(), t1.end());
  file[11] = 2;
  tml_message* song = load(file);
  std::vector<Event> e = events_of(song);
  tml_free(song);
  return check(e.size() == 2, "событий " + std::to_string(e.size()) + ", ждали 2");
}

// Отказ памяти на каждом выделении по очереди: NULL или целая песня, без утечек.
bool tml_out_of_memory_is_clean() {
  Bytes t0, t1;
  for (int i = 0; i < 300; ++i) { event(t0, 1, {0x90, uint8_t(i & 0x7F), 100}); event(t1, 1, {0x91, uint8_t(i & 0x7F), 100}); }
  eot(t0); eot(t1);
  Bytes file = smf(1, 480, {t0, t1});
  alloc_reset();
  tml_free(load(file));
  const long total = g_counter;
  // дорожки, сообщения (одним блоком с 1.7.3), куча слияния
  bool ok = check(total >= 3, "выделений меньше 3");
  for (long n = 0; n < total && ok; ++n) {
    long live = g_live;
    alloc_reset();
    g_fail_at = n;
    tml_message* song = load(file);
    g_fail_at = -1;
    if (song) ok &= check(events_of(song).size() == 600, "при отказе №" + std::to_string(n) + " песня неполная");
    tml_free(song);
    ok &= check(g_live == live, "утечка при отказе №" + std::to_string(n));
  }
  return ok;
}

// --- SF2 ----------------------------------------------------------------------------
struct Gen { uint16_t oper; uint16_t amount; };
using Zone = std::vector<Gen>;
struct Inst { std::vector<Zone> zones; };
struct Preset { uint16_t preset, bank; std::vector<Zone> zones; };
struct Sample { uint32_t start, end, loop_start, loop_end, rate; uint8_t pitch; uint16_t type = 1; };

// Модулятор зоны: в Zone — «генератор» MOD_OPER + номер в g_mods (сборщик кладёт его в pmod/imod).
struct Mod { uint16_t src, dst; int16_t amount; uint16_t amt_src, trans; };
std::vector<Mod> g_mods;
const uint16_t MOD_OPER = 0xF000;
Gen mod(uint16_t src, uint16_t dst, int16_t amount, uint16_t amt_src = 0) {
  g_mods.push_back({src, dst, amount, amt_src, 0});
  return {uint16_t(MOD_OPER + g_mods.size() - 1), 0};
}

enum { G_KEYRANGE = 43, G_VELRANGE = 44, G_ATTEN = 48, G_COARSE = 51, G_SAMPLE = 53, G_MODES = 54,
       G_SCALE = 56, G_EXCLUSIVE = 57, G_ROOT = 58, G_INSTRUMENT = 41, G_FC = 8 };

Gen range(uint16_t oper, uint8_t lo, uint8_t hi) { return {oper, uint16_t(lo | (hi << 8))}; }

void put16(Bytes& out, uint16_t v) { out.push_back(uint8_t(v)); out.push_back(uint8_t(v >> 8)); }
void put32(Bytes& out, uint32_t v) { put16(out, uint16_t(v)); put16(out, uint16_t(v >> 16)); }
void name20(Bytes& out, const char* name) { char b[20] = {0}; std::strncpy(b, name, 19); out.insert(out.end(), b, b + 20); }

Bytes chunk(const char* id, const Bytes& body) {
  Bytes out(id, id + 4);
  put32(out, uint32_t(body.size()));
  out.insert(out.end(), body.begin(), body.end());
  if (body.size() & 1) out.push_back(0);
  return out;
}

Bytes list(const char* type, const Bytes& body) {
  Bytes inner(type, type + 4);
  inner.insert(inner.end(), body.begin(), body.end());
  return chunk("LIST", inner);
}

std::vector<int16_t> sine_samples() {
  std::vector<int16_t> s(2000 + 46);
  for (int i = 0; i < 2000; ++i) s[i] = int16_t(12000 * std::sin(2 * 3.14159265358979 * i / 100.0));
  return s;
}

// smpl_last — блок отсчётов в конце файла (за ним — защитная страница образа Guarded).
Bytes sf2_build(const std::vector<Preset>& presets, const std::vector<Inst>& insts, const std::vector<Sample>& samples,
                bool smpl_last = false) {
  const std::vector<int16_t> pcm = sine_samples();
  Bytes smpl(reinterpret_cast<const uint8_t*>(pcm.data()), reinterpret_cast<const uint8_t*>(pcm.data()) + pcm.size() * 2);
  Bytes phdr, pbag, pmod, pgen, inst, ibag, imod, igen, shdr;
  uint16_t bags = 0, gens = 0, mods = 0;
  auto put_mod = [](Bytes& out, const Mod& m) { put16(out, m.src); put16(out, m.dst); put16(out, uint16_t(m.amount)); put16(out, m.amt_src); put16(out, m.trans); };
  for (const Preset& p : presets) {
    name20(phdr, "P"); put16(phdr, p.preset); put16(phdr, p.bank); put16(phdr, bags); put32(phdr, 0); put32(phdr, 0); put32(phdr, 0);
    for (const Zone& z : p.zones) {
      put16(pbag, gens); put16(pbag, mods); ++bags;
      for (const Gen& g : z) {
        if (g.oper >= MOD_OPER) { put_mod(pmod, g_mods[g.oper - MOD_OPER]); ++mods; continue; }
        put16(pgen, g.oper); put16(pgen, g.amount); ++gens;
      }
    }
  }
  name20(phdr, "EOP"); put16(phdr, 0); put16(phdr, 0); put16(phdr, bags); put32(phdr, 0); put32(phdr, 0); put32(phdr, 0);
  put16(pbag, gens); put16(pbag, mods);
  put16(pgen, 0); put16(pgen, 0);
  put_mod(pmod, {0, 0, 0, 0, 0});
  bags = gens = mods = 0;
  for (const Inst& i : insts) {
    name20(inst, "I"); put16(inst, bags);
    for (const Zone& z : i.zones) {
      put16(ibag, gens); put16(ibag, mods); ++bags;
      for (const Gen& g : z) {
        if (g.oper >= MOD_OPER) { put_mod(imod, g_mods[g.oper - MOD_OPER]); ++mods; continue; }
        put16(igen, g.oper); put16(igen, g.amount); ++gens;
      }
    }
  }
  name20(inst, "EOI"); put16(inst, bags);
  put16(ibag, gens); put16(ibag, mods);
  put16(igen, 0); put16(igen, 0);
  put_mod(imod, {0, 0, 0, 0, 0});
  for (const Sample& s : samples) {
    name20(shdr, "S"); put32(shdr, s.start); put32(shdr, s.end); put32(shdr, s.loop_start); put32(shdr, s.loop_end);
    put32(shdr, s.rate); shdr.push_back(s.pitch); shdr.push_back(0); put16(shdr, 0); put16(shdr, s.type);
  }
  name20(shdr, "EOS"); for (int i = 0; i < 26; ++i) shdr.push_back(0);
  Bytes pdta = chunk("phdr", phdr);
  for (auto [id, body] : {std::pair<const char*, const Bytes*>{"pbag", &pbag}, {"pmod", &pmod}, {"pgen", &pgen}, {"inst", &inst},
                          {"ibag", &ibag}, {"imod", &imod}, {"igen", &igen}, {"shdr", &shdr}}) {
    Bytes c = chunk(id, *body);
    pdta.insert(pdta.end(), c.begin(), c.end());
  }
  Bytes body = {'s', 'f', 'b', 'k'};
  Bytes sdta = list("sdta", chunk("smpl", smpl)), pd = list("pdta", pdta);
  if (smpl_last) std::swap(sdta, pd);
  body.insert(body.end(), sdta.begin(), sdta.end());
  body.insert(body.end(), pd.begin(), pd.end());
  Bytes out = {'R', 'I', 'F', 'F'};
  put32(out, uint32_t(body.size()));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

const Sample kSine = {0, 2000, 100, 1900, 44100, 60};

// Простой банк: пресет 0 банка 0 — инструмент 0 с одной зоной (сэмпл 0, петля).
std::vector<Inst> one_instrument(Zone zone) { return {Inst{{zone}}}; }
Zone looped_zone(Zone extra = {}) {
  Zone z = extra;
  z.push_back({G_MODES, 1});
  z.push_back({G_SAMPLE, 0});
  return z;
}

// tsf_load_memory берёт отсчёты прямо из буфера (VDAC4): буфер живёт, пока жив tsf, — здесь
// до конца программы.
tsf* load_sf2(const Bytes& bank) {
  static std::list<Bytes> keep;
  keep.push_back(bank);
  return tsf_load_memory(keep.back().data(), int(keep.back().size()));
}

// Глобальная зона пресета сужает клавиши до 60–72, у инструмента зоны 0–59 и 60–127:
// регион один (прежде насчитывалось два, второй — неинициализированный).
bool sf2_global_zone_ranges_count_regions() {
  std::vector<Preset> presets = {{0, 0, {Zone{range(G_KEYRANGE, 60, 72)}, Zone{{G_INSTRUMENT, 0}}}}};
  std::vector<Inst> insts = {Inst{{Zone{range(G_KEYRANGE, 0, 59), {G_MODES, 1}, {G_SAMPLE, 0}},
                                   Zone{range(G_KEYRANGE, 60, 127), {G_MODES, 1}, {G_SAMPLE, 0}}}}};
  tsf* f = load_sf2(sf2_build(presets, insts, {kSine}));
  if (!check(f != nullptr, "банк не загрузился")) return false;
  bool ok = check(f->presets[0].regionNum == 1, "регионов " + std::to_string(f->presets[0].regionNum) + ", ждали 1");
  tsf_set_max_voices(f, 8);
  tsf_note_on(f, 0, 40, 1.0f);
  ok &= check(tsf_active_voice_count(f) == 0, "клавиша 40 зазвучала");
  tsf_note_on(f, 0, 65, 1.0f);
  ok &= check(tsf_active_voice_count(f) == 1, "клавиша 65 не зазвучала");
  tsf_close(f);
  return ok;
}

// Ссылки за массивы: сэмпл 65535, терминальный сэмпл и инструмент — зоны пропускаются;
// связи пресетов не по возрастанию, обрезанный pdta — банк не грузится.
bool sf2_bad_references() {
  bool ok = true;
  std::vector<Preset> presets = {{0, 0, {Zone{{G_INSTRUMENT, 0}}, Zone{{G_INSTRUMENT, 1}}}}};   // 1 — терминальная запись
  std::vector<Inst> insts = {Inst{{Zone{{G_SAMPLE, 65535}}, Zone{{G_SAMPLE, 1}}, Zone{{G_MODES, 1}, {G_SAMPLE, 0}}}}};
  Bytes bank = sf2_build(presets, insts, {kSine});
  tsf* f = load_sf2(bank);
  ok &= check(f != nullptr, "банк не загрузился");
  if (f) {
    ok &= check(f->presets[0].regionNum == 1, "регионов " + std::to_string(f->presets[0].regionNum) + ", ждали 1");
    tsf_close(f);
  }
  // Связь пресета не по возрастанию: presetBagNdx второго пресета меньше первого.
  std::vector<Preset> two = {{0, 0, {Zone{{G_INSTRUMENT, 0}}, Zone{{G_INSTRUMENT, 0}}}}, {1, 0, {Zone{{G_INSTRUMENT, 0}}}}};
  Bytes broken = sf2_build(two, one_instrument(looped_zone()), {kSine});
  size_t phdr = 0;
  for (size_t i = 0; i + 4 < broken.size(); ++i)
    if (std::memcmp(&broken[i], "phdr", 4) == 0) { phdr = i + 8; break; }
  // presetBagNdx — в записи phdr со смещения 24: первый 0 -> 2, второй 2 -> 0.
  broken[phdr + 24] = 2;
  broken[phdr + 38 + 24] = 0;
  tsf* bad = load_sf2(broken);
  ok &= check(phdr != 0 && bad == nullptr, "банк с убывающими связями загрузился");
  tsf_close(bad);
  // Обрезанный pdta: файл кончается посреди shdr.
  Bytes cut = sf2_build(presets, insts, {kSine});
  cut.resize(cut.size() - 30);
  tsf* g = load_sf2(cut);
  ok &= check(g == nullptr, "обрезанный банк загрузился");
  tsf_close(g);
  return ok;
}

// initialAttenuation 600 сантибел — 24 дБ (шкала EMU10K1, 0,04 дБ на единицу).
bool sf2_attenuation_scale() {
  tsf* f = load_sf2(sf2_build({{0, 0, {Zone{{G_INSTRUMENT, 0}}}}}, one_instrument(looped_zone({{G_ATTEN, 600}})), {kSine}));
  if (!check(f != nullptr, "банк не загрузился")) return false;
  const float db = f->presets[0].regions[0].attenuation;
  tsf_close(f);
  return check(std::fabs(db - 24.0f) < 0.01f, "затухание " + std::to_string(db) + " дБ");
}

// ScaleTuning 0 и CoarseTune +12: любая клавиша — корневая плюс октава; корневая 255 — 60.
bool sf2_pitch_tuning() {
  bool ok = true;
  Zone zone = looped_zone({{G_SCALE, 0}, {G_COARSE, 12}, {G_ROOT, 60}});
  tsf* f = load_sf2(sf2_build({{0, 0, {Zone{{G_INSTRUMENT, 0}}}}}, one_instrument(zone), {kSine}));
  if (!check(f != nullptr, "банк не загрузился")) return false;
  tsf_set_max_voices(f, 4);
  tsf_note_on(f, 0, 40, 1.0f);
  tsf_note_on(f, 0, 80, 1.0f);
  for (int i = 0; i < 2; ++i)
    ok &= check(std::fabs(f->voices[i].pitchInputTimecents - 7200.0f) < 0.01f,
                "высота " + std::to_string(f->voices[i].pitchInputTimecents) + ", ждали 7200");
  tsf_close(f);
  Sample unpitched = kSine;
  unpitched.pitch = 255;
  f = load_sf2(sf2_build({{0, 0, {Zone{{G_INSTRUMENT, 0}}}}}, one_instrument(looped_zone()), {unpitched}));
  if (!check(f != nullptr, "банк без высоты не загрузился")) return false;
  ok &= check(f->presets[0].regions[0].pitch_keycenter == 60, "корневая " + std::to_string(f->presets[0].regions[0].pitch_keycenter));
  tsf_close(f);
  return ok;
}

// Банк для каналов: пресет 0 — два слоя с классом 1 (хай-хэт), пресет 1 — простой.
tsf* channel_bank(Zone extra = {}) {
  std::vector<Inst> insts = {Inst{{Zone{{G_EXCLUSIVE, 1}, {G_MODES, 1}, {G_SAMPLE, 0}}, Zone{{G_EXCLUSIVE, 1}, {G_MODES, 1}, {G_SAMPLE, 0}}}},
                             Inst{{looped_zone(extra)}}};
  tsf* f = load_sf2(sf2_build({{0, 0, {Zone{{G_INSTRUMENT, 0}}}}, {1, 0, {Zone{{G_INSTRUMENT, 1}}}}}, insts, {kSine}));
  if (f) {
    tsf_set_max_voices(f, 16);
    for (int c = 0; c < 16; ++c) tsf_channel_set_presetindex(f, c, 0);
  }
  return f;
}

int sounding(tsf* f, int channel, int key) {
  int n = 0;
  for (int i = 0; i < f->voiceNum; ++i) {
    const tsf_voice& v = f->voices[i];
    if (v.playingPreset != -1 && v.playingChannel == channel && (key < 0 || v.playingKey == key) && v.ampenv.segment < TSF_SEGMENT_RELEASE) ++n;
  }
  return n;
}

// Класс: слои одной ноты живут вместе; следующая нота канала гасит прежние; чужой канал — нет.
bool cc_exclusive_class() {
  tsf* f = channel_bank();
  if (!check(f != nullptr, "банк не загрузился")) return false;
  tsf_channel_note_on(f, 9, 42, 1.0f);
  bool ok = check(sounding(f, 9, 42) == 2, "слоёв ноты " + std::to_string(sounding(f, 9, 42)) + ", ждали 2");
  tsf_channel_note_on(f, 10, 42, 1.0f);
  ok &= check(sounding(f, 9, 42) == 2, "нота другого канала погасила канал 9");
  tsf_channel_note_on(f, 9, 44, 1.0f);
  ok &= check(sounding(f, 9, 42) == 0 && sounding(f, 9, 44) == 2, "новая нота класса не погасила прежнюю");
  tsf_close(f);
  return ok;
}

// CC121 (RP-015): педаль снимается (удержанная нота гаснет), колесо — в центр; громкость,
// панорама, банк остаются.
bool cc_reset_all_controllers() {
  tsf* f = channel_bank();
  if (!check(f != nullptr, "банк не загрузился")) return false;
  tsf_channel_set_presetindex(f, 0, 1);
  tsf_channel_midi_control(f, 0, 7, 20);
  tsf_channel_midi_control(f, 0, 10, 0);
  tsf_channel_midi_control(f, 0, 0, 3);
  tsf_channel_midi_control(f, 0, 64, 127);
  tsf_channel_note_on(f, 0, 60, 1.0f);
  tsf_channel_note_off(f, 0, 60);
  tsf_channel_set_pitchwheel(f, 0, 0);
  const tsf_channel& c = f->channels->channels[0];
  const float gain = c.gainDB, pan = c.panOffset;
  bool ok = check(sounding(f, 0, 60) == 1, "педаль не держит ноту");
  tsf_channel_midi_control(f, 0, 121, 0);
  ok &= check(sounding(f, 0, 60) == 0, "удержанная педалью нота не погасла");
  ok &= check(c.gainDB == gain && c.panOffset == pan && c.bank == 3, "громкость, панорама или банк сброшены");
  ok &= check(c.pitchWheel == 8192 && !c.sustain, "колесо или педаль не сброшены");
  tsf_close(f);
  return ok;
}

// CC123 при нажатой педали: ноты держатся, пока педаль не отпустят.
bool cc_all_notes_off_keeps_sustain() {
  tsf* f = channel_bank();
  if (!check(f != nullptr, "банк не загрузился")) return false;
  tsf_channel_set_presetindex(f, 0, 1);
  tsf_channel_midi_control(f, 0, 64, 127);
  tsf_channel_note_on(f, 0, 60, 1.0f);
  tsf_channel_midi_control(f, 0, 123, 0);
  bool ok = check(sounding(f, 0, 60) == 1, "педаль не удержала ноту после CC123");
  tsf_channel_midi_control(f, 0, 64, 0);
  ok &= check(sounding(f, 0, 60) == 0, "нота не погасла с педалью");
  tsf_channel_note_on(f, 0, 62, 1.0f);
  tsf_channel_midi_control(f, 0, 123, 0);
  ok &= check(sounding(f, 0, 62) == 0, "CC123 без педали не отпустил ноту");
  tsf_close(f);
  return ok;
}

// Выбор банка: MSB, без него — LSB; второй LSB не теряет MSB.
bool cc_bank_select() {
  tsf* f = channel_bank();
  if (!check(f != nullptr, "банк не загрузился")) return false;
  const tsf_channel& c = f->channels->channels[0];
  tsf_channel_midi_control(f, 0, 0, 1);
  tsf_channel_midi_control(f, 0, 32, 2);
  bool ok = check(c.bank == 1, "CC0=1 CC32=2: банк " + std::to_string(c.bank));
  tsf_channel_midi_control(f, 0, 32, 3);
  ok &= check(c.bank == 1, "второй CC32: банк " + std::to_string(c.bank));
  tsf_channel_midi_control(f, 0, 0, 0);
  tsf_channel_midi_control(f, 0, 32, 5);
  ok &= check(c.bank == 5, "CC0=0 CC32=5: банк " + std::to_string(c.bank));
  tsf_close(f);
  return ok;
}

// RPN: грубая +1, точная −0,5 — +0,5; повтор точной — то же.
bool rpn_tuning() {
  tsf* f = channel_bank();
  if (!check(f != nullptr, "банк не загрузился")) return false;
  const tsf_channel& c = f->channels->channels[0];
  tsf_channel_midi_control(f, 0, 101, 0); tsf_channel_midi_control(f, 0, 100, 2); tsf_channel_midi_control(f, 0, 6, 65);
  tsf_channel_midi_control(f, 0, 101, 0); tsf_channel_midi_control(f, 0, 100, 1);
  tsf_channel_midi_control(f, 0, 6, 32); tsf_channel_midi_control(f, 0, 38, 0);
  bool ok = check(std::fabs(c.tuning - 0.5f) < 0.001f, "настройка " + std::to_string(c.tuning) + ", ждали 0.5");
  tsf_channel_midi_control(f, 0, 6, 32);
  ok &= check(std::fabs(c.tuning - 0.5f) < 0.001f, "после повтора — " + std::to_string(c.tuning));
  tsf_close(f);
  return ok;
}

// CC74: 0 приглушает, возврат к 64 возвращает фильтр банка (срез 13500 центов, ~19,9 кГц).
bool cc74_returns_to_neutral() {
  tsf* f = channel_bank();
  if (!check(f != nullptr, "банк не загрузился")) return false;
  tsf_set_output(f, TSF_STEREO_INTERLEAVED, 44100, 0);
  tsf_channel_set_presetindex(f, 0, 1);
  float out[2 * 64], sends[3 * 64];
  tsf_channel_note_on(f, 0, 60, 1.0f);
  tsf_voice* v = nullptr;
  for (int i = 0; i < f->voiceNum; ++i) if (f->voices[i].playingPreset != -1) v = &f->voices[i];
  if (!check(v != nullptr, "нет голоса")) { tsf_close(f); return false; }
  tsf_render_float_sends(f, out, sends, 64);
  const float a0 = v->lowpass.a0, b1 = v->lowpass.b1, b2 = v->lowpass.b2;
  tsf_channel_midi_control(f, 0, 74, 0);
  tsf_render_float_sends(f, out, sends, 64);
  bool ok = check(std::fabs(v->lowpass.a0 - a0) > 1e-3f, "CC74=0 не изменил фильтр");
  tsf_channel_midi_control(f, 0, 74, 64);
  tsf_render_float_sends(f, out, sends, 64);
  ok &= check(std::fabs(v->lowpass.a0 - a0) < 1e-6f && std::fabs(v->lowpass.b1 - b1) < 1e-6f && std::fabs(v->lowpass.b2 - b2) < 1e-6f,
              "после CC74=64 фильтр не вернулся к банку");
  tsf_close(f);
  return ok;
}

// Перемотка: все голоса — сразу в тишину.
bool voices_kill() {
  tsf* f = channel_bank();
  if (!check(f != nullptr, "банк не загрузился")) return false;
  tsf_channel_set_presetindex(f, 0, 1);
  for (int k = 50; k < 60; ++k) tsf_channel_note_on(f, 0, k, 1.0f);
  bool ok = check(tsf_active_voice_count(f) == 10, "голосов не 10");
  tsf_voices_kill(f);
  ok &= check(tsf_active_voice_count(f) == 0, "голоса остались");
  tsf_close(f);
  return ok;
}

// Образ вплотную к странице без доступа: чтение за концом роняет программу.
struct Guarded {
  uint8_t* base = nullptr;
  size_t total = 0;
  uint8_t* data = nullptr;
  explicit Guarded(size_t size) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const size_t page = si.dwPageSize;
    const size_t body = (size + page - 1) / page * page;
    total = body + page;
    base = static_cast<uint8_t*>(VirtualAlloc(nullptr, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    DWORD old;
    VirtualProtect(base + body, page, PAGE_NOACCESS, &old);
    data = base + body - size;
    data = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(data) & ~uintptr_t(1));   // чётный: отсчёты на месте
  }
  ~Guarded() { VirtualFree(base, 0, MEM_RELEASE); }
};

// Тысячи случайных порч банка: загрузка, ноты всех пресетов и звук — без чтения за образом.
bool sf2_damaged_banks_stay_in_bounds() {
  std::vector<Preset> presets = {{0, 0, {Zone{range(G_KEYRANGE, 0, 90)}, Zone{{G_INSTRUMENT, 0}}}},
                                 {5, 128, {Zone{{G_INSTRUMENT, 1}}, Zone{range(G_VELRANGE, 10, 100), {G_INSTRUMENT, 0}}}}};
  std::vector<Inst> insts = {Inst{{Zone{{G_ATTEN, 50}}, Zone{range(G_KEYRANGE, 0, 59), {G_MODES, 1}, {G_SAMPLE, 0}},
                                   Zone{range(G_KEYRANGE, 60, 127), {G_MODES, 3}, {G_SAMPLE, 1}}}},
                             Inst{{Zone{{G_EXCLUSIVE, 2}, {G_FC, 3000}, {G_SAMPLE, 1}}}}};
  const Bytes clean = sf2_build(presets, insts, {kSine, {10, 1500, 40, 1400, 22050, 64}});
  tsf* sane = load_sf2(clean);
  if (!check(sane != nullptr && sane->presetNum == 2, "целый банк не загрузился")) return false;
  tsf_close(sane);
  int loaded = 0;
  uint32_t seed = 12345;
  auto next = [&seed]() { seed = seed * 1103515245u + 12345u; return seed >> 8; };
  float out[2 * 128], sends[3 * 128];
  for (int round = 0; round < 3000; ++round) {
    Bytes bank = clean;
    const int damage = 1 + int(next() % 6);
    for (int k = 0; k < damage; ++k) {
      const size_t at = next() % bank.size();
      bank[at] = (next() & 1) ? uint8_t(next()) : uint8_t(bank[at] ^ (1u << (next() % 8)));
    }
    if (round % 7 == 0) bank.resize(bank.size() - 1 - next() % 64);
    Guarded image(bank.size());
    std::memcpy(image.data, bank.data(), bank.size());
    tsf* f = tsf_load_memory(image.data, int(bank.size()));
    if (!f) continue;
    ++loaded;
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, 44100, 0);
    tsf_set_max_voices(f, 32);
    for (int p = 0; p < tsf_get_presetcount(f); ++p) {
      tsf_channel_set_presetindex(f, p & 15, p);
      for (int key = 0; key < 128; key += 9) tsf_channel_note_on(f, p & 15, key, (key % 3 + 1) / 3.0f);
    }
    for (int i = 0; i < 40; ++i) tsf_render_float_sends(f, out, sends, 128);
    tsf_close(f);
  }
  // Порча чаще всего задевает отсчёты: такие банки грузятся и играют — проверка не впустую.
  return check(loaded > 1000, "загрузилось только " + std::to_string(loaded) + " испорченных банков");
}

// Конец петли за данными (endLoop больше длины отсчётов) зажимается до последнего отсчёта.
// Прежде он мог стать равным длине, и интерполяция на стыке петли (сосед слева начала
// петли — её конец) читала отсчёт за массивом: здесь отсчёты — в конце файла, вплотную к
// странице без доступа; шаг 2 приводит позицию ровно на начало петли.
bool sf2_loop_end_inside_samples() {
  const Sample wide = {0, 2046, 100, 2047, 44100, 60};
  const Bytes bank = sf2_build({{0, 0, {Zone{{G_INSTRUMENT, 0}}}}}, one_instrument(looped_zone()), {wide}, true);
  Guarded image(bank.size());
  std::memcpy(image.data, bank.data(), bank.size());
  tsf* f = tsf_load_memory(image.data, int(bank.size()));
  if (!check(f != nullptr, "банк не загрузился")) return false;
  bool ok = check(f->presets[0].regions[0].loop_end + 1 == f->fontSampleCount,
                  "конец петли " + std::to_string(f->presets[0].regions[0].loop_end) + " при " + std::to_string(f->fontSampleCount) + " отсчётах");
  tsf_set_output(f, TSF_STEREO_INTERLEAVED, 44100, 0);
  tsf_set_max_voices(f, 4);
  tsf_note_on(f, 0, 72, 1.0f);
  float out[2 * 256], sends[3 * 256];
  for (int i = 0; i < 400; ++i) tsf_render_float_sends(f, out, sends, 256);
  tsf_close(f);
  return ok;
}

// --- MIDI.ELF 1.7: модуляторы SF2, огибающая, голоса, педали, NRPN, SysEx ----------------

enum { G_Q = 9, G_CHORUS = 15, G_REVERB = 16, G_ATTACK = 34, G_HOLD = 35, G_KEYNUM = 46 };

tsf_voice* voice_of(tsf* f, int key, int channel = 0) {
  for (int i = 0; i < f->voiceNum; ++i) {
    tsf_voice& v = f->voices[i];
    if (v.playingPreset != -1 && v.playingKey == key && v.playingChannel == channel && !tsf_voice_quick_ending(&v)) return &v;
  }
  return nullptr;
}

// Банк с каналами: пресет 0 — инструмент 0 из одной зоны; preset — генераторы и модуляторы
// зоны пресета.
tsf* simple_bank(Zone zone, Zone preset = {}, Sample sample = kSine) {
  preset.push_back({G_INSTRUMENT, 0});
  tsf* f = load_sf2(sf2_build({{0, 0, {preset}}}, one_instrument(zone), {sample}));
  if (f) {
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, 44100, 0);
    tsf_set_max_voices(f, 16);
    for (int c = 0; c < 16; ++c) tsf_channel_set_presetindex(f, c, 0);
  }
  return f;
}

bool close_to(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

// Скорость → затухание по вогнутой кривой SF2 (модулятор по умолчанию): 64 — на 11,9 дБ тише
// 127 (громкость ~ скорость²); прежде — 6 дБ (громкость ~ скорость).
bool mod_velocity_curve() {
  tsf* f = simple_bank(looped_zone());
  if (!check(f != nullptr, "банк не загрузился")) return false;
  tsf_channel_note_on(f, 0, 60, 1.0f);
  tsf_channel_note_on(f, 0, 62, 64 / 127.0f);
  const float diff = voice_of(f, 62)->noteGainDB - voice_of(f, 60)->noteGainDB;
  tsf_close(f);
  return check(close_to(diff, -11.91f, 0.05f), "64 против 127: " + std::to_string(diff) + " дБ");
}

// Громкость канала по умолчанию — 100 (GM): на 4,15 дБ тише CC7=127 (вогнутая кривая).
bool mod_default_volume() {
  tsf* f = simple_bank(looped_zone());
  if (!check(f != nullptr, "банк не загрузился")) return false;
  tsf_channel_midi_control(f, 1, 7, 127);
  tsf_channel_note_on(f, 0, 60, 1.0f);
  tsf_channel_note_on(f, 1, 60, 1.0f);
  const float diff = voice_of(f, 60, 0)->noteGainDB - voice_of(f, 60, 1)->noteGainDB;
  tsf_close(f);
  return check(close_to(diff, -4.15f, 0.05f), "CC7 100 против 127: " + std::to_string(diff) + " дБ");
}

// Модуляторы зоны инструмента заменяют умолчания (как TimGM6mb: CC1 → вибрато 0, CC10 →
// панорама 1000): CC1=127 — без вибрато, CC10=32 — уже до упора влево.
bool mod_bank_overrides() {
  bool ok = true;
  tsf* f = simple_bank(looped_zone({mod(0x0081, 6, 0), mod(0x028A, 17, 1000)}));
  tsf* g = simple_bank(looped_zone());
  if (!check(f && g, "банк не загрузился")) return false;
  for (tsf* b : {f, g}) {
    tsf_channel_midi_control(b, 0, 1, 127);
    tsf_channel_midi_control(b, 0, 10, 32);
    tsf_channel_note_on(b, 0, 60, 1.0f);
  }
  tsf_voice *vf = voice_of(f, 60), *vg = voice_of(g, 60);
  ok &= check(vf->vibLfoToPitch == 0.0f, "вибрато не отключено модулятором банка");
  ok &= check(close_to(vg->vibLfoToPitch, 49.61f, 0.05f), "вибрато по умолчанию " + std::to_string(vg->vibLfoToPitch));
  ok &= check(vf->panFactorLeft == 1.0f && vf->panFactorRight == 0.0f, "панорама 1000: CC10=32 — не до упора влево");
  ok &= check(close_to(vg->panFactorLeft, std::sqrt(0.75f), 1e-4f), "панорама по умолчанию: CC10=32 — не четверть влево");
  tsf_close(f);
  tsf_close(g);
  return ok;
}

// Модулятор пресета складывается с модулятором инструмента: CC1 → вибрато 50 + 50.
bool mod_preset_adds() {
  tsf* f = simple_bank(looped_zone(), {mod(0x0081, 6, 50)});
  if (!check(f != nullptr, "банк не загрузился")) return false;
  tsf_channel_midi_control(f, 0, 1, 127);
  tsf_channel_note_on(f, 0, 60, 1.0f);
  const float vib = voice_of(f, 60)->vibLfoToPitch;
  tsf_close(f);
  return check(close_to(vib, 99.22f, 0.05f), "вибрато " + std::to_string(vib) + ", ждали 99,2");
}

// Посылы в генераторах зоны (15, 16) — к модуляторам CC91 (по умолчанию 40) и CC93: у них
// 1000 (посыл CC/128, как CC/127 в 1.6), генераторы — по шкале SF2 (1000 — 1,0); больше — 1,0.
bool sf2_send_generators() {
  tsf* f = simple_bank(looped_zone({{G_REVERB, 20}, {G_CHORUS, 50}}));
  if (!check(f != nullptr, "банк не загрузился")) return false;
  tsf_channel_note_on(f, 0, 60, 1.0f);
  tsf_voice* v = voice_of(f, 60);
  bool ok = check(close_to(v->reverbSend, (20 + 312.5f) / 1000, 1e-4f), "реверберация " + std::to_string(v->reverbSend)) &&
            check(close_to(v->chorusSend, 50.0f / 1000, 1e-4f), "хорус " + std::to_string(v->chorusSend));
  tsf_channel_midi_control(f, 0, 93, 64);       // 64/128 × 1000 = 500 к своим 50
  tsf_channel_midi_control(f, 0, 91, 127);      // 20 + 992 > 1000 — потолок
  ok = check(close_to(v->chorusSend, 550.0f / 1000, 1e-4f), "хорус на CC93 64: " + std::to_string(v->chorusSend)) &&
       check(close_to(v->reverbSend, 1.0f, 1e-6f), "реверберация на CC91 127: " + std::to_string(v->reverbSend)) && ok;
  tsf_close(f);
  return ok;
}

// Резонанс 20 дБ: усиление фильтра на постоянном токе — минус половина (−10 дБ, 0,316).
bool filter_resonance_gain() {
  tsf* f = simple_bank(looped_zone({{G_Q, 200}}));
  if (!check(f != nullptr, "банк не загрузился")) return false;
  tsf_channel_note_on(f, 0, 60, 1.0f);
  const tsf_voice_lowpass lp = voice_of(f, 60)->lowpass;   // копия: голос живёт до tsf_close
  const float dc = (2.0f * lp.a0 + lp.a1) / (1.0f + lp.b1 + lp.b2);
  tsf_close(f);
  return check(lp.active && close_to(dc, 0.3162f, 0.005f), "усиление на постоянном токе " + std::to_string(dc));
}

// Атака 88 отсчётов, удержание 441: после двух кусков по 64 — удержание, 40 отсчётов уже в
// нём (прежде остаток куска терялся: 441).
bool envelope_stage_carry() {
  tsf* f = simple_bank(looped_zone({{G_ATTACK, uint16_t(-10759)}, {G_HOLD, uint16_t(-7972)}}));
  if (!check(f != nullptr, "банк не загрузился")) return false;
  float out[2 * 64], sends[3 * 64];
  tsf_channel_note_on(f, 0, 60, 1.0f);
  tsf_voice* v = voice_of(f, 60);
  tsf_render_float_sends(f, out, sends, 64);
  tsf_render_float_sends(f, out, sends, 64);
  const bool ok = check(v->ampenv.segment == TSF_SEGMENT_HOLD, "не удержание") &&
                  check(v->ampenv.samplesUntilNextSegment >= 400 && v->ampenv.samplesUntilNextSegment <= 402,
                        "до конца удержания " + std::to_string(v->ampenv.samplesUntilNextSegment));
  tsf_close(f);
  return ok;
}

// Громкость внутри куска растёт от начала атаки линейно: первый отсчёт — ноль, последние —
// уже звучат (прежде весь первый кусок атаки молчал: огибающая стояла ступенью).
bool envelope_gain_ramp() {
  // Начало — с пика синуса (смещение 25 = четверть периода): первый отсчёт сам не ноль.
  tsf* f = simple_bank(looped_zone({{G_ATTACK, uint16_t(-7972)}, {0, 25}}));
  if (!check(f != nullptr, "банк не загрузился")) return false;
  float out[2 * 64], sends[3 * 64];
  std::memset(sends, 0, sizeof(sends));
  tsf_channel_note_on(f, 0, 60, 1.0f);
  tsf_render_float_sends(f, out, sends, 64);
  float tail = 0.0f;
  for (int i = 48; i < 64; ++i) tail = std::max(tail, std::fabs(out[2 * i]));
  tsf_close(f);
  return check(out[0] == 0.0f && out[1] == 0.0f, "первый отсчёт атаки не ноль") && check(tail > 1e-4f, "конец первого куска атаки молчит");
}

// Предел 4 голоса: пятая нота отдаёт голос самой тихой (скорость 0,5) — та гаснет за 10 мс в
// запасном месте, а не обрывается.
bool steal_quietest_fades() {
  tsf* f = simple_bank(looped_zone());
  if (!check(f != nullptr, "банк не загрузился")) return false;
  tsf_set_max_voices(f, 6);
  tsf_set_voice_cap(f, 4);
  const float vels[4] = {1.0f, 0.9f, 0.5f, 0.8f};
  for (int k = 0; k < 4; ++k) tsf_channel_note_on(f, 0, 60 + k, vels[k]);
  tsf_voice* quiet = voice_of(f, 62);
  tsf_channel_note_on(f, 0, 64, 1.0f);
  const bool ok = check(quiet->playingPreset != -1 && quiet->playingKey == 62 && tsf_voice_quick_ending(quiet), "тихий голос не гаснет плавно") &&
                  check(voice_of(f, 64) != nullptr, "новая нота без голоса") &&
                  check(tsf_voices_sounding(f) == 4 && tsf_active_voice_count(f) == 5, "звучащих не 4 или гаснущего нет");
  tsf_close(f);
  return ok;
}

// Отданная нота гаснет целиком: у двухслойной ноты — оба голоса, хотя новой (однослойной)
// нужен один.
bool steal_whole_note() {
  std::vector<Inst> insts = {Inst{{looped_zone(), looped_zone()}}, Inst{{looped_zone()}}};
  tsf* f = load_sf2(sf2_build({{0, 0, {Zone{{G_INSTRUMENT, 0}}}}, {1, 0, {Zone{{G_INSTRUMENT, 1}}}}}, insts, {kSine}));
  if (!check(f != nullptr, "банк не загрузился")) return false;
  tsf_set_output(f, TSF_STEREO_INTERLEAVED, 44100, 0);
  tsf_set_max_voices(f, 8);
  tsf_set_voice_cap(f, 4);
  tsf_channel_set_presetindex(f, 0, 0);
  tsf_channel_set_presetindex(f, 1, 1);
  tsf_channel_note_on(f, 0, 60, 0.5f);
  tsf_channel_note_on(f, 0, 62, 1.0f);
  tsf_channel_note_on(f, 1, 64, 1.0f);
  int fading = 0;
  for (int i = 0; i < f->voiceNum; ++i)
    if (f->voices[i].playingPreset != -1 && f->voices[i].playingKey == 60 && tsf_voice_quick_ending(&f->voices[i])) ++fading;
  tsf_close(f);
  return check(fading == 2, "гаснет голосов ноты 60: " + std::to_string(fading));
}

// Sostenuto держит ноту, нажатую в миг нажатия педали, а взятую после — нет.
bool pedal_sostenuto() {
  tsf* f = simple_bank(looped_zone());
  if (!check(f != nullptr, "банк не загрузился")) return false;
  tsf_channel_note_on(f, 0, 60, 1.0f);
  tsf_channel_midi_control(f, 0, 66, 127);
  tsf_channel_note_on(f, 0, 62, 1.0f);
  tsf_channel_note_off(f, 0, 60);
  tsf_channel_note_off(f, 0, 62);
  bool ok = check(voice_of(f, 60) && voice_of(f, 60)->ampenv.segment < TSF_SEGMENT_RELEASE, "педаль не держит ноту 60") &&
            check(!voice_of(f, 62) || voice_of(f, 62)->ampenv.segment >= TSF_SEGMENT_RELEASE, "нота 62 держится, хотя взята после педали");
  tsf_channel_midi_control(f, 0, 66, 0);
  ok &= check(!voice_of(f, 60) || voice_of(f, 60)->ampenv.segment >= TSF_SEGMENT_RELEASE, "педаль отпущена, нота 60 звучит");
  tsf_close(f);
  return ok;
}

// Левая педаль (CC67, модулятор как у SpessaSynth) — срез на 1200 центов ниже.
bool pedal_soft_filter() {
  tsf* f = simple_bank(looped_zone());
  if (!check(f != nullptr, "банк не загрузился")) return false;
  tsf_channel_midi_control(f, 0, 67, 127);
  tsf_channel_note_on(f, 0, 60, 1.0f);
  const float fc = voice_of(f, 60)->initialFilterFc;
  tsf_close(f);
  return check(fc == 12300.0f, "срез " + std::to_string(fc));
}

// NRPN GS/XG 01 20 (срез) — как CC74: значение 0 — срез 13500 − 9600 = 3900 центов.
bool nrpn_cutoff() {
  tsf* f = simple_bank(looped_zone());
  if (!check(f != nullptr, "банк не загрузился")) return false;
  tsf_channel_note_on(f, 0, 60, 1.0f);
  tsf_channel_midi_control(f, 0, 99, 1);
  tsf_channel_midi_control(f, 0, 98, 0x20);
  tsf_channel_midi_control(f, 0, 6, 0);
  const float fc = voice_of(f, 60)->initialFilterFc;
  tsf_close(f);
  return check(fc == 3900.0f, "срез " + std::to_string(fc));
}

// RPN 5 = 1 полутон: глубина вибрато колесом модуляции — 100 центов вместо 50.
bool rpn5_modulation_depth() {
  tsf* f = simple_bank(looped_zone());
  if (!check(f != nullptr, "банк не загрузился")) return false;
  tsf_channel_midi_control(f, 0, 101, 0); tsf_channel_midi_control(f, 0, 100, 5);
  tsf_channel_midi_control(f, 0, 6, 1); tsf_channel_midi_control(f, 0, 38, 0);
  tsf_channel_midi_control(f, 0, 1, 127);
  tsf_channel_note_on(f, 0, 60, 1.0f);
  const float vib = voice_of(f, 60)->vibLfoToPitch;
  tsf_close(f);
  return check(close_to(vib, 99.22f, 0.05f), "вибрато " + std::to_string(vib));
}

// Data Increment/Decrement у диапазона колеса — на цент.
bool data_increment() {
  tsf* f = simple_bank(looped_zone());
  if (!check(f != nullptr, "банк не загрузился")) return false;
  const tsf_channel& c = f->channels->channels[0];
  tsf_channel_midi_control(f, 0, 101, 0); tsf_channel_midi_control(f, 0, 100, 0);
  tsf_channel_midi_control(f, 0, 6, 2); tsf_channel_midi_control(f, 0, 38, 0);
  tsf_channel_midi_control(f, 0, 96, 0);
  bool ok = check(close_to(c.pitchRange, 2.01f, 1e-4f), "после +1: " + std::to_string(c.pitchRange));
  tsf_channel_midi_control(f, 0, 97, 0); tsf_channel_midi_control(f, 0, 97, 0);
  ok &= check(close_to(c.pitchRange, 1.99f, 1e-4f), "после −2: " + std::to_string(c.pitchRange));
  tsf_close(f);
  return ok;
}

// Одноголосный режим (CC126): новая нота гасит прежнюю.
bool mono_mode() {
  tsf* f = simple_bank(looped_zone());
  if (!check(f != nullptr, "банк не загрузился")) return false;
  tsf_channel_midi_control(f, 0, 126, 1);
  tsf_channel_note_on(f, 0, 60, 1.0f);
  tsf_channel_note_on(f, 0, 62, 1.0f);
  const bool ok = check(!voice_of(f, 60) && voice_of(f, 62), "прежняя нота не погасла");
  tsf_close(f);
  return ok;
}

// Постоянная клавиша зоны (генератор 46): любая нота звучит как клавиша 60.
bool sf2_fixed_key() {
  tsf* f = simple_bank(looped_zone({{G_KEYNUM, 60}}));
  if (!check(f != nullptr, "банк не загрузился")) return false;
  tsf_channel_note_on(f, 0, 72, 1.0f);
  const float pitch = voice_of(f, 72)->pitchInputTimecents;
  tsf_close(f);
  return check(close_to(pitch, 6000.0f, 0.01f), "высота " + std::to_string(pitch));
}

// Сэмпл ПЗУ (тип 0x8001) не играется; банк из одних таких — не банк.
bool sf2_rom_samples() {
  Sample rom = kSine;
  rom.type = 0x8001;
  std::vector<Inst> insts = {Inst{{Zone{range(G_KEYRANGE, 0, 59), {G_MODES, 1}, {G_SAMPLE, 0}}, Zone{range(G_KEYRANGE, 60, 127), {G_MODES, 1}, {G_SAMPLE, 1}}}}};
  tsf* f = load_sf2(sf2_build({{0, 0, {Zone{{G_INSTRUMENT, 0}}}}}, insts, {rom, kSine}));
  bool ok = check(f && f->presets[0].regionNum == 1, "регион с сэмплом ПЗУ не отброшен");
  tsf_close(f);
  tsf* g = load_sf2(sf2_build({{0, 0, {Zone{{G_INSTRUMENT, 0}}}}}, one_instrument(looped_zone()), {rom}));
  ok &= check(g == nullptr, "банк без играбельных зон загрузился");
  tsf_close(g);
  return ok;
}

// Нет пресета — канал молчит; у ударных нет подбора мелодического.
bool missing_preset_is_silent() {
  tsf* f = simple_bank(looped_zone());
  if (!check(f != nullptr, "банк не загрузился")) return false;
  bool ok = check(tsf_channel_set_presetnumber(f, 0, 5, 0) == 0, "пресет 5 найден");
  tsf_channel_note_on(f, 0, 60, 1.0f);
  ok &= check(tsf_active_voice_count(f) == 0, "канал без пресета звучит");
  ok &= check(tsf_channel_set_presetnumber(f, 9, 0, 1) == 0, "набор ударных найден");
  tsf_channel_note_on(f, 9, 38, 1.0f);
  ok &= check(tsf_active_voice_count(f) == 0, "ударные звучат мелодическим инструментом");
  tsf_close(f);
  return ok;
}

// SysEx: сбросы GM, GS, XG, громкость, ударная партия GS, продолжение пакетами F7, escape.
bool tml_sysex_events() {
  Bytes t;
  event(t, 0, {0xF0, 0x05, 0x7E, 0x7F, 0x09, 0x01, 0xF7});                                // GM On
  event(t, 1, {0xF0, 0x0A, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7});  // GS Reset
  event(t, 1, {0xF0, 0x07, 0x7F, 0x7F, 0x04, 0x01, 0x00, 0x40, 0xF7});                    // громкость 8192
  event(t, 1, {0xF0, 0x0A, 0x41, 0x10, 0x42, 0x12, 0x40, 0x11, 0x15, 0x01, 0x19, 0xF7});  // партия 1 — ударные
  event(t, 1, {0xF0, 0x04, 0x43, 0x10, 0x4C, 0x00});                                      // XG On, начало
  event(t, 1, {0xF7, 0x04, 0x00, 0x7E, 0x00, 0xF7});                                      // ... продолжение
  event(t, 1, {0xF7, 0x06, 0xF0, 0x7E, 0x7F, 0x09, 0x03, 0xF7});                          // escape: GM2 On
  event(t, 1, {0xF0, 0x03, 0x7D, 0x01, 0xF7});                                            // чужой — нет
  eot(t);
  tml_message* song = load(smf(0, 480, {t}));
  std::vector<int> kinds;
  int volume = -1, part = -1, drum = -1;
  for (tml_message* m = song; m; m = m->next) {
    if (m->type != TML_SYSTEM_EXCLUSIVE) continue;
    kinds.push_back(m->channel);
    if (m->channel == TML_SYSEX_MASTER_VOLUME) volume = m->pitch_bend;
    if (m->channel == TML_SYSEX_GS_RHYTHM) part = m->key, drum = m->velocity;
  }
  tml_free(song);
  const std::vector<int> expected = {TML_SYSEX_GM_ON, TML_SYSEX_GS_RESET, TML_SYSEX_MASTER_VOLUME, TML_SYSEX_GS_RHYTHM, TML_SYSEX_XG_ON, TML_SYSEX_GM2_ON};
  return check(kinds == expected, "виды SysEx не те (" + std::to_string(kinds.size()) + ")") &&
         check(volume == 8192, "громкость " + std::to_string(volume)) && check(part == 0 && drum == 1, "партия GS не та");
}

// Отказ памяти на каждом выделении загрузки банка (пресеты, регионы, модуляторы): NULL или
// банк целиком, без утечек (память TSF у MIDI.ELF — у плеера, его отказ — обычное дело).
bool sf2_out_of_memory_is_clean() {
  const Bytes bank = sf2_build({{0, 0, {Zone{{G_INSTRUMENT, 0}}}}, {1, 0, {Zone{mod(0x0081, 6, 25), {G_INSTRUMENT, 0}}}}},
                               one_instrument(looped_zone({mod(0x0081, 6, 0)})), {kSine});
  alloc_reset();
  tsf_close(load_sf2(bank));
  const long total = g_counter;
  bool ok = check(total >= 10, "выделений меньше 10");
  for (long n = 0; n < total && ok; ++n) {
    const long live = g_live;
    alloc_reset();
    g_fail_at = n;
    tsf* f = load_sf2(bank);
    g_fail_at = -1;
    if (f) ok &= check(f->presetNum == 2 && f->presets[0].regionNum == 1 && f->presets[1].regionNum == 1, "при отказе №" + std::to_string(n) + " банк неполный");
    tsf_close(f);
    ok &= check(g_live == live, "утечка при отказе №" + std::to_string(n));
  }
  return ok;
}

// 6000 дорожек по 40 нот: слияние кучей — быстро (прежде каждый шаг обходил все дорожки).
bool tml_many_tracks_fast() {
  std::vector<Bytes> tracks(6000);
  for (int i = 0; i < 6000; ++i) {
    for (int k = 0; k < 40; ++k) event(tracks[i], k ? 6000 : i + 1, {0x90, uint8_t(i % 100), 1});
    eot(tracks[i]);
  }
  const Bytes file = smf(1, 480, tracks);
  const DWORD start = GetTickCount();
  tml_message* song = load(file);
  const DWORD spent = GetTickCount() - start;
  size_t count = 0;
  unsigned last = 0;
  bool ordered = true;
  for (tml_message* m = song; m; m = m->next) ordered &= m->time >= last, last = m->time, ++count;
  tml_free(song);
  return check(count == 240000 && ordered, "событий " + std::to_string(count) + " или не по порядку") &&
         check(spent < 1500, "разбор " + std::to_string(spent) + " мс");
}

// Песня из памяти: массив сообщений — одним блоком на все события (tml_count_events), без
// роста через realloc (на плате старый и новый блоки жили вместе, освобождённые оставались
// дырами, и песне в 23 тысячи событий не хватало PSRAM), дорожки читаются на месте (без
// копии). Смесь событий: running status, программы (один байт), SysEx с продолжением,
// мета-события, чужой блок между дорожками, End of Track посреди дорожки.
bool tml_messages_in_one_block() {
  Bytes a, b;
  event(a, 0, {0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20});
  event(a, 0, {0xFF, 0x03, 0x04, 'S', 'o', 'n', 'g'});
  event(a, 0, {0xF0, 0x04, 0x43, 0x10, 0x4C, 0x00});
  event(a, 0, {0xF7, 0x04, 0x00, 0x7E, 0x00, 0xF7});
  event(a, 0, {0xC0, 5});
  for (int i = 0; i < 6000; ++i) {
    event(a, 10, {0x90, uint8_t(40 + i % 40), 100});
    event(a, 10, {uint8_t(40 + i % 40), 0});           // running status: отпускание
    if (i % 100 == 0) event(a, 0, {0xB0, 7, uint8_t(i % 128)}), event(a, 0, {0xC0, uint8_t(i % 128)}), event(a, 0, {uint8_t(i % 7)});
  }
  eot(a);
  for (int i = 0; i < 3000; ++i) event(b, 20, {0x99, 36, 90}), event(b, 0, {36, 0});
  eot(b);
  event(b, 0, {0x90, 60, 100});                        // после End of Track — не событие песни
  Bytes file = smf(1, 480, {a, b});
  const Bytes foreign = {'X', 'F', 'I', 'H', 0, 0, 0, 3, 1, 2, 3};
  file.insert(file.begin() + 14, foreign.begin(), foreign.end());
  alloc_reset();
  tml_message* song = load(file);
  const long allocations = g_counter, reallocs = g_reallocs;
  const size_t largest = g_largest;
  size_t notes = 0, count = 0;
  for (tml_message* m = song; m; m = m->next, ++count) notes += (m->type == TML_NOTE_ON && m->velocity);
  tml_free(song);
  const size_t events = 5 + 6000 * 2 + 60 * 3 + 1 + 3000 * 2 + 1;   // с End of Track каждой дорожки
  return check(notes == 9000, "нот " + std::to_string(notes)) &&
         check(reallocs == 0, "realloc массива: " + std::to_string(reallocs)) &&
         check(largest == events * sizeof(tml_message), "самый большой блок " + std::to_string(largest) + ", ждали " + std::to_string(events * sizeof(tml_message))) &&
         check(allocations <= 3, "выделений " + std::to_string(allocations) + " (дорожки, сообщения, куча слияния)") &&
         check(count <= events, "сообщений больше событий");
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);   // при падении вывод до него не теряется
  report("tml_format1_merge_keeps_every_track", tml_format1_merge_keeps_every_track());
  report("tml_tempo_changes_do_not_drift", tml_tempo_changes_do_not_drift());
  report("tml_long_delta_is_kept", tml_long_delta_is_kept());
  report("tml_bad_headers_are_refused", tml_bad_headers_are_refused());
  report("tml_huge_track_is_not_allocated", tml_huge_track_is_not_allocated());
  report("tml_running_status_rules", tml_running_status_rules());
  report("tml_unknown_chunk_is_skipped", tml_unknown_chunk_is_skipped());
  report("tml_out_of_memory_is_clean", tml_out_of_memory_is_clean());
  report("tml_messages_in_one_block", tml_messages_in_one_block());
  report("sf2_global_zone_ranges_count_regions", sf2_global_zone_ranges_count_regions());
  report("sf2_bad_references", sf2_bad_references());
  report("sf2_attenuation_scale", sf2_attenuation_scale());
  report("sf2_pitch_tuning", sf2_pitch_tuning());
  report("cc_exclusive_class", cc_exclusive_class());
  report("cc_reset_all_controllers", cc_reset_all_controllers());
  report("cc_all_notes_off_keeps_sustain", cc_all_notes_off_keeps_sustain());
  report("cc_bank_select", cc_bank_select());
  report("rpn_tuning", rpn_tuning());
  report("cc74_returns_to_neutral", cc74_returns_to_neutral());
  report("voices_kill", voices_kill());
  report("sf2_damaged_banks_stay_in_bounds", sf2_damaged_banks_stay_in_bounds());
  report("sf2_loop_end_inside_samples", sf2_loop_end_inside_samples());
  report("mod_velocity_curve", mod_velocity_curve());
  report("mod_default_volume", mod_default_volume());
  report("mod_bank_overrides", mod_bank_overrides());
  report("mod_preset_adds", mod_preset_adds());
  report("sf2_send_generators", sf2_send_generators());
  report("filter_resonance_gain", filter_resonance_gain());
  report("envelope_stage_carry", envelope_stage_carry());
  report("envelope_gain_ramp", envelope_gain_ramp());
  report("steal_quietest_fades", steal_quietest_fades());
  report("steal_whole_note", steal_whole_note());
  report("pedal_sostenuto", pedal_sostenuto());
  report("pedal_soft_filter", pedal_soft_filter());
  report("nrpn_cutoff", nrpn_cutoff());
  report("rpn5_modulation_depth", rpn5_modulation_depth());
  report("data_increment", data_increment());
  report("mono_mode", mono_mode());
  report("sf2_fixed_key", sf2_fixed_key());
  report("sf2_rom_samples", sf2_rom_samples());
  report("missing_preset_is_silent", missing_preset_is_silent());
  report("tml_sysex_events", tml_sysex_events());
  report("tml_many_tracks_fast", tml_many_tracks_fast());
  report("sf2_out_of_memory_is_clean", sf2_out_of_memory_is_clean());
  return g_failures ? 1 : 0;
}
