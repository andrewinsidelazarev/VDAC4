#pragma once

#include <stddef.h>
#include <stdint.h>

namespace zifi {

// Опись образа ESP на GitHub (firmware.sha256) и сравнение номеров версий.
// Отдельно от OnlineUpdater: этот код нужен и обновлятору VDAC4 (vdac4_updater),
// который собирается и на стенде ПК, где нет ни Update.h, ни TLS.

constexpr size_t kOnlineUpdateVersionCapacity = 32;

struct OnlineUpdateManifest {
  uint8_t sha256[32];
  char version[kOnlineUpdateVersionCapacity];
};

// firmware.sha256: строка «VERSION <версия>» и строки sha256sum
// («<64 hex>  [*]имя»); нужна строка firmware.bin. Повтор VERSION или SHA
// firmware.bin, недопустимый знак в версии — отказ: опись не угадывается.
bool parseOnlineManifest(const uint8_t* data, size_t length,
                         OnlineUpdateManifest& manifest);

// Отношение опубликованной версии к установленной (OnlineUpdateRelation из
// protocol.hpp). format — образец sscanf с %u.%u.%u и %n в конце
// (board::kReleaseVersionFormat): строки не по образцу — kOnlineUpdateDifferent.
uint8_t compareReleaseVersion(const char* installed, const char* published,
                              const char* format);

}  // namespace zifi
