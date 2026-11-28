/* Стенд предела передачи FatFs (components/fatfs/diskio/diskio.c, 0.1.179) на ПК.
 *
 * Тот же diskio.c, что в прошивке, собирается MSVC вместе с поддельным диском: тот запоминает каждую
 * передачу (сектор, число секторов, адрес буфера) и отказывает, если она длиннее «буфера драйвера».
 * Так проверяется то, ради чего предел заведён: длинные чтения и записи FatFs доходят до драйвера
 * флешки кусками не длиннее его буфера, по порядку, с верными секторами и местами в буфере, а ошибка
 * куска останавливает остальные. Печатает OK или список расхождений; код возврата 0 — всё сошлось. */
#include <stdio.h>
#include <string.h>

#include "diskio_impl.h"

#define SECTOR 512u
#define DISK_SECTORS 256u

static unsigned char g_disk[DISK_SECTORS * SECTOR];   /* содержимое «флешки» */
static UINT g_buffer_sectors;      /* «буфер драйвера», секторов; передача длиннее — отказ */
static int g_fail_call = -1;       /* номер вызова, на котором диск откажет; -1 — не откажет */
static int g_calls;
static LBA_t g_sector[64];
static UINT g_count[64];
static const BYTE* g_buff[64];
static int g_failures;

static void check(int ok, const char* what) {
  if (!ok) {
    ++g_failures;
    printf("FAIL: %s\n", what);
  }
}

static void reset_log(void) {
  g_calls = 0;
  g_fail_call = -1;
}

static DSTATUS fake_init(BYTE pdrv) { (void)pdrv; return 0; }
static DSTATUS fake_status(BYTE pdrv) { (void)pdrv; return 0; }
static DRESULT fake_ioctl(BYTE pdrv, BYTE cmd, void* buff) { (void)pdrv; (void)cmd; (void)buff; return RES_OK; }

static DRESULT note(LBA_t sector, UINT count, const BYTE* buff) {
  const int call = g_calls;
  if (g_calls < 64) {
    g_sector[g_calls] = sector;
    g_count[g_calls] = count;
    g_buff[g_calls] = buff;
  }
  ++g_calls;
  if (call == g_fail_call) {
    return RES_ERROR;
  }
  if (g_buffer_sectors != 0 && count > g_buffer_sectors) {
    return RES_PARERR;   /* настоящий драйвер тут растил бы буфер — этого и нельзя допустить */
  }
  if (sector + count > DISK_SECTORS) {
    return RES_PARERR;
  }
  return RES_OK;
}

static DRESULT fake_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
  (void)pdrv;
  const DRESULT res = note(sector, count, buff);
  if (res == RES_OK) {
    memcpy(buff, g_disk + (size_t)sector * SECTOR, (size_t)count * SECTOR);
  }
  return res;
}

static DRESULT fake_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
  (void)pdrv;
  const DRESULT res = note(sector, count, buff);
  if (res == RES_OK) {
    memcpy(g_disk + (size_t)sector * SECTOR, buff, (size_t)count * SECTOR);
  }
  return res;
}

static const ff_diskio_impl_t kFake = {fake_init, fake_status, fake_read, fake_write, fake_ioctl};

int main(void) {
  static BYTE data[64 * SECTOR];
  static BYTE back[64 * SECTOR];
  const BYTE pdrv = 1;
  for (size_t i = 0; i < sizeof g_disk; ++i) {
    g_disk[i] = (unsigned char)(i * 7u + (i >> 9));
  }
  ff_diskio_register(pdrv, &kFake);

  /* Без предела — одна передача, как в исходном FatFs (так у дисков, где предел не задан) */
  reset_log();
  check(ff_disk_read(pdrv, back, 10, 20) == RES_OK && g_calls == 1 && g_count[0] == 20,
        "no limit: one transfer");

  /* Предел 8 секторов (буфер драйвера 4 КБ): 20 секторов = 8 + 8 + 4 */
  g_buffer_sectors = 8;
  ff_diskio_set_max_transfer(pdrv, 8, SECTOR);
  reset_log();
  memset(back, 0, sizeof back);
  check(ff_disk_read(pdrv, back, 10, 20) == RES_OK, "read 20 sectors with limit 8");
  check(g_calls == 3 && g_count[0] == 8 && g_count[1] == 8 && g_count[2] == 4, "read split 8 + 8 + 4");
  check(g_sector[0] == 10 && g_sector[1] == 18 && g_sector[2] == 26, "read: sectors follow each other");
  check(g_buff[0] == back && g_buff[1] == back + 8 * SECTOR && g_buff[2] == back + 16 * SECTOR,
        "read: each piece lands at its place in the buffer");
  check(memcmp(back, g_disk + 10 * SECTOR, 20 * SECTOR) == 0, "read: data as on the disk");

  /* Короче предела и ровно предел — одной передачей */
  reset_log();
  check(ff_disk_read(pdrv, back, 0, 8) == RES_OK && g_calls == 1, "exactly the limit: one transfer");
  reset_log();
  check(ff_disk_read(pdrv, back, 0, 1) == RES_OK && g_calls == 1, "one sector: one transfer");

  /* Запись делится так же, и на диске — ровно записанное */
  for (size_t i = 0; i < sizeof data; ++i) {
    data[i] = (BYTE)(0xA5 ^ i ^ (i >> 8));
  }
  reset_log();
  check(ff_disk_write(pdrv, data, 100, 17) == RES_OK, "write 17 sectors with limit 8");
  check(g_calls == 3 && g_count[0] == 8 && g_count[1] == 8 && g_count[2] == 1, "write split 8 + 8 + 1");
  check(g_sector[0] == 100 && g_sector[1] == 108 && g_sector[2] == 116, "write: sectors follow each other");
  check(memcmp(g_disk + 100 * SECTOR, data, 17 * SECTOR) == 0, "write: data on the disk as sent");

  /* Ошибка куска — итог ошибка, следующие куски не идут */
  reset_log();
  g_fail_call = 1;
  check(ff_disk_read(pdrv, back, 0, 30) == RES_ERROR && g_calls == 2, "read: error stops the rest");
  reset_log();
  g_fail_call = 0;
  check(ff_disk_write(pdrv, data, 0, 30) == RES_ERROR && g_calls == 1, "write: error stops the rest");

  /* Буфер вырос (шара SMB, 16 КБ = 32 сектора): предел — новый */
  g_buffer_sectors = 32;
  ff_diskio_set_max_transfer(pdrv, 32, SECTOR);
  reset_log();
  check(ff_disk_read(pdrv, back, 0, 64) == RES_OK && g_calls == 2 && g_count[0] == 32, "limit follows the buffer");

  /* Сектор 4096 байт (флешки 4Kn): буфер 4 КБ — по одному сектору, шаг в буфере — 4096 */
  g_buffer_sectors = 0;
  ff_diskio_set_max_transfer(pdrv, 1, 4096);
  reset_log();
  check(ff_disk_read(pdrv, back, 0, 3) == RES_OK && g_calls == 3 && g_buff[1] == back + 4096 &&
            g_buff[2] == back + 8192,
        "4096-byte sectors: step in the buffer is the sector size");

  /* Длина сектора 0 — предел не задан (как без вызова) */
  ff_diskio_set_max_transfer(pdrv, 8, 0);
  reset_log();
  check(ff_disk_read(pdrv, back, 0, 20) == RES_OK && g_calls == 1, "sector size 0: no limit");

  /* Новая регистрация диска на этом номере сбрасывает предел */
  ff_diskio_set_max_transfer(pdrv, 8, SECTOR);
  ff_diskio_register(pdrv, &kFake);
  reset_log();
  check(ff_disk_read(pdrv, back, 0, 20) == RES_OK && g_calls == 1, "register resets the limit");

  /* Предел одного диска не трогает другой */
  ff_diskio_register(0, &kFake);
  ff_diskio_set_max_transfer(pdrv, 8, SECTOR);
  reset_log();
  check(ff_disk_read(0, back, 0, 20) == RES_OK && g_calls == 1, "another drive keeps no limit");

  if (g_failures == 0) {
    printf("OK\n");
    return 0;
  }
  return 1;
}
