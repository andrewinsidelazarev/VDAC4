#pragma once

#include "zifi/board.hpp"

#if ZIFI_HAS_SPI_LINK

#include "zifi/vdac4_updater.hpp"

namespace zifi {

// Плата VDAC4 для обновлятора (vdac4_updater): ESP — раздел OTA (OnlineUpdater::installImage),
// Tang — LINK (tang_link), flash ПЛИС через мост TS-Config (tang_flash, как служба 8268 BEGIN),
// BL616 — почтовый ящик ядра (bl616_push, как служба 8268 BL616), microSD Tang — FatFs в /tangsd
// (tang_sd, как SDPUT). Зовёт её одна задача сеанса обновления (ядро 0).
class Vdac4BoardEsp : public Vdac4Board {
 public:
  const char* espVersion() override;
  const char* releaseFormat() override;
  bool espImageSha256(uint32_t size, uint8_t digest[32]) override;
  bool espInstall(const char* rawPath, const uint8_t sha256[32], VupProgress progress,
                  void* context, char* error, size_t errorSize) override;
  void espRestart() override;

  bool tangVersion(uint32_t& version) override;
  bool bl616Info(uint32_t& info) override;
  const char* flashReady() override;
  bool flashRead(uint32_t address, uint8_t* data, size_t length) override;
  bool flashWriteSlot(uint32_t base, const uint8_t* image, size_t size, VupProgress progress,
                      void* context, char* error, size_t errorSize) override;
  bool bl616Push(const uint8_t* image, size_t size, uint32_t& newInfo, char* error,
                 size_t errorSize) override;

  const char* tangSdReady() override;
  bool tangSdStat(const char* path, bool& exists, uint32_t& size) override;
  bool tangSdRead(const char* path, uint8_t* data, uint32_t size) override;
  bool tangSdWrite(const char* path, const uint8_t* data, uint32_t size, char* error,
                   size_t errorSize) override;
};

}  // namespace zifi

#endif  // ZIFI_HAS_SPI_LINK
