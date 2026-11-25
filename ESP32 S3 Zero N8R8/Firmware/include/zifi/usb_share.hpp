// usb_share.hpp — шара SMB «USB»: флешка в разъёме USB-C ESP32 (0.1.171, 25.11.2026).
//
// Пользователь: «Нужно добавить в SMB сервер третью шару — USB OTG Flash ESP32. Шара должна быть
// только если флешка в USB ESP32 вставлена».
//
// Как устроено. Флешку открывает и монтирует usb_drive (том FAT32 или exFAT в /usb, FatFs ESP). Шара
// работает с ней тем же классом TangVfs, что и шара «TANG» с microSD Tang: TangVfs ходит в FatFs по
// букве диска ("0:", "1:"), а букву ему даёт «порт тома» (FatVolumePort). Порт флешки здесь.
//
// Буква диска. Флешку монтирует компонент usb_host_msc, и свою букву FatFs он наружу не отдаёт. Порт
// находит её сам: у FatFs ESP два тома (CONFIG_FATFS_VOLUME_COUNT=2) — карта Tang и флешка; та
// буква, что не у карты Tang, и есть флешка, если её размер совпадает с размером тома /usb
// (esp_vfs_fat_info). Пока порт ищет, карта Tang держится замком tang_sd::Session — её буква не
// может смениться посреди поиска. Найденная буква годна до следующего монтирования флешки
// (UsbDrive::generation).
//
// Когда шары нет: флешки нет (или она не FAT32/exFAT), WC держит её как SD2 (VDAC4 VFS, режим
// «USB Flash»: WC пишет в её файлы через свой том), буква диска не нашлась.
#pragma once

#include "zifi/board.hpp"

#if ZIFI_HAS_USB_DRIVE

#include "zifi/tang_vfs.hpp"

namespace zifi {
namespace usb_share {

// Источник моста VFS для шары «USB» (TangVfs над флешкой); один на прошивку.
TangVfs& vfs();

// Есть ли шара сейчас: флешка готова, WC её не держит, буква диска не признана ненайденной. Без
// побочных действий — проба для SMB-сервера из любой задачи.
bool shareReady();

}  // namespace usb_share
}  // namespace zifi

#endif  // ZIFI_HAS_USB_DRIVE
