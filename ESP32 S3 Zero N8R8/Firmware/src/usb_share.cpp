// Шара SMB «USB»: флешка в USB-C ESP32 для TangVfs — проверка флешки, буква диска FatFs (0.1.171).
// Как и зачем — в include/zifi/usb_share.hpp.
#include "zifi/usb_share.hpp"

#if ZIFI_HAS_USB_DRIVE

#include <atomic>
#include <stdio.h>
#include <string.h>

#include <esp_vfs_fat.h>

#include "ff.h"
#include "zifi/bringup_log.hpp"
#include "zifi/usb_drive.hpp"
#include "zifi/vdisk_service.hpp"
#if ZIFI_HAS_SPI_LINK
#include "zifi/tang_sd.hpp"
#endif

namespace zifi {
namespace usb_share {
namespace {

constexpr char kTag[] = "zifi-usbshare";
constexpr const char* kNoDrive = "no USB drive";
constexpr const char* kHeldByWc = "the USB drive is SD2 in WC (VDAC4 VFS)";
constexpr const char* kNoLetter = "FatFs drive of the USB drive not found";
#if ZIFI_HAS_SPI_LINK
// Сколько ждать карту Tang, если её держит другая операция (поиск буквы — раз на монтирование).
constexpr uint32_t kTangWaitMs = 2000;
#endif

// Номер монтирования, на котором букву найти не удалось (0 — такого не было): шара до следующего
// монтирования прячется. atomic: пишет ядро 1 (мост VFS), читает проба из задачи SMB.
std::atomic<uint32_t> g_noLetterGeneration{0};

// Том флешки для TangVfs. Все вызовы — с ядра 1 (мост VFS): там же usb_drive отмонтирует вынутую
// флешку, так что посреди операции том не пропадёт.
class UsbVolume : public FatVolumePort {
 public:
  const char* acquire() override {
    if (!usbDrive().ready()) {
      return kNoDrive;
    }
    if (vdisk_service::usbHeldByWc()) {
      return kHeldByWc;
    }
    return findLetter();
  }

  void release() override {}

  const char* drive() const override { return letter_; }

 private:
  // Буква диска флешки для текущего монтирования: nullptr — найдена (в letter_), иначе причина.
  const char* findLetter() {
    const uint32_t generation = usbDrive().generation();
    if (generation_ == generation && letter_[0] != 0) {
      return nullptr;
    }
    letter_[0] = 0;
    generation_ = 0;
    // Размер тома /usb по данным VFS — тот, кого ищем. Первый вызов на exFAT считает свободное
    // место по битовой карте флешки (доли секунды), дальше FatFs помнит его сам.
    uint64_t total = 0;
    uint64_t freeBytes = 0;
    if (esp_vfs_fat_info(UsbDrive::kMountPoint, &total, &freeBytes) != ESP_OK || total == 0) {
      return kNoDrive;
    }
#if ZIFI_HAS_SPI_LINK
    // Буква карты Tang не должна смениться, пока ищем: её монтирование и снятие — под тем же замком.
    tang_sd::Session session(kTangWaitMs);
    if (!session.ok()) {
      return tang_sd::kBusy;
    }
    const char* tang = tang_sd::fatDrive();   // "" — карта не смонтирована
#else
    const char* tang = "";
#endif
    for (int pdrv = 0; pdrv < FF_VOLUMES; ++pdrv) {
      char candidate[4] = {static_cast<char>('0' + pdrv), ':', 0, 0};
      if (strcmp(candidate, tang) == 0) {
        continue;                        // том карты Tang не трогать: он её, а не флешки
      }
      char root[5];
      snprintf(root, sizeof(root), "%s/", candidate);
      DWORD freeClusters = 0;
      FATFS* fs = nullptr;
      if (f_getfree(root, &freeClusters, &fs) != FR_OK || fs == nullptr) {
        continue;                        // буква пуста (никем не смонтирована)
      }
#if FF_MAX_SS != FF_MIN_SS
      const uint64_t sectorBytes = fs->ssize;
#else
      const uint64_t sectorBytes = FF_MAX_SS;
#endif
      // тот же расчёт, что у esp_vfs_fat_info: (кластеров) × (секторов в кластере) × (байт в секторе)
      if (static_cast<uint64_t>(fs->n_fatent - 2) * fs->csize * sectorBytes == total) {
        memcpy(letter_, candidate, sizeof(letter_));
        generation_ = generation;
        bringupLog(kTag, "USB share: FatFs drive %s, %u MB", letter_,
                   static_cast<unsigned>(total / (1024 * 1024)));
        return nullptr;
      }
    }
    g_noLetterGeneration = generation;
    bringupLog(kTag, "USB share: %s (/usb %u MB)", kNoLetter, static_cast<unsigned>(total / (1024 * 1024)));
    return kNoLetter;
  }

  char letter_[4] = {};
  uint32_t generation_ = 0;   // монтирование, для которого letter_ найдена (0 — не найдена)
};

}  // namespace

TangVfs& vfs() {
  static UsbVolume* volume = new UsbVolume();
  static TangVfs* source = new TangVfs(*volume, TangVfs::kUsbSerial, "USB");
  return *source;
}

bool shareReady() {
  const UsbDrive& drive = usbDrive();
  return drive.isReady() && !vdisk_service::usbHeldByWc() &&
         g_noLetterGeneration.load() != drive.generation();
}

}  // namespace usb_share
}  // namespace zifi

#endif  // ZIFI_HAS_USB_DRIVE
