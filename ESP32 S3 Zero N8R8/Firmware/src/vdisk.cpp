// Виртуальный диск VDAC4 VFS — см. vdisk.hpp
#include "zifi/vdisk.hpp"

#include <string.h>

#include <algorithm>

namespace zifi {
namespace vdisk {

namespace {

constexpr uint32_t kEoc = 0x0FFFFFFF;
constexpr uint32_t kDirRecords = kDirClusters * kClusterBytes / 32;
constexpr uint32_t kWindowBytes = kClusterBytes;
constexpr uint32_t kVolumeId = 0x56443446;   // "F4DV"
const char kLabel[11] = {'V', 'D', 'A', 'C', '4', ' ', 'V', 'F', 'S', ' ', ' '};

void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void put32(uint8_t* p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }

// UTF-8 -> UTF-16; неверная последовательность — '_'. Возвращает число единиц (не больше cap)
size_t utf16(const char* s, size_t n, uint16_t* out, size_t cap) {
  size_t k = 0;
  for (size_t i = 0; i < n;) {
    const uint8_t c = (uint8_t)s[i];
    uint32_t cp = '_';
    size_t len = 1;
    if (c < 0x80) cp = c;
    else if ((c & 0xE0) == 0xC0) len = 2, cp = c & 0x1F;
    else if ((c & 0xF0) == 0xE0) len = 3, cp = c & 0x0F;
    else if ((c & 0xF8) == 0xF0) len = 4, cp = c & 0x07;
    else len = 1, cp = '_';
    if (len > 1) {
      if (i + len > n) { cp = '_'; len = 1; }
      else {
        for (size_t j = 1; j < len; ++j) {
          const uint8_t d = (uint8_t)s[i + j];
          if ((d & 0xC0) != 0x80) { cp = '_'; len = 1; break; }
          cp = cp << 6 | (d & 0x3F);
        }
      }
    }
    i += len;
    if (cp >= 0x10000 && cp <= 0x10FFFF) {
      if (k + 2 > cap) break;
      cp -= 0x10000;
      out[k++] = (uint16_t)(0xD800 | (cp >> 10));
      out[k++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
    } else {
      if (k + 1 > cap) break;
      out[k++] = (uint16_t)(cp >= 0xD800 && cp <= 0xDFFF ? '_' : cp);
    }
  }
  return k;
}

bool sfnChar(uint8_t c) {
  return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("!#$%&'()-@^_`{}~", c) != nullptr;
}

uint8_t checksum(const uint8_t sfn[11]) {
  uint8_t sum = 0;
  for (int i = 0; i < 11; ++i) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + sfn[i]);
  return sum;
}

}  // namespace

void Volume::configure(Source* const* sources, size_t count) {
  sources_.assign(sources, sources + count);
  uint32_t fat = 1;
  for (;;) {
    const uint32_t data = kTotalSectors - kReservedSectors - kFatCount * fat;
    const uint32_t clusters = data / kSectorsPerCluster;
    const uint32_t need = (uint32_t)(((uint64_t)clusters + 2) * 4 + kSectorSize - 1) / kSectorSize;
    if (need <= fat) { clusterCount_ = clusters; break; }
    fat = need;
  }
  fatSectors_ = fat;
  reset();
}

void Volume::reset() {
  built_ = false;
  ++generation_;
  for (Window& w : windows_) w.node = UINT32_MAX;
}

uint8_t Volume::flags() const { return kReadOnly | (sources_.empty() ? 0 : kReady) | (truncated_ ? kTruncated : 0); }

void Volume::build() {
  nodes_.clear();
  extents_.clear();
  names_.clear();
  nextCluster_ = kRootCluster;
  truncated_ = false;
  addNode(0, "", 0, true, 0, 0, 0, 0xFF);   // корень: кластеры 2…
  Node& root = nodes_[0];
  root.firstChild = (uint32_t)nodes_.size();
  for (size_t i = 0; i < sources_.size(); ++i) {
    const char* name = sources_[i]->folder();
    addNode(0, name, strlen(name), true, 0, 0, 0, (uint8_t)i);
  }
  nodes_[0].childCount = (uint32_t)(nodes_.size() - nodes_[0].firstChild);
  nodes_[0].flags |= kLoaded;
  for (uint32_t i = nodes_[0].firstChild; i < nodes_.size(); ++i) makeShortName(0, nodes_[0].firstChild, i);
  built_ = true;
}

uint32_t Volume::addNode(uint32_t parent, const char* name, size_t nameLength, bool dir, uint64_t size,
                         uint16_t date, uint16_t time, uint8_t source) {
  Node node{};
  node.parent = parent;
  node.size = dir ? 0 : size;
  node.nameOffset = (uint32_t)names_.size();
  node.nameLength = (uint16_t)std::min<size_t>(nameLength, 255);
  node.date = date;
  node.time = time;
  node.source = source;
  node.flags = dir ? kDir : 0;
  names_.insert(names_.end(), name, name + node.nameLength);
  const uint32_t index = (uint32_t)nodes_.size();
  nodes_.push_back(node);
  Node& n = nodes_.back();
  const uint32_t clusters = dir ? kDirClusters : (uint32_t)((size + kClusterBytes - 1) / kClusterBytes);
  if (!allocate(n, clusters)) {
    truncated_ = true;
    n.size = 0;   // места нет: файл пустой, каталог — без кластеров (WC его не откроет)
  }
  if (n.clusters) extents_.push_back({n.cluster, index});
  return index;
}

bool Volume::allocate(Node& node, uint32_t clusters) {
  node.cluster = node.clusters = 0;
  if (!clusters) return true;
  if ((uint64_t)nextCluster_ + clusters > (uint64_t)clusterCount_ + 2) return false;
  node.cluster = nextCluster_;
  node.clusters = clusters;
  nextCluster_ += clusters;
  return true;
}

// Короткое имя 8.3: годное — как есть, без LFN; иначе основа~N.РАС и записи LFN
void Volume::makeShortName(uint32_t parent, uint32_t first, uint32_t index) {
  (void)parent;
  Node& n = nodes_[index];
  const char* name = &names_[n.nameOffset];
  const size_t len = n.nameLength;
  memset(n.sfn, ' ', 11);
  // уже 8.3 заглавными?
  const char* dot = nullptr;
  bool valid = len > 0 && len <= 12 && name[0] != '.';
  for (size_t i = 0; i < len && valid; ++i) {
    const uint8_t c = (uint8_t)name[i];
    if (c == '.') { if (dot) valid = false; dot = name + i; }
    else if (!sfnChar(c)) valid = false;
  }
  if (valid) {
    const size_t base = dot ? (size_t)(dot - name) : len;
    const size_t ext = dot ? len - base - 1 : 0;
    valid = base >= 1 && base <= 8 && ext <= 3 && (!dot || ext >= 1);
    if (valid) {
      memcpy(n.sfn, name, base);
      if (ext) memcpy(n.sfn + 8, dot + 1, ext);
    }
  }
  bool unique = valid;
  for (uint32_t i = first; i < index && unique; ++i) unique = memcmp(nodes_[i].sfn, n.sfn, 11) != 0;
  if (valid && unique) {
    n.lfnCount = 0;
    if (n.sfn[0] == 0xE5) n.sfn[0] = 0x05;
    return;
  }
  // основа и расширение из имени: заглавные ASCII, прочее — '_', пробелы и точки выкинуты
  const char* last = nullptr;
  for (size_t i = 0; i < len; ++i) if (name[i] == '.') last = name + i;
  if (last == name) last = nullptr;   // имя с точки — без расширения
  uint8_t base[8], ext[3];
  size_t b = 0, e = 0;
  for (size_t i = 0; i < len && b < 8; ++i) {
    if (last && name + i == last) break;
    uint8_t c = (uint8_t)name[i];
    if (c == ' ' || c == '.') continue;
    if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 32);
    if (c >= 0x80) {
      while (i + 1 < len && ((uint8_t)name[i + 1] & 0xC0) == 0x80) ++i;   // весь знак UTF-8 — одним '_'
      c = '_';
    } else if (!sfnChar(c)) c = '_';
    base[b++] = c;
  }
  if (last) {
    for (const char* p = last + 1; p < name + len && e < 3; ++p) {
      uint8_t c = (uint8_t)*p;
      if (c == ' ' || c == '.') continue;
      if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 32);
      if (c >= 0x80) {
        while (p + 1 < name + len && ((uint8_t)p[1] & 0xC0) == 0x80) ++p;
        c = '_';
      } else if (!sfnChar(c)) c = '_';
      ext[e++] = c;
    }
  }
  if (!b) base[b++] = '_';
  for (uint32_t tail = 1; tail < 1000000; ++tail) {
    char digits[8];
    int d = 0;
    for (uint32_t t = tail; t; t /= 10) digits[d++] = (char)('0' + t % 10);
    const size_t keep = std::min<size_t>(b, 8 - 1 - (size_t)d);
    uint8_t candidate[11];
    memset(candidate, ' ', 11);
    memcpy(candidate, base, keep);
    candidate[keep] = '~';
    for (int i = 0; i < d; ++i) candidate[keep + 1 + i] = (uint8_t)digits[d - 1 - i];
    memcpy(candidate + 8, ext, e);
    bool taken = false;
    for (uint32_t i = first; i < index && !taken; ++i) taken = memcmp(nodes_[i].sfn, candidate, 11) == 0;
    if (!taken) { memcpy(n.sfn, candidate, 11); break; }
  }
  uint16_t units[260];
  const size_t count = utf16(name, len, units, 255);
  n.lfnCount = (uint8_t)((count + 12) / 13);
  if (!n.lfnCount) n.lfnCount = 1;
}

bool Volume::path(uint32_t index, std::vector<char>& out) const {
  std::vector<uint32_t> chain;
  for (uint32_t i = index; i != 0; i = nodes_[i].parent) chain.push_back(i);
  out.clear();
  // chain.back() — папка источника в корне: в путь источника не входит
  for (size_t k = chain.size(); k-- > 0;) {
    if (k == chain.size() - 1) continue;
    const Node& n = nodes_[chain[k]];
    if (!out.empty()) out.push_back('/');
    out.insert(out.end(), names_.begin() + n.nameOffset, names_.begin() + n.nameOffset + n.nameLength);
  }
  out.push_back(0);
  return true;
}

namespace {
struct Collect {
  std::vector<Entry> entries;
  std::vector<char> pool;
  std::vector<size_t> offsets;
  uint32_t records = 0;
  bool full = false;
};
bool collect(const Entry& e, void* user) {
  Collect& c = *static_cast<Collect*>(user);
  if (!e.name || !e.name[0] || !strcmp(e.name, ".") || !strcmp(e.name, "..")) return true;
  const size_t len = std::min<size_t>(strlen(e.name), 255);
  uint16_t units[260];
  const size_t count = utf16(e.name, len, units, 255);
  const uint32_t records = 1 + (uint32_t)((count + 12) / 13);   // с запасом: LFN есть почти у всех
  if (c.records + records > kDirRecords) { c.full = true; return false; }
  c.records += records;
  c.offsets.push_back(c.pool.size());
  c.pool.insert(c.pool.end(), e.name, e.name + len);
  c.pool.push_back(0);
  c.entries.push_back(e);
  return true;
}
}  // namespace

bool Volume::load(uint32_t index) {
  if (nodes_[index].flags & kLoaded) return true;
  const uint8_t source = nodes_[index].source;
  if (source >= sources_.size()) return false;
  std::vector<char> where;
  path(index, where);
  Collect c;
  c.records = 2;   // "." и ".."
  if (!sources_[source]->list(where.data(), collect, &c)) return false;
  if (c.full) truncated_ = true;
  const uint32_t first = (uint32_t)nodes_.size();
  for (size_t i = 0; i < c.entries.size(); ++i) {
    if (nodes_.size() >= kMaxNodes) { truncated_ = true; break; }
    const Entry& e = c.entries[i];
    const char* name = &c.pool[c.offsets[i]];
    addNode(index, name, strlen(name), e.dir, e.size, e.date, e.time, source);
  }
  Node& n = nodes_[index];   // после push_back ссылки устарели
  n.firstChild = first;
  n.childCount = (uint32_t)(nodes_.size() - first);
  n.flags |= kLoaded;
  for (uint32_t i = first; i < nodes_.size(); ++i) makeShortName(index, first, i);
  return true;
}

int Volume::findExtent(uint32_t cluster) const {
  // extents_ растут по кластерам (выдача по порядку)
  size_t lo = 0, hi = extents_.size();
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const Extent& x = extents_[mid];
    const Node& n = nodes_[x.node];
    if (cluster < x.cluster) hi = mid;
    else if (cluster >= x.cluster + n.clusters) lo = mid + 1;
    else return (int)mid;
  }
  return -1;
}

void Volume::renderBoot(uint8_t* p) const {
  memset(p, 0, kSectorSize);
  p[0] = 0xEB; p[1] = 0x58; p[2] = 0x90;
  memcpy(p + 3, "VDAC4VFS", 8);
  put16(p + 11, kSectorSize);
  p[13] = kSectorsPerCluster;
  put16(p + 14, kReservedSectors);
  p[16] = kFatCount;
  p[21] = 0xF8;
  put16(p + 24, 63);
  put16(p + 26, 255);
  put32(p + 32, kTotalSectors);
  put32(p + 36, fatSectors_);
  put32(p + 44, kRootCluster);
  put16(p + 48, 1);   // FSInfo
  put16(p + 50, 6);   // копия загрузочного
  p[64] = 0x80;
  p[66] = 0x29;
  put32(p + 67, kVolumeId);
  memcpy(p + 71, kLabel, 11);
  memcpy(p + 82, "FAT32   ", 8);
  p[510] = 0x55; p[511] = 0xAA;
}

void Volume::renderInfo(uint8_t* p) const {
  memset(p, 0, kSectorSize);
  put32(p, 0x41615252);
  put32(p + 484, 0x61417272);
  put32(p + 488, 0);            // свободного нет: только чтение
  put32(p + 492, 0xFFFFFFFF);
  put32(p + 508, 0xAA550000);
}

void Volume::renderFat(uint32_t sector, uint8_t* out) const {
  const uint32_t first = sector * (kSectorSize / 4);
  // первый участок, который может задеть сектор
  size_t lo = 0, hi = extents_.size();
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const Extent& x = extents_[mid];
    if (x.cluster + nodes_[x.node].clusters <= first) lo = mid + 1;
    else hi = mid;
  }
  size_t k = lo;
  for (uint32_t i = 0; i < kSectorSize / 4; ++i) {
    const uint32_t c = first + i;
    uint32_t value = 0;
    if (c == 0) value = 0x0FFFFFF8;
    else if (c == 1) value = kEoc;
    else {
      while (k < extents_.size() && extents_[k].cluster + nodes_[extents_[k].node].clusters <= c) ++k;
      if (k < extents_.size() && c >= extents_[k].cluster) {
        const uint32_t end = extents_[k].cluster + nodes_[extents_[k].node].clusters - 1;
        value = c < end ? c + 1 : kEoc;
      }
    }
    put32(out + i * 4, value);
  }
}

bool Volume::renderDir(uint32_t index, uint64_t offset, uint8_t* out) {
  memset(out, 0, kSectorSize);
  if (!load(index)) return false;
  const uint32_t from = (uint32_t)(offset / 32), to = from + kSectorSize / 32;
  uint32_t record = 0;
  auto emit = [&](const uint8_t* r) {
    if (record >= from && record < to) memcpy(out + (record - from) * 32, r, 32);
    ++record;
  };
  const Node self = nodes_[index];
  uint8_t r[32];
  auto shortEntry = [&](const uint8_t name[11], uint8_t attr, uint32_t cluster, uint32_t size, uint16_t date,
                        uint16_t time) {
    memset(r, 0, 32);
    memcpy(r, name, 11);
    r[11] = attr;
    put16(r + 14, time);
    put16(r + 16, date);
    put16(r + 18, date);
    put16(r + 20, (uint16_t)(cluster >> 16));
    put16(r + 22, time);
    put16(r + 24, date);
    put16(r + 26, (uint16_t)cluster);
    put32(r + 28, size);
    emit(r);
  };
  if (index == 0) {
    shortEntry((const uint8_t*)kLabel, 0x08, 0, 0, 0, 0);
  } else {
    uint8_t dot[11], dotdot[11];
    memset(dot, ' ', 11); dot[0] = '.';
    memset(dotdot, ' ', 11); dotdot[0] = dotdot[1] = '.';
    shortEntry(dot, 0x10, self.cluster, 0, self.date, self.time);
    shortEntry(dotdot, 0x10, self.parent == 0 ? 0 : nodes_[self.parent].cluster, 0, self.date, self.time);
  }
  for (uint32_t i = 0; i < self.childCount && record < to; ++i) {
    const Node& n = nodes_[self.firstChild + i];
    if ((n.flags & kDir) && !n.clusters) continue;   // каталог без места — не показывать
    if (n.lfnCount) {
      uint16_t units[260];
      const size_t count = utf16(&names_[n.nameOffset], n.nameLength, units, 255);
      const uint8_t sum = checksum(n.sfn);
      static const uint8_t pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
      for (int seq = n.lfnCount; seq >= 1; --seq) {
        memset(r, 0, 32);
        r[0] = (uint8_t)(seq | (seq == n.lfnCount ? 0x40 : 0));
        r[11] = 0x0F;
        r[13] = sum;
        for (int j = 0; j < 13; ++j) {
          const size_t u = (size_t)(seq - 1) * 13 + j;
          uint16_t v = u < count ? units[u] : u == count ? 0x0000 : 0xFFFF;
          put16(r + pos[j], v);
        }
        emit(r);
      }
    }
    const uint32_t size = (n.flags & kDir) ? 0 : (uint32_t)std::min<uint64_t>(n.size, 0xFFFFFFFFu);
    shortEntry(n.sfn, (n.flags & kDir) ? 0x10 : 0x21, n.cluster, size, n.date, n.time);
  }
  return true;
}

bool Volume::renderFile(uint32_t index, uint64_t offset, uint8_t* out) {
  memset(out, 0, kSectorSize);
  const Node& n = nodes_[index];
  if (offset >= n.size) return true;
  const uint64_t base = offset & ~(uint64_t)(kWindowBytes - 1);
  Window* w = nullptr;
  for (Window& x : windows_)
    if (x.node == index && x.offset == base) w = &x;
  if (!w) {
    w = &windows_[nextWindow_];
    nextWindow_ = (nextWindow_ + 1) % 2;
    w->node = UINT32_MAX;
    w->data.resize(kWindowBytes);
    const size_t want = (size_t)std::min<uint64_t>(kWindowBytes, n.size - base);
    std::vector<char> where;
    path(index, where);
    size_t got = 0;
    if (n.source >= sources_.size() || !sources_[n.source]->read(where.data(), base, w->data.data(), want, &got))
      return false;
    w->node = index;
    w->offset = base;
    w->length = got;
  }
  const size_t at = (size_t)(offset - base);
  if (at < w->length) memcpy(out, w->data.data() + at, std::min<size_t>(kSectorSize, w->length - at));
  return true;
}

bool Volume::read(uint32_t lba, uint8_t* out) {
  if (!built_) build();
  memset(out, 0, kSectorSize);
  if (lba >= kTotalSectors) return false;
  if (lba == 0 || lba == 6) { renderBoot(out); return true; }
  if (lba == 1 || lba == 7) { renderInfo(out); return true; }
  if (lba < kReservedSectors) return true;
  if (lba < dataStart()) { renderFat((lba - kReservedSectors) % fatSectors_, out); return true; }
  const uint32_t rel = lba - dataStart();
  const uint32_t cluster = kRootCluster + rel / kSectorsPerCluster;
  const int x = findExtent(cluster);
  if (x < 0) return true;
  const uint32_t index = extents_[x].node;
  const Node& n = nodes_[index];
  const uint64_t offset = (uint64_t)(cluster - n.cluster) * kClusterBytes + (uint64_t)(rel % kSectorsPerCluster) * kSectorSize;
  return (n.flags & kDir) ? renderDir(index, offset, out) : renderFile(index, offset, out);
}

}  // namespace vdisk
}  // namespace zifi
