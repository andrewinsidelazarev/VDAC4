#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

namespace zifi {

// Кэш ресурсов музыки (банк MIDI.SF2, ПЗУ OPL4 YRW801.ROM) на флешке в разъёме USB
// модуля: файлы под своими именами 8.3 в одном каталоге (на плате —
// /usb/SYSTEM/CACHE/MUSIC). Самой музыки там нет: только то, что ядро держит в
// PSRAM как ресурс и что иначе после каждого перезапуска ESP плагин заново
// передавал бы с карты Evo.
//
// Копия годна, если её длина та же и время записи не старее файла на карте Evo.
// Время — FAT, как его шлёт плагин в MOD_QUERY: время в младших 16 битах, дата в
// старших; такие числа сравниваются как есть. Записанной копии ставится время
// файла с карты, поэтому следующая проверка находит его же. Время файла
// переводится функциями mktime/localtime_r — теми же, что у VFS FatFs, — поэтому
// поля даты и времени на флешке совпадают с полями на карте при любом поясе.
//
// Переносимый код (stdio, stat, utime): тот же класс работает в прошивке, в
// программе ESP для эмулятора Unreal (каталог на ПК вместо флешки) и в тесте.
class ResourceDiskCache {
 public:
  // directory — каталог кэша без «/» в конце; должен существовать.
  explicit ResourceDiskCache(const char* directory);

  // Что кэшируется: всё, кроме наборов волн MoonBlaster (.MWK) — они лежат в
  // папке песни, и одноимённые наборы разных песен — разные файлы.
  static bool cacheable(const char* name);

  // Есть ли годная копия ресурса с картой Evo: длина size, время FAT stamp.
  bool fresh(const char* name, uint32_t size, uint32_t stamp) const;
  // Прочитать копию целиком в data (ровно size байтов).
  bool read(const char* name, uint8_t* data, uint32_t size) const;
  // Положить копию: сначала во временный файл, потом под своё имя, со временем
  // stamp. Пропало питание посреди записи — под своим именем остаётся прежняя
  // копия или ничего, но не половина файла.
  bool write(const char* name, const uint8_t* data, uint32_t size, uint32_t stamp) const;

  // Время FAT (время | дата << 16) ↔ время файла.
  static time_t timeOf(uint32_t stamp);
  static uint32_t stampOf(time_t when);

  static constexpr const char* kTempName = "WRITING.TMP";

 private:
  void pathOf(const char* name, char* path, size_t size) const;

  char directory_[260];                  // на ПК (Unreal, тест) путь бывает длинным
};

}  // namespace zifi
