#include "zifi/core_cache_client.hpp"

#include <string.h>

namespace zifi {
namespace core_cache {
namespace {

// Регистры LINK ядра Tang (ts_fb_core.sv) и ящик (bl616/app/fw_mailbox.h, core_mailbox.h)
constexpr uint32_t kRegVersion = 0x04;
constexpr uint32_t kRegCpInfo = 0x30;
constexpr uint32_t kRegMbPage = 0x34;
constexpr uint32_t kRegMbox = 0x200;
constexpr uint32_t kMinCore = 0x0108;
constexpr uint32_t kPageWords = 128;
constexpr uint32_t kPayloadWord = 16;
constexpr uint32_t kPayloadMax = (512 - kPayloadWord) * 4;
constexpr uint32_t kMagic = 0xB7;
constexpr uint32_t kInfoMagic = 0xC2;
constexpr uint32_t kBegin = 1, kData = 2, kEnd = 3, kAbort = 4, kFind = 5, kLoad = 6, kInfo = 7, kFormat = 8;
constexpr uint32_t kFormatKey = 0x21544D46;   // "FMT!"
constexpr uint32_t kOk = 0x00, kLoading = 0x10, kFoundSame = 0x11, kStale = 0x12, kNone = 0x13;

constexpr uint32_t kAckMs = 3000;       // BL616 смотрит ящик раз в 50 мс
constexpr uint32_t kDataMs = 6000;      // посылка со стиранием блока 64 КБ flash BL616
constexpr uint32_t kVerifyMs = 15000;   // SHA-256 ядра во flash BL616 (END, LOAD)
constexpr uint32_t kPollMs = 2;

const char* statusText(uint32_t status) {
  switch (status) {
    case 0x01: return "BL616 cache: command out of order";
    case 0x02: return "BL616 cache: core too big for the cache (or no cache: BL616 flash too small)";
    case 0x03: return "BL616 cache: chunk out of order";
    case 0x04: return "BL616 cache: SHA-256 of the core in BL616 flash does not match";
    case 0x05: return "BL616 cache: not a GW2AR-18 bitstream or multiboot address is not 0";
    case 0x06: return "BL616 cache: no VDAC4 mark in UserCode (core not built for VDAC4)";
    case 0x07: return "BL616 cache: BL616 flash erase/write failed";
    case 0x08: return "BL616 cache: bad core name";
    case 0x09: return "BL616 cache: someone else's data where the cache lives (format needed)";
    case 0x13: return "BL616 cache: no such core";
    case 0x12: return "BL616 cache: the cached core differs (SHA-256)";
    default:
      if ((status & 0xF0) == 0x20) {
        return "BL616: JTAG load of the core failed, Tang is back in slot 0";
      }
      return "BL616 cache: unexpected answer";
  }
}

uint32_t pack(const uint8_t* bytes, size_t size, size_t at) {
  uint32_t word = 0;
  for (size_t i = 0; i < 4 && at + i < size; ++i) {
    word |= static_cast<uint32_t>(bytes[at + i]) << (8 * i);
  }
  return word;
}

// слова 1…13: [1] размер, [2…9] SHA-256, [10…13] имя
void args(uint32_t out[13], uint32_t size, const uint8_t* sha, const char name[16]) {
  memset(out, 0, 13 * 4);
  out[0] = size;
  if (sha != nullptr) {
    for (int i = 0; i < 8; ++i) {
      out[1 + i] = pack(sha, 32, 4 * i);
    }
  }
  if (name != nullptr) {
    for (int i = 0; i < 4; ++i) {
      out[9 + i] = pack(reinterpret_cast<const uint8_t*>(name), 16, 4 * i);
    }
  }
}

}  // namespace

bool normalizeName(const char* name, char out[16]) {
  memset(out, 0, 16);
  size_t n = 0;
  for (; name[n] != 0; ++n) {
    char c = name[n];
    if (n >= 15) {
      return false;
    }
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    if (!ok) {
      return false;
    }
    out[n] = c;
  }
  return n > 0;
}

const char* checkImage(const uint8_t* head, size_t headSize, const uint8_t* tail, size_t tailSize,
                       uint32_t* usercode) {
  static const uint8_t kId[8] = {0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x1B};
  if (headSize < 128 || tailSize < 64) {
    return "core image too small";
  }
  size_t i = 0;
  while (i < 64 && !(head[i] == 0xA5 && head[i + 1] == 0xC3)) {
    ++i;
  }
  if (i >= 64 || memcmp(head + i + 2, kId, 8) != 0) {
    return "not a GW2AR-18 bitstream (no sync word / IDCODE 0000081B)";
  }
  bool jump = false;
  for (size_t j = i + 10; j + 8 <= 128 && head[j] != 0x3B; j += 4) {
    if (head[j] == 0xD2 && head[j + 1] == 0x00 && head[j + 2] == 0xFF && head[j + 3] == 0xFF) {
      if ((head[j + 4] | head[j + 5] | head[j + 6] | head[j + 7]) != 0) {
        return "multiboot jump address is not 0 (the core would not return to slot 0)";
      }
      jump = true;
      break;
    }
  }
  if (!jump) {
    return "no multiboot jump address in the bitstream";
  }
  const uint8_t* t = tail + tailSize - 64;
  for (size_t k = 0; k + 8 <= 64; ++k) {
    if (t[k] == 0x0A && t[k + 1] == 0 && t[k + 2] == 0 && t[k + 3] == 0 && t[k + 4] == 0x56 && t[k + 5] == 0x44 &&
        t[k + 6] == 0x34) {
      if (usercode != nullptr) {
        *usercode = 0x56443400U | t[k + 7];
      }
      return nullptr;
    }
  }
  return "no VDAC4 mark in UserCode (core not built for VDAC4)";
}

bool Client::put(uint32_t first, const uint32_t* words, uint32_t count) {
  while (count > 0) {
    const uint32_t page = first / kPageWords;
    if (page != page_) {
      if (!io_.write(io_.ctx, kRegMbPage, &page, 1)) {
        return false;
      }
      page_ = page;
    }
    uint32_t n = kPageWords - first % kPageWords;
    n = n < 16 ? n : 16;
    n = n < count ? n : count;
    if (!io_.write(io_.ctx, kRegMbox + 4 * (first % kPageWords), words, n)) {
      return false;
    }
    first += n;
    words += n;
    count -= n;
  }
  return true;
}

bool Client::readInfo(uint32_t* value) {
  uint32_t a = 0, b = 0;
  for (int tries = 0; tries < 8; ++tries) {
    if (!io_.read(io_.ctx, kRegCpInfo, &a, 1) || !io_.read(io_.ctx, kRegCpInfo, &b, 1)) {
      return false;
    }
    if (a == b) {
      *value = a;
      return true;
    }
  }
  return false;
}

const char* Client::command(uint32_t cmd, const uint32_t* argWords, uint32_t argCount, const uint32_t* payload,
                            uint32_t payloadWords, uint32_t timeoutMs, uint32_t* status) {
  uint32_t version = 0;
  if (!io_.read(io_.ctx, kRegVersion, &version, 1) || (version & 0xFFFF) < kMinCore || (version >> 24) == 0) {
    return "Tang core has no BL616 mailbox (VDAC4 core 1.8+ needed)";
  }
  if (payloadWords > 0 && !put(kPayloadWord, payload, payloadWords)) {
    return "LINK write failed";
  }
  if (argCount > 0 && !put(1, argWords, argCount)) {
    return "LINK write failed";
  }
  seq_ = (seq_ + 1) & 0xFFFF;
  const uint32_t header = (kMagic << 24) | (cmd << 16) | seq_;
  if (!put(0, &header, 1)) {
    return "LINK write failed";
  }
  const uint32_t start = io_.nowMs(io_.ctx);
  for (;;) {
    uint32_t info = 0;
    if (readInfo(&info) && (info >> 24) == kInfoMagic && (info & 0xFFFF) == seq_) {
      *status = (info >> 16) & 0xFF;
      return nullptr;
    }
    if (io_.nowMs(io_.ctx) - start > timeoutMs) {
      return "BL616 does not answer (firmware 14+ with the core cache needed)";
    }
    io_.sleepMs(io_.ctx, kPollMs);
  }
}

const char* Client::info(uint32_t* flashMb) {
  uint32_t status = 0;
  const char* e = command(kInfo, nullptr, 0, nullptr, 0, kAckMs, &status);
  if (e == nullptr) {
    *flashMb = status;
  }
  return e;
}

const char* Client::find(const char* name, const uint8_t sha[32], Found* found) {
  char n[16];
  if (!normalizeName(name, n)) {
    return "bad core name (up to 15 of A-Z 0-9 _ - .)";
  }
  uint32_t a[13];
  args(a, 0, sha, n);
  uint32_t status = 0;
  const char* e = command(kFind, a, 13, nullptr, 0, kAckMs, &status);
  if (e != nullptr) {
    return e;
  }
  if (status == kFoundSame) {
    *found = Found::kFound;
  } else if (status == kStale) {
    *found = Found::kStale;
  } else if (status == kNone) {
    *found = Found::kNone;
  } else {
    return statusText(status);
  }
  return nullptr;
}

const char* Client::put(const char* name, uint32_t size, const uint8_t sha[32], const ImageReader& reader) {
  char n[16];
  if (!normalizeName(name, n)) {
    return "bad core name (up to 15 of A-Z 0-9 _ - .)";
  }
  // заголовок и конец образа — до передачи: чужое ядро не стирает кэш
  static uint8_t head[128], tail[64];
  if (size < 4096 || !reader.read(reader.ctx, 0, head, sizeof head) ||
      !reader.read(reader.ctx, size - sizeof tail, tail, sizeof tail)) {
    return size < 4096 ? "core image too small" : "core image read failed";
  }
  const char* problem = checkImage(head, sizeof head, tail, sizeof tail, nullptr);
  if (problem != nullptr) {
    return problem;
  }
  uint32_t a[13];
  args(a, size, sha, n);
  uint32_t status = 0;
  const char* e = command(kBegin, a, 13, nullptr, 0, kAckMs, &status);
  if (e == nullptr && status != kOk) {
    e = statusText(status);
  }
  static uint8_t bytes[kPayloadMax];
  static uint32_t payload[kPayloadMax / 4];
  for (uint32_t offset = 0; e == nullptr && offset < size; offset += kPayloadMax) {
    const uint32_t count = size - offset < kPayloadMax ? size - offset : kPayloadMax;
    if (!reader.read(reader.ctx, offset, bytes, count)) {
      e = "core image read failed";
      break;
    }
    const uint32_t words = (count + 3) / 4;
    for (uint32_t i = 0; i < words; ++i) {
      payload[i] = pack(bytes, count, 4 * i);
    }
    const uint32_t chunk[2] = {count, offset};
    e = command(kData, chunk, 2, payload, words, kDataMs, &status);
    if (e == nullptr && status != kOk) {
      e = statusText(status);
    }
    if (e == nullptr && io_.progress != nullptr) {
      io_.progress(io_.ctx, "core", offset + count, size);
    }
  }
  if (e == nullptr) {
    e = command(kEnd, nullptr, 0, nullptr, 0, kVerifyMs, &status);
    if (e == nullptr && status != kOk) {
      e = statusText(status);
    }
  } else {
    uint32_t ignored = 0;
    command(kAbort, nullptr, 0, nullptr, 0, kAckMs, &ignored);
  }
  const uint32_t clear = 0;   // старый заголовок не должен сработать снова
  put(0, &clear, 1);
  return e;
}

const char* Client::load(const char* name, const uint8_t* sha) {
  char n[16];
  if (!normalizeName(name, n)) {
    return "bad core name (up to 15 of A-Z 0-9 _ - .)";
  }
  uint32_t a[13];
  args(a, 0, sha, n);
  uint32_t status = 0;
  const char* e = command(kLoad, a, 13, nullptr, 0, kVerifyMs, &status);
  if (e == nullptr && status != kLoading) {
    e = statusText(status);
  }
  return e;
}

const char* Client::format() {
  const uint32_t key = kFormatKey;
  uint32_t status = 0;
  const char* e = command(kFormat, &key, 1, nullptr, 0, kAckMs, &status);
  if (e == nullptr && status != kOk) {
    e = statusText(status);
  }
  return e;
}

}  // namespace core_cache
}  // namespace zifi
