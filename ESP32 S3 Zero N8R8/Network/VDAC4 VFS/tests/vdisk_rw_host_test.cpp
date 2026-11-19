// Том VDAC4 VFS с записью (режим USB Flash, прошивка 0.1.148) на ПК.
//
// Источник — файловая система в памяти («флешка»). Модель WC работает с томом только секторами
// (Volume::read / Volume::write), в том порядке, в каком пишет ядро CORE32 WC 1.11i:
// - файл (MKFILE): цепочка в FAT (обе копии, сектор за сектором), запись каталога (LFN, затем SFN)
//   с первым кластером и размером, потом данные по цепочке;
// - каталог (MKDIR): кластер в FAT, тело («.», «..», нули) в кластер, потом запись в родителе;
// - удаление (DELFL): цепочка освобождается, записи LFN и последней SFN — 0xE5;
// - переименование и перенос (RENAME): новая запись с той же цепочкой, затем удаление прежней;
// - дописывание (APPEND): звено FAT, данные, новый размер в записи;
// - рост каталога: звено FAT к новому кластеру, нули в него, затем записи.
// После каждого шага: файлы «флешки» совпадают с ожидаемыми, а том, прочитанный моделью заново,
// показывает то же дерево и те же байты.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "zifi/vdisk.hpp"

using namespace zifi::vdisk;

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)

// ---- «флешка» в памяти ----
struct Mem : Source {
  struct File { bool dir = false; std::vector<uint8_t> data; uint16_t date = 0, time = 0; };
  std::map<std::string, File> files;   // путь -> файл; каталоги тоже ("" — корень)
  std::vector<std::string> ops;
  bool fail = false;                   // источник отказывает в создании (нет места, имя)
  Mem() { files[""].dir = true; }
  const char* folder() const override { return "USB"; }
  static std::string parent(const std::string& p) {
    const size_t s = p.rfind('/');
    return s == std::string::npos ? "" : p.substr(0, s);
  }
  static std::string base(const std::string& p) {
    const size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
  }
  bool list(const char* path, Each each, void* user) override {
    auto d = files.find(path);
    if (d == files.end() || !d->second.dir) return false;
    for (auto& [p, f] : files) {
      if (p.empty() || parent(p) != path) continue;
      const std::string b = base(p);
      Entry e{b.c_str(), f.dir, f.data.size(), (uint16_t)((45 << 9) | (10 << 5) | 24), (uint16_t)((12 << 11) | (30 << 5))};
      if (!each(e, user)) break;
    }
    return true;
  }
  bool read(const char* path, uint64_t offset, uint8_t* out, size_t n, size_t* got) override {
    auto f = files.find(path);
    if (f == files.end() || f->second.dir) return false;
    *got = 0;
    if (offset >= f->second.data.size()) return true;
    *got = std::min<size_t>(n, f->second.data.size() - (size_t)offset);
    memcpy(out, f->second.data.data() + offset, *got);
    return true;
  }
  bool create(const char* path, uint64_t size) override {
    ops.push_back(std::string("create ") + path + " " + std::to_string(size));
    if (fail || !files.count(parent(path)) || !files[parent(path)].dir) return false;
    File f;
    f.data.assign((size_t)size, 0xEE);   // содержимое не задано — «мусор»
    files[path] = f;
    return true;
  }
  bool write(const char* path, uint64_t offset, const uint8_t* data, size_t n) override {
    auto f = files.find(path);
    if (f == files.end() || f->second.dir) return false;
    if (offset + n > f->second.data.size()) f->second.data.resize((size_t)(offset + n));
    memcpy(f->second.data.data() + offset, data, n);
    return true;
  }
  bool resize(const char* path, uint64_t size) override {
    ops.push_back(std::string("resize ") + path + " " + std::to_string(size));
    auto f = files.find(path);
    if (f == files.end() || f->second.dir) return false;
    f->second.data.resize((size_t)size, 0xEE);
    return true;
  }
  bool makeDir(const char* path) override {
    ops.push_back(std::string("mkdir ") + path);
    if (fail || files.count(path) || !files.count(parent(path))) return false;
    files[path].dir = true;
    return true;
  }
  bool remove(const char* path, bool dir) override {
    ops.push_back(std::string("remove ") + path);
    auto f = files.find(path);
    if (f == files.end() || f->second.dir != dir) return false;
    if (dir)
      for (auto& [p, x] : files) if (!p.empty() && parent(p) == path) return false;   // не пуст
    files.erase(f);
    return true;
  }
  bool rename(const char* from, const char* to) override {
    ops.push_back(std::string("rename ") + from + " -> " + to);
    if (!files.count(from) || files.count(to) || !files.count(parent(to))) return false;
    const std::string a = from, b = to;
    std::map<std::string, File> next;
    for (auto& [p, f] : files) {
      if (p == a) next[b] = f;
      else if (p.size() > a.size() && p.compare(0, a.size(), a) == 0 && p[a.size()] == '/') next[b + p.substr(a.size())] = f;
      else next[p] = f;
    }
    files.swap(next);
    return true;
  }
  bool setTime(const char* path, uint16_t date, uint16_t time) override {
    auto f = files.find(path);
    if (f == files.end()) return false;
    f->second.date = date;
    f->second.time = time;
    return true;
  }
};

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t* p) { return rd16(p) | (uint32_t)rd16(p + 2) << 16; }
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v) { wr16(p, (uint16_t)v); wr16(p + 2, (uint16_t)(v >> 16)); }

static std::vector<uint8_t> pattern(size_t n, unsigned seed) {
  std::vector<uint8_t> d(n);
  for (size_t i = 0; i < n; ++i) d[i] = (uint8_t)(i * 13 + seed * 101 + (i >> 9) * 7);
  return d;
}

// ---- модель WC: FAT32 только секторами тома ----
struct Wc {
  Volume& v;
  uint32_t spc = 0, reserved = 0, fats = 0, fatSize = 0, root = 0, data = 0, limit = 0, hint = 0;
  unsigned sectorWrites = 0;
  explicit Wc(Volume& vol) : v(vol) { mount(); }
  std::vector<uint8_t> sector(uint32_t lba) {
    std::vector<uint8_t> s(512);
    CHECK(v.read(lba, s.data()));
    return s;
  }
  void put(uint32_t lba, const std::vector<uint8_t>& s) { ++sectorWrites; CHECK(v.write(lba, s.data())); }
  bool tryPut(uint32_t lba, const std::vector<uint8_t>& s) { ++sectorWrites; return v.write(lba, s.data()); }
  void mount() {
    auto b = sector(0);
    spc = b[13];
    reserved = rd16(b.data() + 14);
    fats = b[16];
    fatSize = rd32(b.data() + 36);
    root = rd32(b.data() + 44);
    data = reserved + fats * fatSize;
    limit = (rd32(b.data() + 32) - data) / spc + 2;
    hint = 2 + (limit - 2) / 16 * 15;   // WC без годной подсказки FSInfo: с 15/16 области данных
  }
  uint32_t lbaOf(uint32_t c) const { return data + (c - 2) * spc; }
  uint32_t fat(uint32_t c) { return rd32(sector(reserved + c / 128).data() + (c % 128) * 4) & 0x0FFFFFFF; }
  // запись звеньев: сектор FAT целиком, обе копии (как SAVE_FAT_SECTOR с зеркалом)
  void setFat(const std::vector<std::pair<uint32_t, uint32_t>>& links) {
    std::map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> bySector;
    for (auto& l : links) bySector[l.first / 128].push_back(l);
    for (auto& [s, list] : bySector) {
      auto sec = sector(reserved + s);
      for (auto& l : list) wr32(sec.data() + (l.first % 128) * 4, l.second);
      for (uint32_t k = 0; k < fats; ++k) put(reserved + k * fatSize + s, sec);
    }
  }
  std::vector<uint32_t> allocate(uint32_t count) {
    std::vector<uint32_t> got;
    for (uint32_t c = hint; got.size() < count; c = c + 1 < limit ? c + 1 : 2) {
      if (fat(c) == 0) got.push_back(c);
    }
    hint = got.back() + 1;
    std::vector<std::pair<uint32_t, uint32_t>> links;
    for (size_t i = 0; i < got.size(); ++i) links.push_back({got[i], i + 1 < got.size() ? got[i + 1] : 0x0FFFFFFF});
    setFat(links);
    return got;
  }
  std::vector<uint32_t> chain(uint32_t c) {
    std::vector<uint32_t> out;
    while (c >= 2 && c < 0x0FFFFFF8 && out.size() < 200000) { out.push_back(c); c = fat(c); }
    return out;
  }
  // записи каталога: (номер записи, 32 байта)
  struct Rec { uint32_t index; uint8_t r[32]; };
  std::vector<Rec> records(uint32_t dirCluster) {
    std::vector<Rec> out;
    uint32_t index = 0;
    for (uint32_t c : chain(dirCluster)) {
      for (uint32_t s = 0; s < spc; ++s) {
        auto sec = sector(lbaOf(c) + s);
        for (int k = 0; k < 16; ++k, ++index) { Rec x; x.index = index; memcpy(x.r, sec.data() + k * 32, 32); out.push_back(x); }
      }
    }
    return out;
  }
  void writeRecord(uint32_t dirCluster, uint32_t index, const uint8_t* r) {
    auto ch = chain(dirCluster);
    const uint32_t perCluster = spc * 16;
    while (index / perCluster >= ch.size()) {   // каталог полон: звено, нули, дальше
      auto add = allocate(1);
      setFat({{ch.back(), add[0]}});
      std::vector<uint8_t> zero(512, 0);
      for (uint32_t s = 0; s < spc; ++s) put(lbaOf(add[0]) + s, zero);
      ch.push_back(add[0]);
    }
    const uint32_t lba = lbaOf(ch[index / perCluster]) + (index % perCluster) / 16;
    auto sec = sector(lba);
    memcpy(sec.data() + (index % 16) * 32, r, 32);
    put(lba, sec);
  }
  struct Item { std::string name; bool dir; uint32_t cluster, size; uint32_t sfnIndex, lfnCount; };
  static uint8_t sum(const uint8_t* s) { uint8_t x = 0; for (int i = 0; i < 11; ++i) x = (uint8_t)(((x & 1) << 7) + (x >> 1) + s[i]); return x; }
  std::vector<Item> list(uint32_t dirCluster) {
    std::vector<Item> out;
    auto recs = records(dirCluster);
    std::string lfn;
    uint32_t lfnCount = 0;
    for (auto& x : recs) {
      const uint8_t* r = x.r;
      if (r[0] == 0) break;
      if (r[0] == 0xE5) { lfn.clear(); lfnCount = 0; continue; }
      if (r[11] == 0x0F) {
        if (r[0] & 0x40) { lfn.clear(); lfnCount = 0; }
        std::string part;
        static const int pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
        for (int j = 0; j < 13; ++j) {
          const uint16_t u = rd16(r + pos[j]);
          if (u == 0 || u == 0xFFFF) break;
          if (u < 0x80) part += (char)u;
          else if (u < 0x800) { part += (char)(0xC0 | u >> 6); part += (char)(0x80 | (u & 0x3F)); }
          else { part += (char)(0xE0 | u >> 12); part += (char)(0x80 | ((u >> 6) & 0x3F)); part += (char)(0x80 | (u & 0x3F)); }
        }
        lfn = part + lfn;
        ++lfnCount;
        continue;
      }
      if (r[11] & 0x08) { lfn.clear(); lfnCount = 0; continue; }
      if (r[0] == '.') { lfn.clear(); lfnCount = 0; continue; }
      std::string name = lfn;
      if (name.empty()) {
        for (int i = 0; i < 8 && r[i] != ' '; ++i) name += (char)r[i];
        if (r[8] != ' ') { name += '.'; for (int i = 8; i < 11 && r[i] != ' '; ++i) name += (char)r[i]; }
      }
      out.push_back({name, (r[11] & 0x10) != 0, rd16(r + 26) | (uint32_t)rd16(r + 20) << 16, rd32(r + 28), x.index, lfnCount});
      lfn.clear();
      lfnCount = 0;
    }
    return out;
  }
  Item find(uint32_t dir, const std::string& name) {
    for (auto& i : list(dir)) if (i.name == name) return i;
    printf("no %s\n", name.c_str());
    CHECK(false);
    return {};
  }
  // свободный прогон записей
  uint32_t freeRun(uint32_t dirCluster, uint32_t need) {
    auto recs = records(dirCluster);
    uint32_t run = 0;
    for (auto& x : recs) {
      if (x.r[0] == 0 || x.r[0] == 0xE5) { if (++run == need) return x.index - need + 1; }
      else run = 0;
    }
    return (uint32_t)recs.size() - run;   // в конце — с ростом каталога
  }
  // запись каталога: LFN (всегда, кроме имён 8.3 заглавными) и SFN NAME~N
  void addEntry(uint32_t dir, const std::string& name, bool isDir, uint32_t cluster, uint32_t size, unsigned tag) {
    uint8_t sfn[11];
    memset(sfn, ' ', 11);
    char t[8];
    snprintf(t, sizeof t, "WC%05u", tag);
    memcpy(sfn, t, 7);
    sfn[7] = '~';
    memcpy(sfn + 8, isDir ? "   " : "BIN", 3);
    std::vector<uint16_t> units;
    for (size_t i = 0; i < name.size();) {
      const uint8_t c = (uint8_t)name[i];
      uint32_t cp;
      if (c < 0x80) { cp = c; i += 1; }
      else if ((c & 0xE0) == 0xC0) { cp = (c & 0x1F) << 6 | (name[i + 1] & 0x3F); i += 2; }
      else { cp = (c & 0x0F) << 12 | (name[i + 1] & 0x3F) << 6 | (name[i + 2] & 0x3F); i += 3; }
      units.push_back((uint16_t)cp);
    }
    const uint32_t lfns = (uint32_t)((units.size() + 12) / 13);
    const uint32_t at = freeRun(dir, lfns + 1);
    const uint8_t s = sum(sfn);
    for (uint32_t k = 0; k < lfns; ++k) {
      const uint32_t seq = lfns - k;
      uint8_t r[32] = {};
      r[0] = (uint8_t)(seq | (k == 0 ? 0x40 : 0));
      r[11] = 0x0F;
      r[13] = s;
      static const int pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
      for (int j = 0; j < 13; ++j) {
        const size_t u = (seq - 1) * 13 + j;
        wr16(r + pos[j], u < units.size() ? units[u] : u == units.size() ? 0 : 0xFFFF);
      }
      writeRecord(dir, at + k, r);
    }
    uint8_t r[32] = {};
    memcpy(r, sfn, 11);
    r[11] = isDir ? 0x10 : 0x20;
    wr16(r + 22, (uint16_t)((9 << 11) | (15 << 5)));
    wr16(r + 24, (uint16_t)((46 << 9) | (11 << 5) | 19));
    wr16(r + 20, (uint16_t)(cluster >> 16));
    wr16(r + 26, (uint16_t)cluster);
    wr32(r + 28, size);
    writeRecord(dir, at + lfns, r);
  }
  unsigned tags = 0;
  // MKFILE + запись данных (как копирование F5)
  uint32_t mkfile(uint32_t dir, const std::string& name, const std::vector<uint8_t>& bytes) {
    const uint32_t clusters = (uint32_t)((bytes.size() + spc * 512 - 1) / (spc * 512));
    std::vector<uint32_t> ch = clusters ? allocate(clusters) : std::vector<uint32_t>{};
    addEntry(dir, name, false, ch.empty() ? 0 : ch[0], (uint32_t)bytes.size(), ++tags);
    for (size_t off = 0; off < bytes.size(); off += 512) {
      std::vector<uint8_t> s(512, 0);
      memcpy(s.data(), bytes.data() + off, std::min<size_t>(512, bytes.size() - off));
      put(lbaOf(ch[off / (spc * 512)]) + (uint32_t)((off % (spc * 512)) / 512), s);
    }
    v.flush();   // простой после операции: служба дописывает файл (vdisk_service::poll)
    return ch.empty() ? 0 : ch[0];
  }
  uint32_t mkdir(uint32_t dir, const std::string& name) {
    auto c = allocate(1)[0];
    std::vector<uint8_t> s(512, 0);
    memset(s.data(), ' ', 11); s[0] = '.'; s[11] = 0x10; wr16(s.data() + 26, (uint16_t)c); wr16(s.data() + 20, (uint16_t)(c >> 16));
    memset(s.data() + 32, ' ', 11); s[32] = s[33] = '.'; s[43] = 0x10;
    const uint32_t up = dir == root ? 0 : dir;
    wr16(s.data() + 32 + 26, (uint16_t)up); wr16(s.data() + 32 + 20, (uint16_t)(up >> 16));
    put(lbaOf(c), s);
    std::vector<uint8_t> zero(512, 0);
    for (uint32_t k = 1; k < spc; ++k) put(lbaOf(c) + k, zero);
    addEntry(dir, name, true, c, 0, ++tags);
    return c;
  }
  void markDeleted(uint32_t dir, const Item& it) {
    for (uint32_t k = it.lfnCount; k >= 1; --k) {   // LFN, SFN — последней
      uint8_t r[32];
      for (auto& x : records(dir)) if (x.index == it.sfnIndex - k) memcpy(r, x.r, 32);
      r[0] = 0xE5;
      writeRecord(dir, it.sfnIndex - k, r);
    }
    uint8_t r[32];
    for (auto& x : records(dir)) if (x.index == it.sfnIndex) memcpy(r, x.r, 32);
    r[0] = 0xE5;
    writeRecord(dir, it.sfnIndex, r);
  }
  void del(uint32_t dir, const std::string& name) {
    Item it = find(dir, name);
    if (it.cluster) {
      std::vector<std::pair<uint32_t, uint32_t>> links;
      for (uint32_t c : chain(it.cluster)) links.push_back({c, 0});
      setFat(links);
    }
    markDeleted(dir, it);
  }
  void move(uint32_t from, const std::string& name, uint32_t to, const std::string& newName) {
    Item it = find(from, name);
    addEntry(to, newName, it.dir, it.cluster, it.size, ++tags);
    // перенесённый каталог: «..» на нового родителя
    if (it.dir && from != to) {
      auto s = sector(lbaOf(it.cluster));
      const uint32_t up = to == root ? 0 : to;
      wr16(s.data() + 32 + 26, (uint16_t)up); wr16(s.data() + 32 + 20, (uint16_t)(up >> 16));
      put(lbaOf(it.cluster), s);
    }
    markDeleted(from, it);
  }
  // APPEND: хвост цепочки, данные, размер
  void append(uint32_t dir, const std::string& name, const std::vector<uint8_t>& extra) {
    Item it = find(dir, name);
    const uint32_t cb = spc * 512;
    auto ch = chain(it.cluster);
    const uint32_t total = it.size + (uint32_t)extra.size();
    const uint32_t need = (total + cb - 1) / cb;
    if (need > ch.size()) {
      auto add = allocate(need - (uint32_t)ch.size());
      setFat({{ch.back(), add[0]}});
      ch.insert(ch.end(), add.begin(), add.end());
    }
    for (uint32_t pos = 0; pos < extra.size();) {
      const uint32_t at = it.size + pos;
      const uint32_t lba = lbaOf(ch[at / cb]) + (at % cb) / 512;
      auto s = sector(lba);
      const uint32_t in = at % 512, n = std::min<uint32_t>(512 - in, (uint32_t)extra.size() - pos);
      memcpy(s.data() + in, extra.data() + pos, n);
      put(lba, s);
      pos += n;
    }
    uint8_t r[32];
    for (auto& x : records(dir)) if (x.index == it.sfnIndex) memcpy(r, x.r, 32);
    wr32(r + 28, total);
    writeRecord(dir, it.sfnIndex, r);
    v.flush();
  }
  std::vector<uint8_t> content(const Item& it) {
    std::vector<uint8_t> out;
    for (uint32_t c : chain(it.cluster))
      for (uint32_t s = 0; s < spc && out.size() < it.size; ++s) {
        auto sec = sector(lbaOf(c) + s);
        out.insert(out.end(), sec.begin(), sec.begin() + std::min<size_t>(512, it.size - out.size()));
      }
    return out;
  }
};

// Дерево тома (через модель WC) совпадает с «флешкой»
static void same(Wc& wc, Mem& m, uint32_t dir, const std::string& path) {
  auto items = wc.list(dir);
  size_t children = 0;
  for (auto& [p, f] : m.files) if (!p.empty() && Mem::parent(p) == path) ++children;
  if (items.size() != children) {
    printf("dir '%s': volume %zu, flash %zu\n", path.c_str(), items.size(), children);
    for (auto& i : items) printf("  vol %s\n", i.name.c_str());
    for (auto& [p, f] : m.files) if (!p.empty() && Mem::parent(p) == path) printf("  mem %s\n", p.c_str());
  }
  CHECK(items.size() == children);
  for (auto& it : items) {
    const std::string p = path.empty() ? it.name : path + "/" + it.name;
    auto f = m.files.find(p);
    if (f == m.files.end()) printf("missing on flash: %s\n", p.c_str());
    CHECK(f != m.files.end());
    CHECK(f->second.dir == it.dir);
    if (it.dir) same(wc, m, it.cluster, p);
    else {
      CHECK(f->second.data.size() == it.size);
      CHECK(wc.content(it) == f->second.data);
    }
  }
}

int main() {
  Mem m;
  // флешка до подключения: файлы и каталоги (кириллица, длинные, пустой, многокластерный)
  m.files["readme.txt"].data = pattern(1000, 1);
  m.files["Игры"].dir = true;
  m.files["Игры/Elite.sna"].data = pattern(49179, 2);
  m.files["Игры/пусто.txt"].data = {};
  m.files["Музыка"].dir = true;
  m.files["Музыка/song.mid"].data = pattern(70000, 3);
  m.files["Музыка/Глубоко"].dir = true;
  m.files["Музыка/Глубоко/a.bin"].data = pattern(5, 4);
  m.files["big.dat"].data = pattern(200000, 5);

  Volume v;
  v.configureWritable(&m);
  CHECK(v.writable());
  CHECK((v.flags() & kReadOnly) == 0);
  Wc wc(v);
  CHECK(wc.spc == 64);
  const uint32_t root = wc.root;
  same(wc, m, root, "");

  // 1. копирование нового файла в корень: создан с размером, байты — в файл
  auto data1 = pattern(100000, 11);
  const uint32_t first1 = wc.mkfile(root, "Новый файл.bin", data1);
  CHECK(m.files.count("Новый файл.bin") && m.files["Новый файл.bin"].data == data1);
  {
    // 0.1.151: только что записанный WC файл находится по первому кластеру (sd2_open плеера)
    Source* src = nullptr;
    std::vector<char> where;
    uint64_t size = 0;
    CHECK(v.fileAt(first1, &src, where, &size));
    CHECK(src == &m && size == data1.size() && std::string(where.data()) == "Новый файл.bin");
    CHECK(!v.fileAt(first1 + 1, &src, where, &size));   // не первый кластер — нет
  }
  same(wc, m, root, "");

  // 2. пустой файл и файл ровно в кластер
  wc.mkfile(root, "empty.txt", {});
  wc.mkfile(root, "cluster.bin", pattern(32768, 12));
  CHECK(m.files["empty.txt"].data.empty());
  same(wc, m, root, "");

  // 3. каталог и файлы в нём, вложенный каталог
  const uint32_t d = wc.mkdir(root, "Новая папка");
  CHECK(m.files.count("Новая папка") && m.files["Новая папка"].dir);
  wc.mkfile(d, "inner.txt", pattern(700, 13));
  const uint32_t d2 = wc.mkdir(d, "sub");
  wc.mkfile(d2, "deep.bin", pattern(40000, 14));
  CHECK(m.files["Новая папка/sub/deep.bin"].data == pattern(40000, 14));
  same(wc, m, root, "");

  // 4. удаление файла с флешки (собранного томом) и созданного WC
  wc.del(root, "readme.txt");
  CHECK(!m.files.count("readme.txt"));
  wc.del(root, "cluster.bin");
  CHECK(!m.files.count("cluster.bin"));
  same(wc, m, root, "");

  // 5. переименование в том же каталоге и перенос в другой (файл флешки и файл WC)
  const uint32_t games = wc.find(root, "Игры").cluster;
  wc.move(games, "Elite.sna", games, "Elite (1984).sna");
  CHECK(m.files.count("Игры/Elite (1984).sna") && !m.files.count("Игры/Elite.sna"));
  CHECK(m.files["Игры/Elite (1984).sna"].data == pattern(49179, 2));
  wc.move(root, "Новый файл.bin", games, "moved.bin");
  CHECK(m.files.count("Игры/moved.bin") && m.files["Игры/moved.bin"].data == data1);
  same(wc, m, root, "");

  // 6. пустой файл переименовать (без цепочки — по короткому имени)
  wc.move(root, "empty.txt", root, "empty2.txt");
  CHECK(m.files.count("empty2.txt") && !m.files.count("empty.txt"));
  same(wc, m, root, "");

  // 7. каталог флешки — переименовать и перенести внутрь другого
  const uint32_t music = wc.find(root, "Музыка").cluster;
  (void)music;
  wc.move(root, "Музыка", d, "Music");
  CHECK(m.files.count("Новая папка/Music/Глубоко/a.bin"));
  same(wc, m, root, "");

  // 8. дописывание (APPEND) к файлу флешки и к файлу WC
  auto tail = pattern(40000, 15);
  wc.append(games, "moved.bin", tail);
  auto expect = data1;
  expect.insert(expect.end(), tail.begin(), tail.end());
  CHECK(m.files["Игры/moved.bin"].data == expect);
  wc.append(root, "big.dat", pattern(1000, 16));
  auto big = pattern(200000, 5);
  auto t2 = pattern(1000, 16);
  big.insert(big.end(), t2.begin(), t2.end());
  CHECK(m.files["big.dat"].data == big);
  same(wc, m, root, "");

  // 9. удаление дерева: файлы, потом каталоги (как F8)
  const uint32_t music2 = wc.find(d, "Music").cluster;
  const uint32_t deep = wc.find(music2, "Глубоко").cluster;
  wc.del(deep, "a.bin");
  wc.del(music2, "Глубоко");
  wc.del(music2, "song.mid");
  wc.del(d, "Music");
  CHECK(!m.files.count("Новая папка/Music"));
  same(wc, m, root, "");

  // 10. рост каталога за кластер: 1100 файлов в новом каталоге (1024 записи на кластер)
  const uint32_t many = wc.mkdir(root, "many");
  for (int i = 0; i < 1100; ++i) {
    char n[16];
    snprintf(n, sizeof n, "f%04d.txt", i);
    wc.mkfile(many, n, pattern(10 + i % 7, (unsigned)i));
  }
  size_t count = 0;
  for (auto& [p, f] : m.files) if (Mem::parent(p) == "many") ++count;
  CHECK(count == 1100);
  CHECK(m.files["many/f1099.txt"].data == pattern(10 + 1099 % 7, 1099));
  same(wc, m, root, "");

  // 11. ленивый каталог после записей WC: незагруженный каталог флешки получает кластеры мимо
  // занятых WC — новый том, каталог «later» читается только после копирования
  Mem m2;
  m2.files["later"].dir = true;
  m2.files["later/x.bin"].data = pattern(300000, 21);
  Volume v2;
  v2.configureWritable(&m2);
  Wc w2(v2);
  // WC начинает с 15/16 области — подвинуть подсказку к началу, чтобы занять кластеры раскладки
  w2.hint = 6;
  w2.mkfile(w2.root, "first.bin", pattern(150000, 22));
  const uint32_t later = w2.find(w2.root, "later").cluster;
  auto x = w2.find(later, "x.bin");
  CHECK(w2.content(x) == pattern(300000, 21));
  CHECK(w2.content(w2.find(w2.root, "first.bin")) == pattern(150000, 22));
  same(w2, m2, w2.root, "");

  // 12. отказ источника: каталог не создан — запись каталога не легла, WC читает прежнее
  Mem m3;
  Volume v3;
  v3.configureWritable(&m3);
  Wc w3(v3);
  CHECK(w3.list(w3.root).empty());
  m3.fail = true;
  const uint32_t idx = w3.freeRun(w3.root, 1);
  const uint32_t lba = w3.lbaOf(w3.root) + idx / 16;
  uint8_t before[512];
  CHECK(v3.read(lba, before));
  auto c = w3.allocate(1)[0];
  // запись каталога в родителе — отказ источника: том её не принимает
  uint8_t rec[32] = {};
  memcpy(rec, "BLOCK      ", 11);
  rec[11] = 0x10;
  wr16(rec + 26, (uint16_t)c);
  std::vector<uint8_t> s(before, before + 512);
  memcpy(s.data() + (idx % 16) * 32, rec, 32);
  CHECK(!w3.tryPut(lba, s));
  uint8_t after[512];
  CHECK(v3.read(lba, after));
  CHECK(memcmp(before, after, 512) == 0);
  CHECK(!m3.files.count("BLOCK"));

  printf("PASS %u checks: copy (100 KB, empty, cluster), mkdir + nested, delete flash/WC files, rename + "
         "move files and dirs (Cyrillic LFN), empty-file rename, append to flash/WC files, tree delete, dir "
         "growth to 1100 files, lazy dir after WC allocations, source failure keeps the old sector; "
         "%u sector writes\n", checks, wc.sectorWrites);
  return 0;
}
