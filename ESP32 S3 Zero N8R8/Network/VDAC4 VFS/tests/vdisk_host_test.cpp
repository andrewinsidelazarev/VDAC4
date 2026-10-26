// Виртуальный диск VDAC4 VFS на ПК: источники в памяти, том читается независимым мини-читателем
// FAT32 только через Volume::read (как его читал бы WC). Проверяются: загрузочный сектор и FSInfo,
// корень с папками источников и меткой, вложенные каталоги, LFN (кириллица UTF-8, длинные имена,
// совпадающие 8.3), содержимое файлов побайтно (пустой, 1 байт, ровно кластер, 100 КиБ+), ленивое
// чтение каталогов, обрезка каталога длиннее 2048 записей, отказ источника, reset.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "zifi/vdisk.hpp"

using namespace zifi::vdisk;

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { printf("FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)

// ---- источник в памяти ----
struct Fake : Source {
  struct File { std::string name; bool dir; std::vector<uint8_t> data; };
  std::string name_;
  std::map<std::string, std::vector<File>> dirs;   // путь каталога -> записи
  std::vector<std::string> listed;                 // какие каталоги спрашивали
  unsigned reads = 0;
  bool failRead = false;
  explicit Fake(const char* n) : name_(n) {}
  const char* folder() const override { return name_.c_str(); }
  bool list(const char* path, Each each, void* user) override {
    listed.push_back(path);
    auto it = dirs.find(path);
    if (it == dirs.end()) return false;
    for (const File& f : it->second) {
      Entry e{f.name.c_str(), f.dir, f.data.size(), (uint16_t)((45 << 9) | (10 << 5) | 24), (uint16_t)((12 << 11) | (30 << 5))};
      if (!each(e, user)) break;
    }
    return true;
  }
  const File* find(const char* path) const {
    std::string p(path);
    const size_t slash = p.rfind('/');
    const std::string dir = slash == std::string::npos ? "" : p.substr(0, slash);
    const std::string base = slash == std::string::npos ? p : p.substr(slash + 1);
    auto it = dirs.find(dir);
    if (it == dirs.end()) return nullptr;
    for (const File& f : it->second) if (f.name == base && !f.dir) return &f;
    return nullptr;
  }
  bool read(const char* path, uint64_t offset, uint8_t* out, size_t n, size_t* got) override {
    ++reads;
    if (failRead) return false;
    const File* f = find(path);
    if (!f) return false;
    *got = 0;
    if (offset >= f->data.size()) return true;
    *got = std::min<size_t>(n, f->data.size() - (size_t)offset);
    memcpy(out, f->data.data() + offset, *got);
    return true;
  }
  void add(const std::string& dir, const std::string& name, bool isDir, size_t size = 0, unsigned seed = 0) {
    File f{name, isDir, {}};
    for (size_t i = 0; i < size; ++i) f.data.push_back((uint8_t)(i * 31 + seed * 7 + (i >> 9)));
    dirs[dir].push_back(f);
    if (isDir) dirs[dir.empty() ? name : dir + "/" + name];
  }
};

// ---- независимый читатель FAT32 ----
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t* p) { return rd16(p) | (uint32_t)rd16(p + 2) << 16; }

struct Reader {
  Volume& v;
  uint32_t spc = 0, reserved = 0, fats = 0, fatSize = 0, root = 0, data = 0, total = 0;
  explicit Reader(Volume& vol) : v(vol) {}
  std::vector<uint8_t> sector(uint32_t lba) {
    std::vector<uint8_t> s(512);
    CHECK(v.read(lba, s.data()));
    return s;
  }
  void mount() {
    auto b = sector(0);
    CHECK(b[510] == 0x55 && b[511] == 0xAA && !memcmp(b.data() + 82, "FAT32   ", 8));
    CHECK(rd16(&b[11]) == 512);
    spc = b[13]; reserved = rd16(&b[14]); fats = b[16]; total = rd32(&b[32]); fatSize = rd32(&b[36]); root = rd32(&b[44]);
    CHECK(rd16(&b[17]) == 0 && rd16(&b[22]) == 0 && fats == 2 && spc && (spc & (spc - 1)) == 0);
    data = reserved + fats * fatSize;
    const uint32_t clusters = (total - data) / spc;
    CHECK(clusters >= 65525 && (clusters + 2) * 4 <= fatSize * 512);
    auto info = sector(rd16(&b[48]));
    CHECK(rd32(&info[0]) == 0x41615252 && rd32(&info[484]) == 0x61417272 && rd32(&info[508]) == 0xAA550000);
    auto backup = sector(rd16(&b[50]));
    CHECK(backup == b);
  }
  uint32_t fat(uint32_t c, unsigned copy = 0) {
    auto s = sector(reserved + copy * fatSize + c / 128);
    return rd32(&s[(c % 128) * 4]) & 0x0FFFFFFF;
  }
  std::vector<uint32_t> chain(uint32_t c) {
    std::vector<uint32_t> out;
    while (c >= 2 && c < 0x0FFFFFF8) {
      out.push_back(c);
      CHECK(fat(c, 1) == fat(c, 0));
      c = fat(c);
      CHECK(out.size() < 1000000);
    }
    return out;
  }
  std::vector<uint8_t> clusterData(uint32_t c, size_t limit = SIZE_MAX) {
    std::vector<uint8_t> out;
    for (uint32_t c2 : chain(c))
      for (uint32_t s = 0; s < spc && out.size() < limit; ++s) {
        auto d = sector(data + (c2 - 2) * spc + s);
        out.insert(out.end(), d.begin(), d.end());
      }
    if (out.size() > limit) out.resize(limit);
    return out;
  }
  struct Item { std::string name; std::string sfn; bool dir; uint32_t cluster, size; uint8_t attr; };
  static std::string utf8(const std::vector<uint16_t>& u) {
    std::string s;
    for (size_t i = 0; i < u.size(); ++i) {
      uint32_t cp = u[i];
      if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < u.size()) cp = 0x10000 + ((cp - 0xD800) << 10) + (u[++i] - 0xDC00);
      if (cp < 0x80) s += (char)cp;
      else if (cp < 0x800) { s += (char)(0xC0 | cp >> 6); s += (char)(0x80 | (cp & 63)); }
      else if (cp < 0x10000) { s += (char)(0xE0 | cp >> 12); s += (char)(0x80 | (cp >> 6 & 63)); s += (char)(0x80 | (cp & 63)); }
      else { s += (char)(0xF0 | cp >> 18); s += (char)(0x80 | (cp >> 12 & 63)); s += (char)(0x80 | (cp >> 6 & 63)); s += (char)(0x80 | (cp & 63)); }
    }
    return s;
  }
  std::vector<Item> list(uint32_t cluster) {
    auto raw = clusterData(cluster);
    std::vector<Item> items;
    std::vector<uint16_t> lfn;
    int expect = 0;
    uint8_t sum = 0;
    for (size_t at = 0; at + 32 <= raw.size(); at += 32) {
      const uint8_t* r = &raw[at];
      if (r[0] == 0) break;
      if (r[0] == 0xE5) { lfn.clear(); continue; }
      if (r[11] == 0x0F) {
        const int seq = r[0] & 0x1F;
        if (r[0] & 0x40) { lfn.assign(seq * 13, 0xFFFF); expect = seq; sum = r[13]; }
        CHECK(seq == expect && r[13] == sum && expect > 0);
        static const int pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
        for (int j = 0; j < 13; ++j) lfn[(seq - 1) * 13 + j] = rd16(r + pos[j]);
        --expect;
        continue;
      }
      Item it;
      it.sfn.assign((const char*)r, 11);
      it.attr = r[11];
      it.dir = r[11] & 0x10;
      it.cluster = (uint32_t)rd16(r + 20) << 16 | rd16(r + 26);
      it.size = rd32(r + 28);
      if (!lfn.empty()) {
        CHECK(expect == 0);
        uint8_t s = 0;
        for (int i = 0; i < 11; ++i) s = (uint8_t)(((s & 1) << 7) + (s >> 1) + r[i]);
        CHECK(s == sum);
        std::vector<uint16_t> u;
        for (uint16_t c : lfn) { if (c == 0 || c == 0xFFFF) break; u.push_back(c); }
        it.name = utf8(u);
        lfn.clear();
      } else {
        std::string base = it.sfn.substr(0, 8), ext = it.sfn.substr(8);
        while (!base.empty() && base.back() == ' ') base.pop_back();
        while (!ext.empty() && ext.back() == ' ') ext.pop_back();
        it.name = ext.empty() ? base : base + "." + ext;
      }
      items.push_back(it);
    }
    return items;
  }
};

static std::vector<uint8_t> pattern(size_t size, unsigned seed) {
  std::vector<uint8_t> d;
  for (size_t i = 0; i < size; ++i) d.push_back((uint8_t)(i * 31 + seed * 7 + (i >> 9)));
  return d;
}

int main() {
  Fake tang("TANG"), usb("USB");
  tang.add("", "CORES", true);
  tang.add("", "Games", true);
  tang.add("", "readme.txt", false, 1, 1);
  tang.add("", "EMPTY.BIN", false, 0, 2);
  tang.add("CORES", "NES.BIN", false, 907418 % 100000 + 32768, 3);   // больше кластера
  tang.add("CORES", "NES.ELF", false, 32768, 4);                       // ровно кластер
  tang.add("Games", "Super Mario Bros (World).nes", false, 40976, 5);
  tang.add("Games", "Super Mario Bros 3 (Europe).nes", false, 393232, 6);
  tang.add("Games", "Танчики (Денди).nes", false, 24592, 7);
  tang.add("Games", "Sub", true);
  tang.add("Games/Sub", "deep.nes", false, 100, 8);
  tang.add("Games/Sub", "😀 emoji name.txt", false, 5, 9);
  tang.add("Games", "GAME.NES", false, 16, 10);
  tang.add("Games", "game.nes ", false, 17, 11);   // с пробелом: другое имя, то же 8.3
  for (int i = 0; i < 3000; ++i) {
    char name[64];
    snprintf(name, sizeof name, "A long file name number %04d.nes", i);
    usb.add("Many", name, false, 10, (unsigned)i);
  }
  usb.add("", "Many", true);
  Source* sources[] = {&tang, &usb};
  Volume v;
  v.configure(sources, 2);
  CHECK(!(v.flags() & kTruncated));
  Reader r(v);
  r.mount();
  CHECK(tang.listed.empty() && usb.listed.empty());        // монтирование каталогов не читает

  auto root = r.list(r.root);
  CHECK(root.size() == 3);
  CHECK(root[0].attr == 0x08 && root[0].sfn == "VDAC4 VFS  ");
  CHECK(root[1].name == "TANG" && root[1].dir && root[2].name == "USB" && root[2].dir);
  CHECK(tang.listed.empty());                               // корень тома — без источников

  auto top = r.list(root[1].cluster);
  CHECK(tang.listed.size() == 1 && tang.listed[0] == "");
  CHECK(top.size() == 6 && top[0].sfn == ".          " && top[1].sfn == "..         " && top[1].cluster == 0);
  CHECK(top[0].cluster == root[1].cluster);
  std::map<std::string, Reader::Item> byName;
  for (auto& it : top) byName[it.name] = it;
  CHECK(byName.count("CORES") && byName["CORES"].dir && byName["CORES"].sfn == "CORES      ");   // годное 8.3 — без LFN
  CHECK(byName.count("Games") && byName["Games"].dir);
  CHECK(byName["readme.txt"].size == 1 && r.clusterData(byName["readme.txt"].cluster, 1) == pattern(1, 1));
  CHECK(byName["EMPTY.BIN"].size == 0 && byName["EMPTY.BIN"].cluster == 0);

  auto cores = r.list(byName["CORES"].cluster);
  CHECK(cores.size() == 4 && cores[1].cluster == root[1].cluster);   // ".." — папка источника TANG
  for (auto& it : cores) byName["CORES/" + it.name] = it;
  const size_t nesSize = 907418 % 100000 + 32768;
  CHECK(byName["CORES/NES.BIN"].size == nesSize);
  CHECK(r.clusterData(byName["CORES/NES.BIN"].cluster, nesSize) == pattern(nesSize, 3));
  CHECK(r.chain(byName["CORES/NES.ELF"].cluster).size() == 1);
  CHECK(r.clusterData(byName["CORES/NES.ELF"].cluster, 32768) == pattern(32768, 4));

  auto games = r.list(byName["Games"].cluster);
  CHECK(games.size() == 2 + 6);
  CHECK(games[1].cluster == root[1].cluster);
  std::map<std::string, Reader::Item> g;
  std::map<std::string, int> sfns;
  for (auto& it : games) { g[it.name] = it; sfns[it.sfn]++; }
  for (auto& s : sfns) CHECK(s.second == 1);                // короткие имена в каталоге различны
  CHECK(g.count("Super Mario Bros (World).nes") && g["Super Mario Bros (World).nes"].size == 40976);
  CHECK(g.count("Танчики (Денди).nes") && g["Танчики (Денди).nes"].sfn == "______~1NES");
  CHECK(g.count("GAME.NES") && g["GAME.NES"].sfn == "GAME    NES");
  CHECK(g.count("game.nes ") && g["game.nes "].sfn != "GAME    NES");
  CHECK(r.clusterData(g["Super Mario Bros 3 (Europe).nes"].cluster, 393232) == pattern(393232, 6));
  CHECK(r.clusterData(g["Танчики (Денди).nes"].cluster, 24592) == pattern(24592, 7));
  CHECK(r.clusterData(g["GAME.NES"].cluster, 16) == pattern(16, 10));
  auto sub = r.list(g["Sub"].cluster);
  CHECK(sub.size() == 4);
  CHECK(sub[1].cluster == byName["Games"].cluster);
  std::map<std::string, Reader::Item> s2;
  for (auto& it : sub) s2[it.name] = it;
  CHECK(s2.count("😀 emoji name.txt") && r.clusterData(s2["😀 emoji name.txt"].cluster, 5) == pattern(5, 9));
  CHECK(r.clusterData(s2["deep.nes"].cluster, 100) == pattern(100, 8));
  // ленивое чтение: каталоги источника спрошены по разу
  CHECK(tang.listed.size() == 4);

  // каталог длиннее 2048 записей — обрезан, признак kTruncated
  auto usbTop = r.list(root[2].cluster);
  CHECK(usbTop.size() == 3 && usbTop[2].name == "Many");
  auto many = r.list(usbTop[2].cluster);
  CHECK(many.size() == 2 + 1023 && (v.flags() & kTruncated));   // по 4 записи на имя: 4094/4
  CHECK(many[2].name == "A long file name number 0000.nes" && many.back().name.find("A long file name number") == 0);
  CHECK(r.clusterData(many[500].cluster, 10) == pattern(10, 498));

  // отказ источника — сектор не годен (драйвер отдаст ошибку)
  usb.failRead = true;
  {
    uint8_t s[512];
    const uint32_t lba = r.data + (many[100].cluster - 2) * r.spc;
    CHECK(!v.read(lba, s));
  }
  usb.failRead = false;

  // reset: дерево заново, каталоги снова спрашиваются
  const uint32_t gen = v.generation();
  v.reset();
  CHECK(v.generation() == gen + 1);
  tang.listed.clear();
  Reader r2(v);
  r2.mount();
  auto root2 = r2.list(r2.root);
  CHECK(root2.size() == 3 && tang.listed.empty());
  auto top2 = r2.list(root2[1].cluster);
  CHECK(top2.size() == 6 && tang.listed.size() == 1);
  printf("PASS %u checks: FAT32 boot/FSInfo/backup, root folders and label, nested dirs, LFN (Cyrillic, emoji, long),"
         " unique 8.3, file bytes (empty, 1 B, cluster, 384 KiB), lazy listing, >2048 truncation, source failure, reset;"
         " %zu nodes, %u clusters\n", checks, v.nodeCount(), v.clustersUsed());
  return 0;
}
