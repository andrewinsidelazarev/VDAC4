#pragma once

#include <stddef.h>
#include <stdint.h>

#include "zifi/bl616_push.hpp"

namespace zifi {
namespace core_cache {

// Кэш ядер ПЛИС во flash BL616 (прошивка BL616 14+, bl616/app/core_cache.c; docs/next-core.md):
// ядра, которых нет во flash Tang (ZX Next и другие), ESP кладёт туда через почтовый ящик ядра
// TS-Config — тот же, что у обновления прошивки BL616 (bl616_push), с меткой команд 0xB7
// (раскладка — bl616/app/core_mailbox.h). BL616 по команде LOAD грузит ядро в SRAM ПЛИС по JTAG.
// Ответы — CP_INFO {0xC2, состояние, номер команды}. Их совместную работу проверяет
// bl616/app/test/cache_test.cpp. Код переносимый: LINK и время — через bl616::PushIo.
//
// Имя ядра — до 15 знаков латиницы, цифр и «_-.», хранится заглавными.

// Источник образа: кусок [offset, offset + size).
struct ImageReader {
  void* ctx;
  bool (*read)(void* ctx, uint32_t offset, uint8_t* data, uint32_t size);
};

enum class Found : uint8_t { kNone, kStale, kFound };

// Проверка образа до передачи — то же, что проверяет BL616: head — первые 128 байт
// (синхрослово A5C3, IDCODE GW2AR-18, адрес перехода multiboot 0), tail — последние 64
// (UserCode 56 44 34 xx — метка VDAC4). nullptr — годится; *usercode — UserCode.
const char* checkImage(const uint8_t* head, size_t headSize, const uint8_t* tail, size_t tailSize,
                       uint32_t* usercode);

// Имя ядра для кэша: false — не годится.
bool normalizeName(const char* name, char out[16]);

class Client {
 public:
  Client(const bl616::PushIo& io, uint32_t seed) : io_(io), seq_(seed & 0xFFFF) {}

  // Объём flash BL616, МБ (0 — кэша нет; 0xFF — на месте кэша чьи-то данные, нужен FORMAT).
  const char* info(uint32_t* flashMb);
  const char* find(const char* name, const uint8_t sha[32], Found* found);
  // Записать ядро: size байтов из reader; SHA-256 — sha. Прогресс — через io.progress.
  const char* put(const char* name, uint32_t size, const uint8_t sha[32], const ImageReader& reader);
  // Загрузить ядро в ПЛИС. nullptr — BL616 принял (LOADING): через 50 мс ПЛИС стирается, затем
  // работает новое ядро (видно по VERSION LINK) или — если не вышло — снова слот 0.
  // sha == nullptr — любое ядро с этим именем.
  const char* load(const char* name, const uint8_t* sha);
  // Пустой каталог (кэш на месте чужих данных) — только по слову пользователя.
  const char* format();
  // Номер последней команды: ответ BL616 о ней в CP_INFO — {0xC2, состояние, номер}.
  uint32_t lastSeq() const { return seq_; }

 private:
  const char* command(uint32_t cmd, const uint32_t* args, uint32_t argCount, const uint32_t* payload,
                      uint32_t payloadWords, uint32_t timeoutMs, uint32_t* status);
  bool put(uint32_t first, const uint32_t* words, uint32_t count);
  bool readInfo(uint32_t* value);

  const bl616::PushIo& io_;
  uint32_t seq_;
  uint32_t page_ = 0xFFFFFFFF;
};

}  // namespace core_cache
}  // namespace zifi
