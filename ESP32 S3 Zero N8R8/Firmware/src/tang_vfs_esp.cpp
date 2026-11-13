// Шара SMB «TANG»: microSD Tang для TangVfs на ESP — замок карты, монтирование, буква FatFs.
#include "zifi/tang_vfs_esp.hpp"

#if ZIFI_HAS_SPI_LINK

#include <new>

#include "zifi/tang_link.hpp"
#include "zifi/tang_sd.hpp"

namespace zifi {
namespace tang_sd {
namespace {

// Сколько мост VFS ждёт карту, если её держит другая операция (загрузка ядра — десятки секунд).
constexpr uint32_t kVfsWaitMs = 2000;

// Том для TangVfs: каждая операция — под Session (карта Tang, ящик BL616 и смена ядра — по
// одному), карта смонтирована и не отдана WC.
class SdVolume : public FatVolumePort {
 public:
  const char* acquire() override {
    session_ = new (storage_) Session(kVfsWaitMs, true);   // серию записи CMD25 не закрывать
    if (!session_->ok()) {
      drop();
      return kBusy;
    }
    const char* problem = heldByWc() ? kHeldByWc : mount();
    if (problem != nullptr) {
      drop();
      return problem;
    }
    return nullptr;
  }

  void release() override { drop(); }

  const char* drive() const override { return fatDrive(); }

 private:
  void drop() {
    if (session_ != nullptr) {
      session_->~Session();
      session_ = nullptr;
    }
  }

  alignas(Session) unsigned char storage_[sizeof(Session)];
  Session* session_ = nullptr;
};

}  // namespace

TangVfs& vfs() {
  static SdVolume* volume = new SdVolume();
  static TangVfs* source = new TangVfs(*volume);
  return *source;
}

bool shareReady() {
  if (!tang::ready() || heldByWc()) {
    return false;
  }
  Session session(kQuickMs, true);
  return session.ok() && mount() == nullptr;
}

}  // namespace tang_sd
}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
