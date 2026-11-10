#include "zifi/online_updater.hpp"

#include <Arduino.h>
#include <Update.h>

#include <stdio.h>
#include <string.h>

#include "zifi/board.hpp"
#include "zifi/ota_image.hpp"
#include "zifi/sha256.hpp"

namespace zifi {
namespace {

constexpr char kGithubHost[] = "raw.githubusercontent.com";
// У каждой платы свой образ: каталог публикации задан в board.hpp.
constexpr char kManifestPath[] = ZIFI_UPDATE_DIRECTORY "firmware.sha256";
constexpr char kFirmwarePath[] = ZIFI_UPDATE_DIRECTORY "firmware.bin";
constexpr uint16_t kHttpsPort = 443;

void setError(char* error, size_t errorSize, const char* text) {
  if (error != nullptr && errorSize != 0) {
    snprintf(error, errorSize, "%s", text != nullptr ? text : "error");
  }
}

void setHttpError(char* error, size_t errorSize, const char* prefix,
                  uint16_t status) {
  if (error != nullptr && errorSize != 0) {
    snprintf(error, errorSize, "%s http %u", prefix,
             static_cast<unsigned>(status));
  }
}

bool notify(OnlineUpdateProgress progress, void* context, uint8_t stage,
            uint8_t percent) {
  return progress == nullptr || progress(context, stage, percent);
}

}  // namespace

OnlineUpdater::OnlineUpdater(NetClient& client) : client_(client) {}

bool OnlineUpdater::loadManifest(uint8_t* scratch, size_t scratchSize,
                                 OnlineUpdateManifest& manifest,
                                 OnlineUpdateProgress progress,
                                 void* progressContext, char* error,
                                 size_t errorSize) {
  notify(progress, progressContext, kOnlineUpdateManifest, 0);
  uint16_t status = 0;
  uint32_t contentLength = 0;
  if (!client_.httpGet(kGithubHost, kHttpsPort, kManifestPath, status,
                       contentLength, error, errorSize)) {
    return false;
  }
  if (status < 200 || status >= 300) {
    client_.close();
    setHttpError(error, errorSize, "manifest", status);
    return false;
  }
  if (contentLength == 0 || contentLength >= scratchSize) {
    client_.close();
    setError(error, errorSize, "manifest length");
    return false;
  }

  size_t total = 0;
  bool eof = false;
  while (total < contentLength) {
    size_t received = 0;
    if (!client_.receive(scratch + total, scratchSize - total - 1, received,
                         eof, error, errorSize)) {
      client_.close();
      return false;
    }
    total += received;
    if (total > contentLength || total >= scratchSize) {
      client_.close();
      setError(error, errorSize, "manifest overflow");
      return false;
    }
    if (eof && total < contentLength) {
      client_.close();
      setError(error, errorSize, "manifest short");
      return false;
    }
    if (received == 0) {
      delay(2);
    }
  }
  client_.close();
  scratch[total] = 0;
  if (total != contentLength) {
    setError(error, errorSize, "manifest short");
    return false;
  }
  if (!parseOnlineManifest(scratch, total, manifest)) {
    setError(error, errorSize, "manifest version or sha");
    return false;
  }
  notify(progress, progressContext, kOnlineUpdateManifest, 100);
  return true;
}

bool OnlineUpdater::check(uint8_t* scratch, size_t scratchSize,
                          OnlineUpdateManifest& manifest,
                          OnlineUpdateProgress progress,
                          void* progressContext, char* error,
                          size_t errorSize) {
  if (scratch == nullptr || scratchSize < 128) {
    setError(error, errorSize, "check arguments");
    return false;
  }
  return loadManifest(scratch, scratchSize, manifest, progress,
                      progressContext, error, errorSize);
}

bool OnlineUpdater::installImage(const char* firmwarePath,
                                 uint8_t* scratch, size_t scratchSize,
                                 const uint8_t expectedSha[32],
                                 uint8_t digest[32],
                                 OnlineUpdateProgress progress,
                                 void* progressContext, char* error,
                                 size_t errorSize) {
  notify(progress, progressContext, kOnlineUpdateFirmware, 0);
  uint16_t status = 0;
  uint32_t contentLength = 0;
  if (!client_.httpGet(kGithubHost, kHttpsPort, firmwarePath, status,
                       contentLength, error, errorSize)) {
    return false;
  }
  if (status < 200 || status >= 300) {
    client_.close();
    setHttpError(error, errorSize, "firmware", status);
    return false;
  }
  if (contentLength < kOtaImageHeaderSize ||
      contentLength > ESP.getFreeSketchSpace()) {
    client_.close();
    setError(error, errorSize, "firmware does not fit");
    return false;
  }
  if (scratch == nullptr || scratchSize < kOtaImageHeaderSize) {
    client_.close();
    setError(error, errorSize, "update buffer");
    return false;
  }

  size_t headerLength = 0;
  bool eof = false;
  while (headerLength < kOtaImageHeaderSize) {
    size_t received = 0;
    if (!client_.receive(scratch + headerLength,
                         kOtaImageHeaderSize - headerLength, received, eof,
                         error, errorSize)) {
      client_.close();
      return false;
    }
    headerLength += received;
    if (eof || (received == 0 && headerLength < kOtaImageHeaderSize)) {
      if (!eof) {
        delay(2);
        continue;
      }
      setError(error, errorSize, "firmware short header");
      return false;
    }
  }
  // До проверки application-заголовка flash вообще не открывается на запись.
  if (!isEsp32S3ApplicationImage(scratch, headerLength)) {
    client_.close();
    setError(error, errorSize, "not ESP32-S3 application");
    return false;
  }

  Sha256 sha;
  if (!sha.begin()) {
    client_.close();
    setError(error, errorSize, "sha256 init");
    return false;
  }
  if (!Update.begin(contentLength, U_FLASH)) {
    client_.close();
    setError(error, errorSize, "update begin");
    return false;
  }
  uint32_t total = static_cast<uint32_t>(headerLength);
  if (Update.write(scratch, headerLength) != headerLength ||
      !sha.update(scratch, headerLength)) {
    Update.abort();
    client_.close();
    setError(error, errorSize, "first flash block");
    return false;
  }

  uint8_t lastPercent = 0;
  while (total < contentLength) {
    size_t received = 0;
    eof = false;
    const size_t remaining = static_cast<size_t>(contentLength - total);
    const size_t wanted = remaining < scratchSize ? remaining : scratchSize;
    if (!client_.receive(scratch, wanted, received, eof, error, errorSize)) {
      Update.abort();
      client_.close();
      return false;
    }
    if (received != 0) {
      if (Update.write(scratch, received) != received ||
          !sha.update(scratch, received)) {
        Update.abort();
        client_.close();
        setError(error, errorSize, "flash write");
        return false;
      }
      total += static_cast<uint32_t>(received);
      const uint8_t percent = static_cast<uint8_t>(
          (static_cast<uint64_t>(total) * 100U) / contentLength);
      if (percent == 100 || percent >= static_cast<uint8_t>(lastPercent + 5)) {
        lastPercent = percent;
        notify(progress, progressContext, kOnlineUpdateFirmware, percent);
      }
    } else if (eof) {
      Update.abort();
      setError(error, errorSize, "firmware short body");
      return false;
    } else {
      delay(2);
    }
  }
  client_.close();

  notify(progress, progressContext, kOnlineUpdateVerify, 0);
  if (!sha.finish(digest)) {
    Update.abort();
    setError(error, errorSize, "sha256 finish");
    return false;
  }
  uint8_t difference = 0;
  for (size_t index = 0; index < 32; ++index) {
    difference |= digest[index] ^ expectedSha[index];
  }
  if (difference != 0) {
    Update.abort();
    setError(error, errorSize, "sha256 mismatch");
    return false;
  }
  if (!Update.end(false)) {
    Update.abort();
    setError(error, errorSize, "update end");
    return false;
  }
  notify(progress, progressContext, kOnlineUpdateVerify, 100);
  setError(error, errorSize, "");
  return true;
}

bool OnlineUpdater::install(uint8_t* scratch, size_t scratchSize,
                            uint8_t digest[32],
                            OnlineUpdateProgress progress,
                            void* progressContext, char* error,
                            size_t errorSize) {
  if (scratch == nullptr || scratchSize < 128 || digest == nullptr) {
    setError(error, errorSize, "update arguments");
    return false;
  }
  OnlineUpdateManifest manifest{};
  if (!loadManifest(scratch, scratchSize, manifest, progress,
                    progressContext, error, errorSize)) {
    return false;
  }
  return installImage(kFirmwarePath, scratch, scratchSize, manifest.sha256,
                      digest, progress, progressContext, error,
                      errorSize);
}

}  // namespace zifi
