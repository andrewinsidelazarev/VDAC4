#include "zifi/spi_link_core.hpp"
#include "zifi/protocol.hpp"

#include <string.h>

namespace zifi {
namespace {

constexpr bool halvesAreInverse(uint8_t value) {
  return ((value >> 4) ^ (value & 0x0F)) == 0x0F;
}

// См. замечание у kRequestMarker.
static_assert(halvesAreInverse(SpiLinkCore::kRequestMarker) &&
                  halvesAreInverse(SpiLinkCore::kResponseMarker) &&
                  halvesAreInverse(SpiLinkCore::kStateEmpty) &&
                  halvesAreInverse(SpiLinkCore::kStatePending),
              "the Z80 driver finds the data start by these bytes");

}  // namespace

SpiLinkCore::SpiLinkCore()
    : fromZ80_(),
      toZ80_(),
      lastResponse_{},
      lastLength_(0),
      lastTid_(0),
      haveLast_(false),
      blockOut_(false),
      bulkGaveUp_(false),
      blockTries_(0),
      blockInline_(0),
      blockPending_(false),
      putArmed_(false),
      putMedia_(false),
      putCommand_(kAudioData),
      putPrefix_{},
      putPrefixSize_(0),
      putSize_(kBulkSize),
      putXor_(0),
      turns_(0),
      repeats_(0),
      badRequests_(0),
      shortAccepts_(0),
      bytesFromZ80_(0),
      bytesToZ80_(0),
      bulkBlocks_(0),
      bulkBad_(0),
      bulkGaveUps_(0),
      putBlocks_(0),
      putBad_(0) {}

bool SpiLinkCore::begin(uint8_t* fromZ80Storage, size_t fromZ80Capacity,
                        uint8_t* toZ80Storage, size_t toZ80Capacity) {
  haveLast_ = false;
  lastLength_ = 0;
  blockOut_ = false;
  bulkGaveUp_ = false;
  blockTries_ = 0;
  blockInline_.store(0, std::memory_order_relaxed);
  blockPending_.store(false, std::memory_order_relaxed);
  putArmed_ = false;
  return fromZ80_.attach(fromZ80Storage, fromZ80Capacity) &&
         toZ80_.attach(toZ80Storage, toZ80Capacity);
}

uint8_t SpiLinkCore::checksum(const uint8_t* data, size_t length) {
  uint8_t sum = 0;
  for (size_t index = 0; index < length; ++index) {
    sum = static_cast<uint8_t>(sum + data[index]);
  }
  // Сумма всего сообщения вместе с этим байтом равна #FF. Буфер из одних нулей
  // (ESP только что перезапустилась) верным сообщением поэтому не выглядит.
  return static_cast<uint8_t>(~sum);
}

bool SpiLinkCore::handleRequest(const uint8_t* request, uint8_t* response,
                                size_t& responseLength, BulkState bulk,
                                uint8_t* bulkBuffer, const PutInput& put) {
  responseLength = 0;
  if (request == nullptr || response == nullptr ||
      request[0] != kRequestMarker) {
    // В буфере наш же прошлый ответ либо ещё ничего: это не запрос.
    return false;
  }
  const uint8_t tid = request[1];
  const uint8_t flags = request[2];
  const size_t length = request[3];
  size_t room = request[4];
  if (length > kMaxChunk ||
      checksum(request, kHeaderSize + length) != request[kHeaderSize + length]) {
    // Запрос прочитан, пока Z80 его ещё писала, либо испорчен в линии.
    // Ответа не будет, и Z80 по тайм-ауту пришлёт его снова.
    badRequests_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (room > kMaxChunk) {
    room = kMaxChunk;
  }

  if ((flags & kFlagNewSession) == 0 && haveLast_ && tid == lastTid_) {
    memcpy(response, lastResponse_, lastLength_);
    responseLength = lastLength_;
    repeats_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  if ((flags & kFlagNewSession) != 0) {
    bulkGaveUp_ = false;
    putArmed_ = false;
  }
  if ((flags & kFlagDropToZ80) != 0) {
    toZ80_.discard(toZ80_.available());
    blockOut_ = false;
    blockInline_.store(0, std::memory_order_relaxed);
  }
  // Блок от Z80, записанный после прошлого «готова», встаёт в очередь раньше
  // данных этого запроса.
  uint8_t putStatus = put.available ? kStatusPutCapable : 0;
  if ((flags & kFlagPutDone) != 0) {
    bool good = false;
    const size_t needed = putSize_ + (putMedia_ ? 4 + putPrefixSize_ : 0);
    if (putArmed_ && put.received != nullptr &&
        put.receivedLength >= putDmaSize() && fromZ80_.freeSpace() >= needed) {
      uint8_t sum = 0;
      if (!putMedia_) {
        for (size_t index = 0; index < putSize_; ++index) {
          sum = static_cast<uint8_t>(sum ^ put.received[index]);
        }
      }
      if (putMedia_ || sum == putXor_) {
        if (putMedia_) {
          const size_t total = putSize_ + putPrefixSize_;
          const uint8_t header[] = {kMediaSync, putCommand_,
              static_cast<uint8_t>(total), static_cast<uint8_t>(total >> 8)};
          fromZ80_.write(header, sizeof(header));
          fromZ80_.write(putPrefix_, putPrefixSize_);
        }
        fromZ80_.write(put.received, putSize_);
        good = true;
      }
    }
    putArmed_ = false;
    if (good) {
      putStatus |= kStatusPutOk;
      putBlocks_.fetch_add(1, std::memory_order_relaxed);
      bytesFromZ80_.fetch_add(static_cast<uint32_t>(putSize_),
                              std::memory_order_relaxed);
    } else {
      putStatus |= kStatusPutBad;
      putBad_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  const bool media = (flags & kFlagMedia) != 0;
  const size_t accepted = media ? length :
      (length != 0 ? fromZ80_.write(request + kHeaderSize, length) : 0);
  if (media) {
    room = 0;
    const uint8_t command = length != 0 ? request[kHeaderSize] : 0;
    const bool resource = command == kModPut && length == 7;
    const bool valid = resource || (command == kAudioData && length == 3);
    const size_t size = valid ? readLe16(request + kHeaderSize + 1) : 0;
    const size_t prefix = resource ? 4 : 0;
    if (put.available && size != 0 && size <= kMediaMax &&
        ((size + 511u) & ~size_t(511u)) <= put.capacity &&
        fromZ80_.freeSpace() >= size + prefix + 4) {
      putArmed_ = true;
      putMedia_ = true;
      putCommand_ = command;
      putPrefixSize_ = prefix;
      if (resource) memcpy(putPrefix_, request + kHeaderSize + 3, prefix);
      putSize_ = size;
      putStatus |= kStatusPutReady;
    }
  }
  if (!media && (flags & kFlagPut) != 0) {
    // Байт 4 — XOR блока, а не место: данных в ответ Z80 не ждёт.
    room = 0;
    if (put.available && fromZ80_.freeSpace() >= kBulkSize) {
      putArmed_ = true;
      putMedia_ = false;
      putSize_ = kBulkSize;
      putXor_ = request[4];
      putStatus |= kStatusPutReady;
    }
  }

  const bool wantsBulk = (flags & kFlagBulk) != 0 && bulkBuffer != nullptr;
  size_t inlineLeft = blockInline_.load(std::memory_order_relaxed);
  uint8_t bulkStatus = 0;
  // Блок, отданный раньше и ещё не подтверждённый.
  if (blockOut_ && inlineLeft == 0) {
    if (bulk == kBulkEchoed) {
      blockOut_ = false;
    } else if (wantsBulk) {
      if (bulk == kBulkLoaded) {
        // Z80 его ещё не читала: он по-прежнему выставлен.
        bulkStatus = kStatusBulk;
      } else {
        bulkBad_.fetch_add(1, std::memory_order_relaxed);
        ++blockTries_;
        if (bulk == kBulkFree && blockTries_ < kBulkAttempts) {
          bulkStatus = kStatusBulk | kStatusEchoBad;
        } else {
          // DMA не работает: блок уйдёт обычными ответами, новых не будет.
          bulkGaveUp_ = true;
          bulkGaveUps_.fetch_add(1, std::memory_order_relaxed);
          inlineLeft = kBulkSize;
          bulkStatus = kStatusEchoBad;
        }
      }
    }
  }
  // Новый блок, если в очереди его набралось. Невостребованный блок прежнего
  // сеанса прежде нужно убрать из DMA.
  if (bulkStatus == 0 && wantsBulk && !blockOut_ && !bulkGaveUp_) {
    if (bulk == kBulkLoaded) {
      bulkStatus = kStatusBulk | kStatusBulkStale;
    } else if (bulk != kBulkOff && toZ80_.available() >= kBulkSize) {
      toZ80_.read(bulkBuffer, kBulkSize);
      blockOut_ = true;
      blockTries_ = 0;
      bulkStatus = kStatusBulk;
      bulkBlocks_.fetch_add(1, std::memory_order_relaxed);
      bytesToZ80_.fetch_add(static_cast<uint32_t>(kBulkSize),
                            std::memory_order_relaxed);
    }
  }
  // Данные в ответе: сначала остаток блока, от DMA которого отказались, потом
  // очередь. За неподтверждённым блоком очередь ждёт.
  size_t sent = 0;
  if (room != 0 && (bulkStatus & kStatusBulk) == 0) {
    if (inlineLeft != 0 && bulkBuffer != nullptr) {
      sent = room < inlineLeft ? room : inlineLeft;
      memcpy(response + kHeaderSize, bulkBuffer + (kBulkSize - inlineLeft), sent);
      inlineLeft -= sent;
      if (inlineLeft == 0) {
        blockOut_ = false;
      }
    } else if (!blockOut_) {
      sent = toZ80_.read(response + kHeaderSize, room);
    }
  }
  blockInline_.store(static_cast<uint32_t>(inlineLeft), std::memory_order_relaxed);

  response[0] = kResponseMarker;
  response[1] = tid;
  // Неподтверждённый блок — тоже данные для Z80: о нём она должна спросить.
  blockPending_.store(blockOut_, std::memory_order_relaxed);
  response[2] = static_cast<uint8_t>(
      (toZ80_.available() != 0 || blockOut_ ? kStatusMore : 0) | bulkStatus |
      putStatus);
  response[3] = static_cast<uint8_t>(sent);
  response[4] = static_cast<uint8_t>(accepted);
  response[kHeaderSize + sent] = checksum(response, kHeaderSize + sent);
  responseLength = kHeaderSize + sent + 1;

  memcpy(lastResponse_, response, responseLength);
  lastLength_ = responseLength;
  lastTid_ = tid;
  haveLast_ = true;

  turns_.fetch_add(1, std::memory_order_relaxed);
  if (accepted != length) {
    shortAccepts_.fetch_add(1, std::memory_order_relaxed);
  }
  bytesFromZ80_.fetch_add(static_cast<uint32_t>(media ? 0 : accepted),
                          std::memory_order_relaxed);
  bytesToZ80_.fetch_add(static_cast<uint32_t>(sent), std::memory_order_relaxed);
  return true;
}

void SpiLinkCore::stateWord(uint8_t word[kStateSize], uint8_t bootNonce) const {
  word[0] = pendingToZ80() != 0 ? kStatePending : kStateEmpty;
  word[1] = kMagic;
  word[2] = kVersion;
  word[3] = bootNonce;
}

size_t SpiLinkCore::read(uint8_t* output, size_t capacity) {
  return fromZ80_.read(output, capacity);
}

size_t SpiLinkCore::write(const uint8_t* data, size_t length) {
  return toZ80_.write(data, length);
}

size_t SpiLinkCore::writeSpace() const {
  return toZ80_.freeSpace();
}

size_t SpiLinkCore::writeCapacity() const {
  return toZ80_.capacity();
}

size_t SpiLinkCore::pendingFromZ80() const { return fromZ80_.available(); }

size_t SpiLinkCore::pendingToZ80() const {
  const size_t queued = toZ80_.available();
  if (!blockPending_.load(std::memory_order_relaxed)) {
    return queued;
  }
  // Блок вне очереди: либо весь ещё у Z80 на проверке, либо его остаток.
  const size_t left = blockInline_.load(std::memory_order_relaxed);
  return queued + (left != 0 ? left : kBulkSize);
}

uint32_t SpiLinkCore::activity() const {
  return turns_.load(std::memory_order_relaxed) +
         repeats_.load(std::memory_order_relaxed);
}

SpiLinkCore::Stats SpiLinkCore::stats() const {
  Stats result{};
  result.turns = turns_.load(std::memory_order_relaxed);
  result.repeats = repeats_.load(std::memory_order_relaxed);
  result.badRequests = badRequests_.load(std::memory_order_relaxed);
  result.shortAccepts = shortAccepts_.load(std::memory_order_relaxed);
  result.bytesFromZ80 = bytesFromZ80_.load(std::memory_order_relaxed);
  result.bytesToZ80 = bytesToZ80_.load(std::memory_order_relaxed);
  result.bulkBlocks = bulkBlocks_.load(std::memory_order_relaxed);
  result.bulkBad = bulkBad_.load(std::memory_order_relaxed);
  result.bulkGaveUp = bulkGaveUps_.load(std::memory_order_relaxed);
  result.putBlocks = putBlocks_.load(std::memory_order_relaxed);
  result.putBad = putBad_.load(std::memory_order_relaxed);
  return result;
}

}  // namespace zifi
