#pragma once

// microSD Tang на стенде ПК: настоящая FatFs прошивки (components/fatfs) над образом диска в
// памяти. Им SMB-симулятор отдаёт шару «TANG» через тот же TangVfs, что на плате.

#include <cstdint>
#include <string>

#include "zifi/tang_vfs.hpp"

namespace zifi {
namespace host {

// Образ megabytes МБ: FAT32, кластер 512 байт; содержимое — копия папки folder (пусто — пустая
// карта). false — причина в error.
bool createTangCard(const std::string& folder, uint32_t megabytes, std::string& error);
// Том для TangVfs (буква "0:").
FatVolumePort& tangVolume();
// «Карта вставлена»: так стенд проверяет пропадание шары.
void setTangCardPresent(bool present);
bool tangCardPresent();

// Флешка в USB-C ESP32 (шара «USB», 0.1.171): образ megabytes МБ, exFAT, кластер 4 КБ (как
// у пользователя на плате — exFAT), содержимое — копия папки folder. Том — буква "1:".
bool createUsbFlash(const std::string& folder, uint32_t megabytes, std::string& error);
FatVolumePort& usbVolume();
// «Флешка вставлена»: так стенд проверяет пропадание шары USB.
void setUsbFlashPresent(bool present);
bool usbFlashPresent();

}  // namespace host
}  // namespace zifi
