#pragma once

#include <stddef.h>
#include <stdint.h>

namespace zifi {

// Сколько первых байтов образа нужно для проверки. На плате VDAC4 проверяется
// ещё имя проекта в esp_app_desc_t (32 байта со смещения 80).
#if defined(ZIFI_BOARD_VDAC4) && ZIFI_BOARD_VDAC4
constexpr size_t kOtaImageHeaderSize = 112;
#else
constexpr size_t kOtaImageHeaderSize = 36;
#endif

// Проверяет заголовок именно application-образа ESP32-S3. Одного байта E9
// недостаточно: объединённый factory-образ тоже начинается с него, но не имеет
// esp_app_desc_t сразу после первого заголовка сегмента.
// На плате VDAC4 образ должен быть собран для неё же: прошивка переходника ZiFi
// там запустилась бы, но связи с Evo у неё нет (UART вместо SPI), и вернуть
// прежнюю можно было бы только через USB.
bool isEsp32S3ApplicationImage(const uint8_t* data, size_t length);

}  // namespace zifi
