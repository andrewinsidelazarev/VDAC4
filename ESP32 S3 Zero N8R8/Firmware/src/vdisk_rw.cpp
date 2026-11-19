// Том VDAC4 VFS с записью — режим USB Flash (0.1.148, 19.11.2026). См. vdisk.hpp.
//
// Зачем. Wild Commander знает только FAT32, а флешки бывают exFAT (64 ГБ с завода). Поэтому WC видит
// не сами секторы флешки, а том FAT32, который ESP собирает из её файлов (как том VFS), — и этот том
// принимает запись: WC пишет секторы FAT, каталогов и данных, а том переводит их в операции над
// файлами флешки (создать, писать, удалить, переименовать — через FatFs, она знает exFAT), как сервер
// SMB переводит запросы ПК.
//
// Как WC пишет (CORE32 WC 1.11i, проверено его же кодом на ПК — tests/test_usb_rw_wc.py):
// - новый файл (MKFILE, копирование F5): сначала вся цепочка кластеров в FAT (MKSG), потом запись
//   каталога с первым кластером и итоговым размером, потом данные по цепочке. Поэтому, когда приходит
//   сектор данных, его файл и смещение уже известны — он пишется прямо в файл флешки;
// - новый каталог (MKDIR): сначала тело каталога («.», «..», нули) в новый кластер, потом запись в
//   родителе. Секторы кластера без владельца том держит у себя (накладка) и отдаёт владельцу, когда
//   тот появится;
// - удаление: освобождение цепочки в FAT и пометка записи 0xE5 (SFN — последней);
// - переименование и перенос: новая запись с тем же первым кластером, потом удаление прежней.
//
// Что хранит том. Накладка — секторы, записанные WC, которые нельзя отдать файлу: FAT, каталоги,
// данные кластеров без владельца. Чтение сначала смотрит в неё. Таблица владельцев — кластер ->
// (узел, номер в цепочке) для цепочек, известных по FAT (файлы и каталоги, созданные WC, и хвосты,
// которые WC нарастил). Кластеры файлов флешки, какими их собрал том, — по раскладке (extents_).
//
// Разбор записи каталога: новый сектор сравнивается со старым по 32-байтным записям; сначала
// создания и изменения, потом удаления — перенос (новая запись, затем удаление прежней) не должен
// удалить файл. Узел записи ищется по первому кластеру, у пустых файлов — по короткому имени.
#include "zifi/vdisk.hpp"

#include <string.h>

#include <algorithm>

namespace zifi {
namespace vdisk {

namespace {

constexpr uint32_t kEoc = 0x0FFFFFFF;
constexpr uint32_t kNone = UINT32_MAX;
constexpr uint32_t kPageSectors = 64;
constexpr uint32_t kMaxChain = 0x20000;   // 4 ГиБ по 32 КиБ: длиннее цепочки FAT32 у файла не бывает

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t rd32(const uint8_t* p) { return rd16(p) | (uint32_t)rd16(p + 2) << 16; }
void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

uint32_t recCluster(const uint8_t* r) { return rd16(r + 26) | (uint32_t)rd16(r + 20) << 16; }
bool isLfn(const uint8_t* r) { return r[0] != 0 && r[0] != 0xE5 && r[11] == 0x0F; }
// Живая короткая запись файла или каталога: не пустая, не удалённая, не LFN, не метка, не «.»/«..»
bool isShort(const uint8_t* r) {
  return r[0] != 0 && r[0] != 0xE5 && r[11] != 0x0F && !(r[11] & 0x08) && r[0] != '.';
}
bool isLink(uint32_t v) { return v >= 2 && v < 0x0FFFFFF0; }

uint8_t checksum(const uint8_t* sfn) {
  uint8_t sum = 0;
  for (int i = 0; i < 11; ++i) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + sfn[i]);
  return sum;
}

void putUtf8(std::vector<char>& out, uint32_t cp) {
  if (cp < 0x80) out.push_back((char)cp);
  else if (cp < 0x800) { out.push_back((char)(0xC0 | cp >> 6)); out.push_back((char)(0x80 | (cp & 0x3F))); }
  else if (cp < 0x10000) {
    out.push_back((char)(0xE0 | cp >> 12)); out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back((char)(0x80 | (cp & 0x3F)));
  } else {
    out.push_back((char)(0xF0 | cp >> 18)); out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); out.push_back((char)(0x80 | (cp & 0x3F)));
  }
}

// Знак короткого имени (OEM WC — CP866) в Unicode: кириллица и Ё/ё; прочие старшие — '_'
uint32_t cp866(uint8_t c) {
  if (c < 0x80) return c;
  if (c <= 0xAF) return 0x0410 + (c - 0x80);
  if (c >= 0xE0 && c <= 0xEF) return 0x0440 + (c - 0xE0);
  if (c == 0xF0) return 0x0401;
  if (c == 0xF1) return 0x0451;
  return '_';
}

uint8_t* sectorOf(std::vector<std::vector<uint8_t>>& pages, uint32_t index) {
  return pages[index / kPageSectors].data() + (index % kPageSectors) * kSectorSize;
}

}  // namespace

// ---- накладка ----

int Volume::SectorStore::slotOf(uint32_t lba) const {
  if (keys_.empty()) return -1;
  const size_t mask = keys_.size() - 1;
  for (size_t i = (lba * 2654435761u) & mask, n = 0; n < keys_.size(); i = (i + 1) & mask, ++n) {
    if (keys_[i] == 0) return -1;
    if (keys_[i] == lba + 1) return (int)i;
  }
  return -1;
}

const uint8_t* Volume::SectorStore::find(uint32_t lba) const {
  const int s = slotOf(lba);
  if (s < 0) return nullptr;
  const uint32_t i = index_[s];
  return pages_[i / kPageSectors].data() + (i % kPageSectors) * kSectorSize;
}

bool Volume::SectorStore::put(uint32_t lba, const uint8_t* data) {
  const int s = slotOf(lba);
  if (s >= 0) {
    memcpy(sectorOf(pages_, index_[s]), data, kSectorSize);
    return true;
  }
  if (live_ >= kMaxSectors) return false;
  if ((used_ + 1) * 2 > keys_.size()) {   // заполнено наполовину (с удалёнными) — перестроить вдвое шире
    std::vector<uint32_t> keys = std::move(keys_), index = std::move(index_);
    const size_t size = std::max<size_t>(64, keys.size() * 2);
    keys_.assign(size, 0);
    index_.assign(size, 0);
    used_ = 0;
    for (size_t i = 0; i < keys.size(); ++i) {
      if (keys[i] < 2) continue;
      size_t j = ((keys[i] - 1) * 2654435761u) & (size - 1);
      while (keys_[j]) j = (j + 1) & (size - 1);
      keys_[j] = keys[i];
      index_[j] = index[i];
      ++used_;
    }
  }
  uint32_t at;
  if (!freeList_.empty()) {
    at = freeList_.back();
    freeList_.pop_back();
  } else {
    at = (uint32_t)(pages_.size() * kPageSectors);
    pages_.emplace_back(kPageSectors * kSectorSize);
    for (uint32_t k = kPageSectors; k-- > 1;) freeList_.push_back(at + k);
  }
  memcpy(sectorOf(pages_, at), data, kSectorSize);
  size_t j = (lba * 2654435761u) & (keys_.size() - 1);
  while (keys_[j] >= 2) j = (j + 1) & (keys_.size() - 1);
  if (keys_[j] == 0) ++used_;
  keys_[j] = lba + 1;
  index_[j] = at;
  ++live_;
  return true;
}

void Volume::SectorStore::erase(uint32_t lba) {
  const int s = slotOf(lba);
  if (s < 0) return;
  freeList_.push_back(index_[s]);
  keys_[s] = 1;
  --live_;
}

void Volume::SectorStore::clear() {
  keys_.clear();
  index_.clear();
  pages_.clear();
  freeList_.clear();
  used_ = live_ = 0;
}

// ---- таблица владельцев ----

int Volume::OwnerMap::slotOf(uint32_t cluster) const {
  if (slots_.empty()) return -1;
  const size_t mask = slots_.size() - 1;
  for (size_t i = (cluster * 2654435761u) & mask, n = 0; n < slots_.size(); i = (i + 1) & mask, ++n) {
    if (slots_[i].key == 0) return -1;
    if (slots_[i].key == cluster + 1) return (int)i;
  }
  return -1;
}

bool Volume::OwnerMap::find(uint32_t cluster, uint32_t* node, uint32_t* index) const {
  const int s = slotOf(cluster);
  if (s < 0) return false;
  *node = slots_[s].node;
  *index = slots_[s].index;
  return true;
}

void Volume::OwnerMap::put(uint32_t cluster, uint32_t node, uint32_t index) {
  const int s = slotOf(cluster);
  if (s >= 0) {
    slots_[s].node = node;
    slots_[s].index = index;
    return;
  }
  if ((used_ + 1) * 2 > slots_.size()) {
    std::vector<Slot> old = std::move(slots_);
    const size_t size = std::max<size_t>(256, old.size() * 2);
    slots_.assign(size, Slot{0, 0, 0});
    used_ = 0;
    for (const Slot& x : old) {
      if (x.key < 2) continue;
      size_t j = ((x.key - 1) * 2654435761u) & (size - 1);
      while (slots_[j].key) j = (j + 1) & (size - 1);
      slots_[j] = x;
      ++used_;
    }
  }
  size_t j = (cluster * 2654435761u) & (slots_.size() - 1);
  while (slots_[j].key >= 2) j = (j + 1) & (slots_.size() - 1);
  if (slots_[j].key == 0) ++used_;
  slots_[j] = Slot{cluster + 1, node, index};
  ++live_;
}

void Volume::OwnerMap::erase(uint32_t cluster) {
  const int s = slotOf(cluster);
  if (s < 0) return;
  slots_[s].key = 1;
  --live_;
}

void Volume::OwnerMap::clear() {
  slots_.clear();
  used_ = live_ = 0;
}

// ---- FAT ----

uint32_t Volume::synthFat(uint32_t c) const {
  if (c == 0) return 0x0FFFFFF8;
  if (c == 1) return kEoc;
  const int x = findExtent(c);
  if (x < 0) return 0;
  const Node& n = nodes_[extents_[x].node];
  const uint32_t end = n.cluster + n.clusters - 1;
  return c < end ? c + 1 : kEoc;
}

uint32_t Volume::fatEntry(uint32_t c) const {
  const uint8_t* p = overlay_.find(kReservedSectors + c / (kSectorSize / 4));
  return p ? rd32(p + (c % (kSectorSize / 4)) * 4) & 0x0FFFFFFF : synthFat(c);
}

// Владелец кластера: сначала таблица (цепочки WC), потом раскладка — если FAT не говорит «свободен»
bool Volume::owner(uint32_t c, uint32_t* node, uint32_t* index) const {
  if (owners_.find(c, node, index)) return !(nodes_[*node].flags & kDeleted);
  const int x = findExtent(c);
  if (x < 0) return false;
  const uint32_t n = extents_[x].node;
  if (nodes_[n].flags & kDeleted) return false;
  if (writable_ && fatEntry(c) == 0) return false;   // WC освободил кластер (усечение файла)
  *node = n;
  *index = c - nodes_[n].cluster;
  return true;
}

bool Volume::clusterOf(uint32_t node, uint32_t index, uint32_t* out) const {
  uint32_t c = nodes_[node].cluster;
  for (uint32_t i = 0; i < index; ++i) {
    const uint32_t v = fatEntry(c);
    if (!isLink(v)) return false;
    c = v;
  }
  *out = c;
  return c >= 2;
}

// Первый занятый кластер на [first, first+count) по накладке FAT и таблице владельцев; kNone — свободно
uint32_t Volume::firstUsed(uint32_t first, uint32_t count) const {
  for (uint32_t c = first; c < first + count; ++c) {
    uint32_t n, i;
    if (owners_.find(c, &n, &i)) return c;
    const uint8_t* p = overlay_.find(kReservedSectors + c / (kSectorSize / 4));
    if (p && (rd32(p + (c % (kSectorSize / 4)) * 4) & 0x0FFFFFFF) != 0) return c;
  }
  return kNone;
}

// Раскладка выдала кластеры, а сектор FAT с ними уже в накладке (WC его писал): вписать цепочку туда
void Volume::patchFat(uint32_t first, uint32_t count) {
  for (uint32_t c = first; c < first + count; ++c) {
    for (uint32_t copy = 0; copy < kFatCount; ++copy) {
      const uint32_t lba = kReservedSectors + copy * fatSectors_ + c / (kSectorSize / 4);
      const uint8_t* p = overlay_.find(lba);
      if (!p) continue;
      uint8_t s[kSectorSize];
      memcpy(s, p, kSectorSize);
      wr32(s + (c % (kSectorSize / 4)) * 4, c + 1 < first + count ? c + 1 : kEoc);
      overlay_.put(lba, s);
    }
  }
}

// Кластер стал известен владельцу: запомнить; данные, пришедшие раньше, — в файл, а у каталога —
// разобрать записи, легшие в него до того (новые файлы и каталоги в нём)
void Volume::claim(uint32_t cluster, uint32_t node, uint32_t index) {
  owners_.put(cluster, node, index);
  const uint32_t lba = dataStart() + (cluster - kRootCluster) * kSectorsPerCluster;
  for (uint32_t s = 0; s < kSectorsPerCluster; ++s) {
    const uint8_t* p = overlay_.find(lba + s);
    if (!p) continue;
    uint8_t data[kSectorSize];
    memcpy(data, p, kSectorSize);
    const uint64_t offset = (uint64_t)index * kClusterBytes + (uint64_t)s * kSectorSize;
    if (nodes_[node].flags & kDir) {
      const uint32_t base = (uint32_t)(offset / 32);
      for (uint32_t r = 0; r < kSectorSize / 32; ++r)
        if (isShort(data + r * 32)) created(node, base + r, data + r * 32);
    } else {
      overlay_.erase(lba + s);
      writeFile(node, offset, data);
    }
  }
}

// Цепочка узла по FAT: файлу — на size байтов, каталогу — до конца цепочки
void Volume::registerChain(uint32_t node, uint64_t size) {
  const bool dir = (nodes_[node].flags & kDir) != 0;
  const uint64_t need64 = dir ? kMaxChain : (size + kClusterBytes - 1) / kClusterBytes;
  const uint32_t need = (uint32_t)std::min<uint64_t>(need64, kMaxChain);
  uint32_t c = nodes_[node].cluster;
  for (uint32_t i = 0; i < need && c >= 2; ++i) {
    uint32_t n, k;
    if (!(owners_.find(c, &n, &k) && n == node && k == i)) claim(c, node, i);
    const uint32_t v = fatEntry(c);
    if (!isLink(v)) break;
    c = v;
  }
}

bool Volume::writeFat(uint32_t lba, const uint8_t* data) {
  const bool first = lba < kReservedSectors + fatSectors_;
  uint8_t old[kSectorSize];
  if (first) {
    const uint32_t base = (lba - kReservedSectors) * (kSectorSize / 4);
    for (uint32_t i = 0; i < kSectorSize / 4; ++i) wr32(old + i * 4, fatEntry(base + i));
  }
  if (!overlay_.put(lba, data)) return false;
  if (!first) return true;   // вторая копия — только чтобы WC прочёл своё
  const uint32_t base = (lba - kReservedSectors) * (kSectorSize / 4);
  for (uint32_t i = 0; i < kSectorSize / 4; ++i) {
    const uint32_t c = base + i, was = rd32(old + i * 4) & 0x0FFFFFFF, now = rd32(data + i * 4) & 0x0FFFFFFF;
    if (c < 2 || was == now) continue;
    uint32_t n, k;
    if (now == 0) {
      owners_.erase(c);                     // освобождён (удаление, усечение)
    } else if (isLink(now) && owner(c, &n, &k)) {
      // WC нарастил цепочку (APPEND, каталог полон): хвост — тому же владельцу
      uint32_t to = now, index = k + 1;
      for (uint32_t step = 0; step < kMaxChain && isLink(to); ++step, ++index) {
        uint32_t n2, k2;
        if (owner(to, &n2, &k2)) break;
        claim(to, n, index);
        to = fatEntry(to);
      }
    }
  }
  return true;
}

// ---- каталоги ----

bool Volume::record(uint32_t dir, uint32_t index, uint8_t* out) {
  const uint64_t offset = (uint64_t)index * 32;
  uint32_t c;
  if (!clusterOf(dir, (uint32_t)(offset / kClusterBytes), &c)) return false;
  uint8_t s[kSectorSize];
  const uint32_t lba = dataStart() + (c - kRootCluster) * kSectorsPerCluster +
                       (uint32_t)((offset % kClusterBytes) / kSectorSize);
  if (!read(lba, s)) return false;
  memcpy(out, s + offset % kSectorSize, 32);
  return true;
}

// Имя записи: LFN перед ней (с верной суммой и номерами), иначе короткое имя. UTF-8 в out.
void Volume::entryName(uint32_t dir, uint32_t index, const uint8_t* sfn, std::vector<char>& out) {
  out.clear();
  const uint8_t sum = checksum(sfn);
  uint16_t units[20 * 13];
  size_t count = 0;
  bool done = false;
  for (uint32_t k = 1; k <= 20 && k <= index; ++k) {
    uint8_t r[32];
    if (!record(dir, index - k, r) || !isLfn(r) || r[13] != sum || (r[0] & 0x3F) != k) break;
    static const uint8_t pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    for (int j = 0; j < 13; ++j) units[(k - 1) * 13 + j] = rd16(r + pos[j]);
    count = k * 13;
    if (r[0] & 0x40) { done = true; break; }
  }
  if (done) {
    for (size_t i = 0; i < count; ++i) {
      uint32_t u = units[i];
      if (u == 0 || u == 0xFFFF) break;
      if (u >= 0xD800 && u <= 0xDBFF && i + 1 < count && units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
        u = 0x10000 + ((u - 0xD800) << 10) + (units[++i] - 0xDC00);
      }
      putUtf8(out, u);
    }
    if (!out.empty()) return;
  }
  // короткое: основа и расширение без пробелов; строчные — по флагам NT (байт 12)
  for (int i = 0; i < 8 && sfn[i] != ' '; ++i) {
    uint8_t c = (i == 0 && sfn[0] == 0x05) ? 0xE5 : sfn[i];
    if ((sfn[12] & 0x08) && c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32);
    putUtf8(out, cp866(c));
  }
  if (sfn[8] != ' ') {
    out.push_back('.');
    for (int i = 8; i < 11 && sfn[i] != ' '; ++i) {
      uint8_t c = sfn[i];
      if ((sfn[12] & 0x10) && c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32);
      putUtf8(out, cp866(c));
    }
  }
}

// Узел записи: по первому кластеру (узлы WC и перенесённые — firsts_, прочие — раскладка), у пустого
// файла — по короткому имени среди живых детей каталога
uint32_t Volume::nodeOfRecord(uint32_t dir, const uint8_t* rec) const {
  const uint32_t c = recCluster(rec);
  if (c) {
    uint32_t n, k;
    if (firsts_.find(c, &n, &k) && !(nodes_[n].flags & kDeleted)) return n;
    const int x = findExtent(c);
    if (x >= 0) {
      const uint32_t m = extents_[x].node;
      if (nodes_[m].cluster == c && !(nodes_[m].flags & kDeleted)) return m;
    }
    return kNone;
  }
  for (uint32_t i = 1; i < nodes_.size(); ++i) {
    const Node& n = nodes_[i];
    if (n.parent == dir && !(n.flags & kDeleted) && n.cluster == 0 && !(n.flags & kDir) &&
        memcmp(n.sfn, rec, 11) == 0)
      return i;
  }
  return kNone;
}

uint32_t Volume::appendNode(uint32_t parent, const std::vector<char>& name, bool dir, uint64_t size,
                            const uint8_t* rec, uint32_t slot) {
  if (nodes_.size() >= kMaxNodes) return kNone;
  Node node{};
  node.parent = parent;
  node.size = dir ? 0 : size;
  node.nameOffset = (uint32_t)names_.size();
  node.nameLength = (uint16_t)std::min<size_t>(name.size(), 255);
  names_.insert(names_.end(), name.begin(), name.begin() + node.nameLength);
  node.cluster = recCluster(rec);
  node.date = rd16(rec + 24);
  node.time = rd16(rec + 22);
  memcpy(node.sfn, rec, 11);
  node.source = 0;
  node.flags = (uint8_t)(kWc | (dir ? kDir | kLoaded : 0));
  node.slotDir = parent;
  node.slot = slot;
  const uint32_t index = (uint32_t)nodes_.size();
  nodes_.push_back(node);
  if (node.cluster) firsts_.put(node.cluster, index, 0);
  return index;
}

void Volume::childPath(uint32_t dir, const std::vector<char>& name, std::vector<char>& out) const {
  path(dir, out);
  out.pop_back();   // завершающий 0
  if (!out.empty()) out.push_back('/');
  out.insert(out.end(), name.begin(), name.end());
  out.push_back(0);
}

void Volume::rename(uint32_t node, uint32_t dir, const std::vector<char>& name) {
  Node& n = nodes_[node];
  n.parent = dir;
  n.nameOffset = (uint32_t)names_.size();
  n.nameLength = (uint16_t)std::min<size_t>(name.size(), 255);
  names_.insert(names_.end(), name.begin(), name.begin() + n.nameLength);
}

bool Volume::created(uint32_t dir, uint32_t index, const uint8_t* rec) {
  const bool isDir = (rec[11] & 0x10) != 0;
  const uint64_t size = isDir ? 0 : rd32(rec + 28);
  std::vector<char> name, to;
  entryName(dir, index, rec, name);
  if (name.empty()) return false;
  childPath(dir, name, to);
  const uint32_t existing = nodeOfRecord(dir, rec);
  if (existing != kNone && recCluster(rec) != 0 && ((nodes_[existing].flags & kDir) != 0) == isDir) {
    // перенос или переименование: та же цепочка под новым именем
    std::vector<char> from;
    finishDirty();
    path(existing, from);
    if (strcmp(from.data(), to.data()) != 0 && !sources_[0]->rename(from.data(), to.data())) return false;
    rename(existing, dir, name);
    Node& n = nodes_[existing];
    memcpy(n.sfn, rec, 11);
    n.slotDir = dir;
    n.slot = index;
    n.flags |= kMoved;
    firsts_.put(n.cluster, existing, 0);
    return true;
  }
  if (existing != kNone && recCluster(rec) == 0 && !isDir) return true;   // та же пустая запись
  finishDirty();
  if (isDir ? !sources_[0]->makeDir(to.data()) : !sources_[0]->create(to.data(), size)) return false;
  const uint32_t node = appendNode(dir, name, isDir, size, rec, index);
  if (node == kNone) return false;
  if (!isDir) sources_[0]->setTime(to.data(), nodes_[node].date, nodes_[node].time);
  if (nodes_[node].cluster) registerChain(node, size);
  return true;
}

bool Volume::changed(uint32_t dir, uint32_t index, const uint8_t* was, const uint8_t* rec) {
  const uint32_t node = nodeOfRecord(dir, was);
  if (node == kNone) return created(dir, index, rec);
  const bool isDir = (nodes_[node].flags & kDir) != 0;
  std::vector<char> where;
  path(node, where);
  // размер и цепочка — до переименования: finishDirty перед ним режет файл по размеру из записи
  if (!isDir) {
    Node& f = nodes_[node];
    const uint32_t cluster = recCluster(rec);
    const uint64_t size = rd32(rec + 28);
    if (cluster != f.cluster) {
      // первый кластер у пустого файла (APPEND) или новая цепочка
      if (f.cluster) firsts_.erase(f.cluster);
      f.cluster = cluster;
      f.clusters = 0;
      f.flags |= kWc;                       // кластеры теперь — по FAT
      if (cluster) firsts_.put(cluster, node, 0);
    }
    if (size != f.size) {
      if (!sources_[0]->resize(where.data(), size)) return false;
      f.size = size;
    }
    if (f.cluster && (f.flags & kWc)) registerChain(node, f.size);
  }
  // имя (короткое изменилось — перечитать и длинное)
  if (memcmp(was, rec, 11) != 0) {
    std::vector<char> name, to;
    entryName(dir, index, rec, name);
    childPath(dir, name, to);
    path(node, where);
    finishDirty();
    if (strcmp(where.data(), to.data()) != 0 && !sources_[0]->rename(where.data(), to.data())) return false;
    rename(node, dir, name);
    memcpy(nodes_[node].sfn, rec, 11);
  }
  Node& m = nodes_[node];
  m.slotDir = dir;
  m.slot = index;
  if (!(m.flags & kWc)) m.flags |= kMoved;
  path(node, where);
  const uint16_t date = rd16(rec + 24), time = rd16(rec + 22);
  if (date != m.date || time != m.time) {
    m.date = date;
    m.time = time;
    if (!isDir && dirty_ != node) sources_[0]->setTime(where.data(), date, time);
  }
  return true;
}

bool Volume::removed(uint32_t dir, uint32_t index, const uint8_t* was) {
  const uint32_t node = nodeOfRecord(dir, was);
  if (node == kNone) return true;           // запись, которой файла нет (уже удалён, чужая)
  Node& n = nodes_[node];
  // запись — не дом узла: прежнее место перенесённого файла
  if ((n.flags & (kWc | kMoved)) ? (n.slotDir != dir || n.slot != index) : n.parent != dir) return true;
  std::vector<char> where;
  path(node, where);
  if (dirty_ == node) finishDirty();
  if (!sources_[0]->remove(where.data(), (n.flags & kDir) != 0)) return false;
  n.flags |= kDeleted;
  if (n.cluster) firsts_.erase(n.cluster);
  return true;
}

bool Volume::writeDir(uint32_t node, uint64_t offset, uint32_t lba, const uint8_t* data) {
  uint8_t old[kSectorSize];
  if (!read(lba, old)) return false;
  const uint8_t* had = overlay_.find(lba);
  std::vector<uint8_t> keep;
  if (had) keep.assign(had, had + kSectorSize);
  if (!overlay_.put(lba, data)) return false;
  const uint32_t base = (uint32_t)(offset / 32);
  bool ok = true;
  // сначала создания и изменения, потом удаления: перенос — новая запись, затем удаление прежней
  for (uint32_t r = 0; r < kSectorSize / 32 && ok; ++r) {
    const uint8_t *o = old + r * 32, *n = data + r * 32;
    if (memcmp(o, n, 32) == 0 || !isShort(n)) continue;
    ok = isShort(o) ? changed(node, base + r, o, n) : created(node, base + r, n);
  }
  for (uint32_t r = 0; r < kSectorSize / 32 && ok; ++r) {
    const uint8_t *o = old + r * 32, *n = data + r * 32;
    if (isShort(o) && !isShort(n)) ok = removed(node, base + r, o);
  }
  if (!ok) {                                // операция не вышла: WC прочтёт прежний сектор
    if (had) overlay_.put(lba, keep.data());
    else overlay_.erase(lba);
  }
  return ok;
}

// ---- данные ----

// Сектор пишется в файл целиком, и за концом файла: APPEND кладёт данные за прежний конец раньше,
// чем новый размер в запись каталога. Лишний хвост (последний сектор копии) снимает finishDirty —
// по размеру из записи.
bool Volume::writeFile(uint32_t node, uint64_t offset, const uint8_t* data) {
  const Node& n = nodes_[node];
  if (n.size > 0xFFFFFFFFull) return false;   // файл длиннее 4 ГиБ — в FAT32 только для чтения
  std::vector<char> where;
  path(node, where);
  if (dirty_ != node) finishDirty();
  if (!sources_[0]->write(where.data(), offset, data, kSectorSize)) return false;
  dirty_ = node;
  dirtyEnd_ = std::max<uint64_t>(dirtyEnd_, offset + kSectorSize);
  for (Window& w : windows_)
    if (w.node == node) w.node = UINT32_MAX;
  return true;
}

void Volume::finishDirty() {
  if (dirty_ == kNone || !writable_) return;
  const uint32_t node = dirty_;
  const uint64_t end = dirtyEnd_;
  dirty_ = kNone;
  dirtyEnd_ = 0;
  sources_[0]->flush();
  if (nodes_[node].flags & kDeleted) return;
  std::vector<char> where;
  path(node, where);
  if (end > nodes_[node].size) sources_[0]->resize(where.data(), nodes_[node].size);   // хвост сектора
  if (nodes_[node].date) sources_[0]->setTime(where.data(), nodes_[node].date, nodes_[node].time);
}

void Volume::flush() {
  finishDirty();
  if (writable_ && !sources_.empty()) sources_[0]->flush();
}

bool Volume::write(uint32_t lba, const uint8_t* data) {
  if (!writable_) return false;
  if (!built_) build();
  if (lba >= kTotalSectors) return false;
  if (lba < kReservedSectors) return overlay_.put(lba, data);   // FSInfo: WC пишет подсказку
  if (lba < dataStart()) return writeFat(lba, data);
  const uint32_t rel = lba - dataStart();
  const uint32_t cluster = kRootCluster + rel / kSectorsPerCluster;
  uint32_t node, index;
  if (!owner(cluster, &node, &index)) return overlay_.put(lba, data);   // владельца ещё нет
  const uint64_t offset = (uint64_t)index * kClusterBytes + (uint64_t)(rel % kSectorsPerCluster) * kSectorSize;
  if (nodes_[node].flags & kDir) return writeDir(node, offset, lba, data);
  return writeFile(node, offset, data);
}

}  // namespace vdisk
}  // namespace zifi
