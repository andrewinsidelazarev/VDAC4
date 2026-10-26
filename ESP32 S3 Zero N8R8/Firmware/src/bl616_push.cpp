#include "zifi/bl616_push.hpp"

#include <string.h>

namespace zifi {
namespace bl616 {
namespace {

// Регистры LINK ядра Tang (ts_fb_core.sv)
constexpr uint32_t kRegVersion = 0x04;
constexpr uint32_t kRegCpInfo = 0x30;
constexpr uint32_t kRegMbPage = 0x34;
constexpr uint32_t kRegMbox = 0x200;
constexpr uint32_t kMinCore = 0x0108;

// Почтовый ящик (bl616/app/fw_mailbox.h)
constexpr uint32_t kPageWords = 128;
constexpr uint32_t kPayloadWord = 16;
constexpr uint32_t kPayloadMax = (512 - kPayloadWord) * 4;
constexpr uint32_t kMagic = 0xB6;
constexpr uint32_t kBegin = 1, kData = 2, kEnd = 3;
constexpr uint32_t kInfoNormal = 0xC0, kInfoUpdate = 0xC1;
constexpr uint32_t kOk = 0x00, kWriting = 0x10;

constexpr uint32_t kAckMs = 3000;      // BL616 смотрит ящик раз в 50 мс, в обновлении — всё время
constexpr uint32_t kEndMs = 10000;     // SHA-256 образа в RAM
constexpr uint32_t kRebootMs = 30000;  // запись flash, перезагрузка, связь с ПЛИС
constexpr uint32_t kPollMs = 2;

const char* statusText(uint32_t status) {
  switch (status) {
    case 0x01: return "BL616: command without BEGIN";
    case 0x02: return "BL616: bad size or no memory for the image";
    case 0x03: return "BL616: chunk outside the image";
    case 0x04: return "BL616: SHA-256 of the assembled image does not match";
    case 0x05: return "BL616: not a BL616 image (no BFNP header)";
    default: return "BL616: unexpected answer";
  }
}

class Mailbox {
 public:
  explicit Mailbox(const PushIo& io) : io_(io) {}

  // Слова ящика с номера first: по страницам окна, по 16 слов за обмен.
  bool put(uint32_t first, const uint32_t* words, uint32_t count) {
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

 private:
  const PushIo& io_;
  uint32_t page_ = 0xFFFFFFFF;
};

// CP_INFO проходит в домен LINK без рукопожатия: верим двум одинаковым чтениям подряд.
bool readInfo(const PushIo& io, uint32_t* value) {
  uint32_t a = 0, b = 0;
  for (int tries = 0; tries < 8; ++tries) {
    if (!io.read(io.ctx, kRegCpInfo, &a, 1) || !io.read(io.ctx, kRegCpInfo, &b, 1)) {
      return false;
    }
    if (a == b) {
      *value = a;
      return true;
    }
  }
  return false;
}

uint32_t pack(const uint8_t* bytes, size_t size, size_t at) {
  uint32_t word = 0;
  for (size_t i = 0; i < 4 && at + i < size; ++i) {
    word |= static_cast<uint32_t>(bytes[at + i]) << (8 * i);
  }
  return word;
}

// Команда: аргументы (слова 1…), данные (с 16), затем заголовок; ждать ответа с её номером.
const char* command(const PushIo& io, Mailbox& box, uint32_t cmd, uint32_t seq, const uint32_t* args,
                    uint32_t argCount, const uint32_t* payload, uint32_t payloadWords, uint32_t timeoutMs,
                    uint32_t* status) {
  if (payloadWords > 0 && !box.put(kPayloadWord, payload, payloadWords)) {
    return "LINK write failed";
  }
  if (argCount > 0 && !box.put(1, args, argCount)) {
    return "LINK write failed";
  }
  const uint32_t header = (kMagic << 24) | (cmd << 16) | (seq & 0xFFFF);
  if (!box.put(0, &header, 1)) {
    return "LINK write failed";
  }
  const uint32_t start = io.nowMs(io.ctx);
  for (;;) {
    uint32_t info = 0;
    if (readInfo(io, &info) && (info >> 24) == kInfoUpdate && (info & 0xFFFF) == (seq & 0xFFFF)) {
      *status = (info >> 16) & 0xFF;
      return nullptr;
    }
    if (io.nowMs(io.ctx) - start > timeoutMs) {
      return "BL616 does not answer (old firmware without network update, or not running)";
    }
    io.sleepMs(io.ctx, kPollMs);
  }
}

}  // namespace

const char* checkImage(const uint8_t* image, size_t size) {
  if (size < 4096 || size > kMaxImage) {
    return "image size must be 4 KB..256 KB";
  }
  if (memcmp(image, "BFNP", 4) != 0) {
    return "no BFNP header: not a BL616 image";
  }
  return nullptr;
}

const char* push(const PushIo& io, const uint8_t* image, size_t size, const uint8_t digest[32],
                 uint32_t seed, uint32_t* newInfo) {
  const char* problem = checkImage(image, size);
  if (problem != nullptr) {
    return problem;
  }
  uint32_t version = 0;
  if (!io.read(io.ctx, kRegVersion, &version, 1) || (version & 0xFFFF) < kMinCore) {
    return "Tang core older than 1.8 (no BL616 mailbox)";
  }
  uint32_t before = 0;
  if (!readInfo(io, &before) || ((before >> 24) != kInfoNormal && (before >> 24) != kInfoUpdate)) {
    return "BL616 firmware is not running (CP_INFO has no C0 mark)";
  }

  Mailbox box(io);
  uint32_t seq = seed & 0xFFFF;
  uint32_t status = 0;
  const char* error = nullptr;

  uint32_t args[9] = {static_cast<uint32_t>(size)};
  for (int i = 0; i < 8; ++i) {
    args[1 + i] = pack(digest, 32, 4 * i);
  }
  error = command(io, box, kBegin, ++seq, args, 9, nullptr, 0, kAckMs, &status);
  if (error == nullptr && status != kOk) {
    error = statusText(status);
  }

  static uint32_t payload[kPayloadMax / 4];
  for (size_t offset = 0; error == nullptr && offset < size; offset += kPayloadMax) {
    const uint32_t bytes = static_cast<uint32_t>(size - offset < kPayloadMax ? size - offset : kPayloadMax);
    const uint32_t words = (bytes + 3) / 4;
    for (uint32_t i = 0; i < words; ++i) {
      payload[i] = pack(image, size, offset + 4 * i);
    }
    const uint32_t chunk[2] = {bytes, static_cast<uint32_t>(offset)};
    error = command(io, box, kData, ++seq, chunk, 2, payload, words, kAckMs, &status);
    if (error == nullptr && status != kOk) {
      error = statusText(status);
    }
    if (error == nullptr && io.progress != nullptr) {
      io.progress(io.ctx, "send", static_cast<uint32_t>(offset + bytes), static_cast<uint32_t>(size));
    }
  }

  if (error == nullptr) {
    error = command(io, box, kEnd, ++seq, nullptr, 0, nullptr, 0, kEndMs, &status);
    if (error == nullptr && status != kWriting) {
      error = statusText(status);
    }
  }

  if (error == nullptr) {
    // BL616 пишет flash и перезагружается: ждать обычного CP_INFO, у которого идёт счётчик
    if (io.progress != nullptr) {
      io.progress(io.ctx, "flash", 0, 1);
    }
    const uint32_t start = io.nowMs(io.ctx);
    uint32_t first = 0;
    bool haveFirst = false;
    for (;;) {
      uint32_t info = 0;
      if (readInfo(io, &info) && (info >> 24) == kInfoNormal) {
        if (!haveFirst) {
          first = info;
          haveFirst = true;
        } else if (info != first) {
          *newInfo = info;
          break;
        }
      }
      if (io.nowMs(io.ctx) - start > kRebootMs) {
        error = "BL616 did not come back after writing the image (recover over USB: UPDATE button)";
        break;
      }
      io.sleepMs(io.ctx, 20);
    }
    if (error == nullptr && io.progress != nullptr) {
      io.progress(io.ctx, "flash", 1, 1);
    }
  }

  const uint32_t clear = 0;  // старый заголовок не должен сработать снова
  box.put(0, &clear, 1);
  return error;
}

}  // namespace bl616
}  // namespace zifi
