#pragma once
// Видео с SD2 WC (VDAC4 VFS, прошивка 0.1.126): файл по первому кластеру и длине из записи каталога
// WC — их знает плеер FTVIEW4 (WC: #38A8 — первый кластер файла). Что сейчас на SD2, знает служба
// vdisk_service:
//   режим 1 — сама карта Tang (WC держит её посекторно): цепочка кластеров — по FAT32 карты
//     (Fat32Map поверх tang_sd::rawRead), поток — мостом ядра видео 1.47 прямо в медиа-FIFO
//     (tang_sd::rawStream: CMD18, данные мимо ESP);
//   режим 0 — том VFS: кластер тома — файл источника (флешка USB, карта Tang через FatFs), байты
//     читает ESP (vdisk::Source::read), в ядро их пишет приложение (окно MF_DATA).
#include <stdint.h>

#include "zifi/board.hpp"

namespace zifi {
namespace sd2_files {

struct File;

// Открыть — с ядра 1 (команда приложения: состояние vdisk_service живёт там). 0 — открыт; 1 — на SD2
// такого файла нет (SD2 не VDAC4 VFS, том собран заново, кластер — не начало файла); 2 — отказ.
// Цепочку кластеров карты open не читает — её строит первое read или stream (задача подачи).
int open(uint32_t cluster, uint32_t size, File** file);
bool raw(const File* file);              // сама карта Tang: годится stream
uint32_t size(const File* file);

// Байты файла с offset (из любой задачи): true — *got (меньше n только у конца файла)
bool read(File* file, uint32_t offset, uint8_t* out, uint32_t n, uint32_t* got);
// Только raw: n байтов с offset — целыми секторами мостом ядра видео 1.47+ прямо в медиа-FIFO. offset
// и n кратны 512 (начало и хвост порции не на границе сектора приложение читает read и пишет в окно
// MF_DATA само), offset + n — не дальше секторов файла. *crcErrors — порций (операций моста), где
// CRC16 блока не сошёлся. 0 — да; 1 — не годится (не карта Tang, ядро без потока, выравнивание);
// 2 — отказ карты или цепочки (сколько ушло — не известно: поток начинать заново).
int stream(File* file, uint32_t offset, uint32_t n, uint32_t* crcErrors);
// Что не так с цепочкой карты (после отказа read/stream), иначе nullptr
const char* problem(const File* file);
void close(File* file);

}  // namespace sd2_files
}  // namespace zifi
