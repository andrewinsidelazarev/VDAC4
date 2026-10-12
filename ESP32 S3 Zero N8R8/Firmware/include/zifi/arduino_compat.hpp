#pragma once

#include <stdint.h>

// Различия Arduino-ядер ESP32, от которых зависит поведение прошивки.
// Ядро 2.x (PlatformIO, плата-переходник ZiFi) и ядро 3.x (ESP-IDF 6, плата VDAC4)
// собирают одни и те же исходники.

namespace zifi {

// Тайм-аут обмена по сокету, в секундах.
// Ядро 2.x: WiFiClient::setTimeout(секунды) сам выставляет SO_RCVTIMEO и SO_SNDTIMEO.
// Ядро 3.x: у NetworkClient остался только Stream::setTimeout(миллисекунды), и это же
// значение уходит в опции сокета. Число 8 без пересчёта означало бы 8 мс вместо 8 с.
template <typename Client>
inline void setSocketTimeoutSeconds(Client& client, uint32_t seconds) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  client.setTimeout(seconds * 1000UL);
#else
  client.setTimeout(seconds);
#endif
}

}  // namespace zifi
