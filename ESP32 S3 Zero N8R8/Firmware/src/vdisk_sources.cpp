// Источники тома VDAC4 VFS — см. zifi/vdisk_sources.hpp
#include "zifi/vdisk_sources.hpp"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#if ZIFI_HAS_SPI_LINK
#include "zifi/tang_sd.hpp"
#endif

namespace zifi {
namespace vdisk_sources {

namespace {

// CP437 0x80…0xFF -> Unicode
const uint16_t kCp437[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

// дата и время изменения в формате FAT
void fatTime(time_t t, uint16_t& date, uint16_t& tm) {
  struct tm local;
  date = tm = 0;
  if (t <= 0 || !gmtime_r(&t, &local) || local.tm_year < 80) return;
  date = (uint16_t)(((local.tm_year - 80) << 9) | ((local.tm_mon + 1) << 5) | local.tm_mday);
  tm = (uint16_t)((local.tm_hour << 11) | (local.tm_min << 5) | (local.tm_sec / 2));
}

}  // namespace

size_t cp437ToUtf8(const char* in, size_t n, char* out) {
  size_t k = 0;
  for (size_t i = 0; i < n; ++i) {
    const uint8_t c = (uint8_t)in[i];
    const uint32_t cp = c < 0x80 ? c : kCp437[c - 0x80];
    if (cp < 0x80) out[k++] = (char)cp;
    else if (cp < 0x800) { out[k++] = (char)(0xC0 | cp >> 6); out[k++] = (char)(0x80 | (cp & 63)); }
    else { out[k++] = (char)(0xE0 | cp >> 12); out[k++] = (char)(0x80 | (cp >> 6 & 63)); out[k++] = (char)(0x80 | (cp & 63)); }
  }
  out[k] = 0;
  return k;
}

size_t utf8ToCp437(const char* in, char* out, size_t capacity) {
  size_t k = 0;
  for (size_t i = 0; in[i] && k + 1 < capacity;) {
    const uint8_t c = (uint8_t)in[i];
    uint32_t cp = c;
    size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
    if (len > 1) {
      cp = c & (0x3F >> (len - 1));
      for (size_t j = 1; j < len; ++j) {
        const uint8_t d = (uint8_t)in[i + j];
        if ((d & 0xC0) != 0x80) { len = j; cp = '?'; break; }
        cp = cp << 6 | (d & 0x3F);
      }
    }
    i += len;
    char o = '?';
    if (cp < 0x80) o = (char)cp;
    else for (int t = 0; t < 128; ++t) if (kCp437[t] == cp) { o = (char)(0x80 + t); break; }
    out[k++] = o;
  }
  out[k] = 0;
  return k;
}

bool PosixSource::full(const char* path, char* out, size_t size) const {
  char oem[512];
  utf8ToCp437(path, oem, sizeof oem);
  const int n = oem[0] ? snprintf(out, size, "%s/%s", mount_, oem) : snprintf(out, size, "%s", mount_);
  return n > 0 && (size_t)n < size;
}

bool PosixSource::list(const char* path, Each each, void* user) {
  char folder[600];
  if (!full(path, folder, sizeof folder)) return false;
  DIR* dir = opendir(folder);
  if (!dir) return false;
  static char item[600 + 256];
  char name[3 * 255 + 1];
  bool ok = true;
  for (dirent* e = readdir(dir); e; e = readdir(dir)) {
    const size_t len = strnlen(e->d_name, 256);
    if (!len || len > 255) continue;
    snprintf(item, sizeof item, "%s/%s", folder, e->d_name);
    struct stat st;
    if (stat(item, &st)) continue;
    cp437ToUtf8(e->d_name, len, name);
    vdisk::Entry entry{name, S_ISDIR(st.st_mode), S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size, 0, 0};
    fatTime(st.st_mtime, entry.date, entry.time);
    if (!each(entry, user)) break;
  }
  closedir(dir);
  return ok;
}

bool PosixSource::read(const char* path, uint64_t offset, uint8_t* out, size_t n, size_t* got) {
  char file[600];
  *got = 0;
  if (!full(path, file, sizeof file)) return false;
  FILE* f = fopen(file, "rb");
  if (!f) return false;
  bool ok = fseek(f, (long)offset, SEEK_SET) == 0;
  if (ok) {
    *got = fread(out, 1, n, f);
    ok = *got == n || !ferror(f);
  }
  fclose(f);
  return ok;
}

#if ZIFI_HAS_SPI_LINK
bool TangSource::list(const char* path, Each each, void* user) {
  tang_sd::Session session(tang_sd::kQuickMs);
  if (!session.ok() || tang_sd::mount()) return false;
  return PosixSource::list(path, each, user);
}

bool TangSource::read(const char* path, uint64_t offset, uint8_t* out, size_t n, size_t* got) {
  tang_sd::Session session(tang_sd::kQuickMs);
  *got = 0;
  if (!session.ok() || tang_sd::mount()) return false;
  return PosixSource::read(path, offset, out, n, got);
}
#endif

}  // namespace vdisk_sources
}  // namespace zifi
