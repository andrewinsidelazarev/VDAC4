// Служба виртуального диска VDAC4 VFS — см. zifi/vdisk_service.hpp
#include "zifi/vdisk_service.hpp"

#include <string.h>

#include "zifi/board.hpp"
#include "zifi/vdisk.hpp"
#include "zifi/vdisk_sources.hpp"
#if ZIFI_HAS_SPI_LINK
#include "zifi/tang_sd.hpp"
#endif

// Плата VDAC4 — с каналом к Tang: без источника TANG том пуст, а INFO отвечает «не готов»
#if defined(ZIFI_BOARD_VDAC4) && ZIFI_BOARD_VDAC4 && !ZIFI_HAS_SPI_LINK
#error "VDAC4 VFS: на плате VDAC4 нет ZIFI_HAS_SPI_LINK — источник TANG не соберётся"
#endif

namespace zifi {
namespace vdisk_service {

namespace {

void put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
uint32_t get32(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

struct State {
  vdisk::Volume volume;
#if ZIFI_HAS_SPI_LINK
  vdisk_sources::TangSource tang;
#endif
  bool configured = false;
  bool raw = false;              // режим 1: SD2 — сама карта Tang
  uint32_t lba = UINT32_MAX;     // собранный сектор
  uint8_t sector[vdisk::kSectorSize];
  void setup() {
    if (configured) return;
    vdisk::Source* sources[] = {
#if ZIFI_HAS_SPI_LINK
        &tang,
#endif
        nullptr};
    volume.configure(sources, sizeof sources / sizeof sources[0] - 1);
    configured = true;
  }
  uint8_t flags() const { return raw ? (kFlagReady | kFlagRaw) : volume.flags(); }
  uint32_t sectors() const {
#if ZIFI_HAS_SPI_LINK
    if (raw) return tang_sd::rawSectors();
#endif
    return volume.sectorCount();
  }
  bool readSector(uint32_t at, uint8_t* out) {
#if ZIFI_HAS_SPI_LINK
    if (raw) return tang_sd::rawRead(at, out);
#endif
    return volume.read(at, out);
  }
  // Режим 0 после 1: карта могла измениться — дерево тома собрать заново
  void leaveRaw() {
#if ZIFI_HAS_SPI_LINK
    tang_sd::releaseFromWc();
#endif
    raw = false;
    volume.reset();
    lba = UINT32_MAX;
  }
};

State& state() {
  static State* s = new State();   // в PSRAM-куче: том держит дерево узлов и окна файлов
  return *s;
}

}  // namespace

void handle(const PacketView& packet, Z80Transport& transport) {
  State& s = state();
  s.setup();
  uint8_t reply[1 + kReadChunk];
  switch (packet.command) {
    case kVdiskInfo: {
      reply[0] = kOk;
      reply[1] = s.flags();
      put32(reply + 2, s.volume.generation());
      put32(reply + 6, s.sectors());
      reply[10] = static_cast<uint8_t>(s.volume.sourceCount());
      transport.send(kRespVdiskInfo, reply, 11);
      break;
    }
    case kVdiskRead: {
      uint16_t length = 1;
      reply[0] = kBadRequest;
      // 8-й байт — режим, в котором драйвер ждёт сектор (SD2 = карта Tang шлёт 1): ESP после
      // перезагрузки — снова в режиме 0, и WC получит отказ, а не сектор чужого тома
      if ((packet.length == 8 && packet.data[7] != (s.raw ? 1 : 0))) {
        reply[0] = kWrongMode;
      } else if (packet.length == 7 || packet.length == 8) {
        const uint32_t lba = get32(packet.data);
        const uint16_t offset = (uint16_t)(packet.data[4] | packet.data[5] << 8);
        const uint16_t count = packet.data[6];
        if (count && count <= kReadChunk && offset + count <= vdisk::kSectorSize) {
          if (s.lba != lba || offset == 0) {   // первая часть — собрать заново
            s.lba = UINT32_MAX;
            if (s.readSector(lba, s.sector)) s.lba = lba;
          }
          if (s.lba == lba) {
            reply[0] = kOk;
            memcpy(reply + 1, s.sector + offset, count);
            length = (uint16_t)(1 + count);
          } else {
            reply[0] = kSourceFailed;
          }
        }
      }
      transport.send(kRespVdiskRead, reply, length);
      break;
    }
    case kVdiskWrite: {
      reply[0] = kBadRequest;
      if (packet.length == 4 + vdisk::kSectorSize) {
        const uint32_t lba = get32(packet.data);
        if (!s.raw) {
          reply[0] = kReadOnly;
        } else {
          s.lba = UINT32_MAX;                  // прежний собранный сектор мог быть этим
#if ZIFI_HAS_SPI_LINK
          reply[0] = tang_sd::rawWrite(lba, packet.data + 4) ? kOk : kSourceFailed;
#else
          reply[0] = kSourceFailed;
#endif
        }
      }
      transport.send(kRespVdiskWrite, reply, 1);
      break;
    }
    case kVdiskMode: {
      reply[0] = kBadRequest;
      if (packet.length == 1 && packet.data[0] <= 1) {
        if (packet.data[0] == 0) {
          s.leaveRaw();
          reply[0] = kOk;
        } else {
#if ZIFI_HAS_SPI_LINK
          const char* problem = tang_sd::holdForWc();
          s.raw = problem == nullptr;
          s.lba = UINT32_MAX;
          reply[0] = s.raw ? kOk : kNoCard;
#else
          reply[0] = kNoCard;
#endif
        }
      }
      reply[1] = s.flags();
      put32(reply + 2, s.sectors());
      transport.send(kRespVdiskMode, reply, 6);
      break;
    }
    case kVdiskReset: {
      if (!s.raw) s.volume.reset();
      s.lba = UINT32_MAX;
      reply[0] = kOk;
      put32(reply + 1, s.volume.generation());
      transport.send(kRespVdiskReset, reply, 5);
      break;
    }
    default:
      break;
  }
}

}  // namespace vdisk_service
}  // namespace zifi
