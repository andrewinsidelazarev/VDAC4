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

}  // namespace host
}  // namespace zifi
