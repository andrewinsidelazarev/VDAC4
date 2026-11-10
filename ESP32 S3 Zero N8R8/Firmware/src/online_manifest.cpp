#include "zifi/online_manifest.hpp"

#include <stdio.h>
#include <string.h>

#include "zifi/protocol.hpp"

namespace zifi {
namespace {

int hexValue(uint8_t value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

bool filenameMatches(const uint8_t* text, size_t length) {
  constexpr char kName[] = "firmware.bin";
  return length == sizeof(kName) - 1 &&
         memcmp(text, kName, sizeof(kName) - 1) == 0;
}

bool versionCharacterAllowed(uint8_t value) {
  return (value >= 'a' && value <= 'z') ||
         (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9') || value == '.' || value == '_' ||
         value == '+' || value == '-';
}

bool parseReleaseVersion(const char* text, const char* format,
                         unsigned& major, unsigned& minor, unsigned& patch) {
  if (text == nullptr || format == nullptr) {
    return false;
  }
  int consumed = 0;
  return sscanf(text, format, &major, &minor, &patch, &consumed) == 3 &&
         text[consumed] == 0;
}

}  // namespace

// VERSION содержит показываемую пользователю версию. Строки SHA сохраняют
// обычный формат sha256sum: HASH, пробелы, необязательный '*', имя.
bool parseOnlineManifest(const uint8_t* data, size_t length,
                         OnlineUpdateManifest& manifest) {
  bool digestFound = false;
  bool versionFound = false;
  memset(&manifest, 0, sizeof(manifest));
  size_t line = 0;
  while (line < length) {
    size_t end = line;
    while (end < length && data[end] != '\r' && data[end] != '\n') {
      ++end;
    }
    size_t cursor = line;
    while (cursor < end && (data[cursor] == ' ' || data[cursor] == '\t')) {
      ++cursor;
    }
    constexpr char kVersionTag[] = "VERSION";
    if (end - cursor > sizeof(kVersionTag) - 1 &&
        memcmp(data + cursor, kVersionTag, sizeof(kVersionTag) - 1) == 0 &&
        (data[cursor + sizeof(kVersionTag) - 1] == ' ' ||
         data[cursor + sizeof(kVersionTag) - 1] == '\t')) {
      if (versionFound) {
        return false;
      }
      cursor += sizeof(kVersionTag) - 1;
      while (cursor < end &&
             (data[cursor] == ' ' || data[cursor] == '\t')) {
        ++cursor;
      }
      size_t versionEnd = end;
      while (versionEnd > cursor &&
             (data[versionEnd - 1] == ' ' || data[versionEnd - 1] == '\t')) {
        --versionEnd;
      }
      const size_t versionLength = versionEnd - cursor;
      if (versionLength == 0 ||
          versionLength >= kOnlineUpdateVersionCapacity) {
        return false;
      }
      for (size_t index = 0; index < versionLength; ++index) {
        if (!versionCharacterAllowed(data[cursor + index])) {
          return false;
        }
      }
      memcpy(manifest.version, data + cursor, versionLength);
      manifest.version[versionLength] = 0;
      versionFound = true;
    } else if (end - cursor >= 64) {
      uint8_t parsed[32];
      bool valid = true;
      for (size_t index = 0; index < sizeof(parsed); ++index) {
        const int high = hexValue(data[cursor + index * 2]);
        const int low = hexValue(data[cursor + index * 2 + 1]);
        if (high < 0 || low < 0) {
          valid = false;
          break;
        }
        parsed[index] = static_cast<uint8_t>((high << 4) | low);
      }
      cursor += 64;
      if (valid && cursor < end &&
          (data[cursor] == ' ' || data[cursor] == '\t')) {
        while (cursor < end &&
               (data[cursor] == ' ' || data[cursor] == '\t')) {
          ++cursor;
        }
        if (cursor < end && data[cursor] == '*') {
          ++cursor;
        }
        size_t nameEnd = cursor;
        while (nameEnd < end && data[nameEnd] != ' ' &&
               data[nameEnd] != '\t') {
          ++nameEnd;
        }
        if (filenameMatches(data + cursor, nameEnd - cursor)) {
          if (digestFound) {
            return false;
          }
          memcpy(manifest.sha256, parsed, sizeof(parsed));
          digestFound = true;
        }
      }
    }
    line = end;
    while (line < length && (data[line] == '\r' || data[line] == '\n')) {
      ++line;
    }
  }
  return digestFound && versionFound;
}

uint8_t compareReleaseVersion(const char* installed, const char* published,
                              const char* format) {
  if (installed != nullptr && published != nullptr &&
      strcmp(installed, published) == 0) {
    return kOnlineUpdateSame;
  }
  unsigned installedMajor = 0;
  unsigned installedMinor = 0;
  unsigned installedPatch = 0;
  unsigned publishedMajor = 0;
  unsigned publishedMinor = 0;
  unsigned publishedPatch = 0;
  if (!parseReleaseVersion(installed, format, installedMajor, installedMinor,
                           installedPatch) ||
      !parseReleaseVersion(published, format, publishedMajor, publishedMinor,
                           publishedPatch)) {
    return kOnlineUpdateDifferent;
  }
  if (publishedMajor != installedMajor) {
    return publishedMajor > installedMajor ? kOnlineUpdateNewer
                                            : kOnlineUpdateOlder;
  }
  if (publishedMinor != installedMinor) {
    return publishedMinor > installedMinor ? kOnlineUpdateNewer
                                            : kOnlineUpdateOlder;
  }
  if (publishedPatch != installedPatch) {
    return publishedPatch > installedPatch ? kOnlineUpdateNewer
                                            : kOnlineUpdateOlder;
  }
  return kOnlineUpdateDifferent;
}

}  // namespace zifi
