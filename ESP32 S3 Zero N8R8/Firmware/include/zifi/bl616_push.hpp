#pragma once

#include <stddef.h>
#include <stdint.h>

namespace zifi {
namespace bl616 {

// Обновление прошивки BL616 модуля Tang (сопроцессор FT812) через почтовый ящик ядра Tang
// (1.8+): ESP пишет образ кусками по 1984 байта в окно LINK MBOX (MB_PAGE 0x34, 0x200…0x3FC),
// BL616 читает их, собирает образ в RAM, проверяет SHA-256 и пишет во flash с 0x40000.
// Ответы BL616 — в CP_INFO (LINK 0x30): {0xC1, состояние, номер команды}; после записи BL616
// перезагружается и новая прошивка пишет обычное {0xC0, версия, …}. Раскладка ящика — та же,
// что в bl616/app/fw_mailbox.h (их совместную работу проверяет bl616/app/test/update_test.cpp).
//
// Код переносимый: LINK, время и отчёт — через PushIo (на ESP — tang::read/write, FreeRTOS).
struct PushIo {
  void* ctx;
  bool (*read)(void* ctx, uint32_t address, uint32_t* words, size_t count);         // регистры LINK
  bool (*write)(void* ctx, uint32_t address, const uint32_t* words, size_t count);  // до 16 слов
  void (*sleepMs)(void* ctx, uint32_t ms);
  uint32_t (*nowMs)(void* ctx);
  void (*progress)(void* ctx, const char* stage, uint32_t done, uint32_t total);   // может быть пустым
};

constexpr size_t kMaxImage = 256 * 1024;

// Проверка образа до передачи: nullptr — годится (заголовок BFNP, размер 4 КБ…256 КБ).
const char* checkImage(const uint8_t* image, size_t size);

// Передать образ и дождаться новой прошивки. nullptr — записан, и BL616 уже работает с ним
// (*newInfo — его CP_INFO); иначе — что не так. seed — начальный номер команды.
const char* push(const PushIo& io, const uint8_t* image, size_t size, const uint8_t digest[32],
                 uint32_t seed, uint32_t* newInfo);

}  // namespace bl616
}  // namespace zifi
