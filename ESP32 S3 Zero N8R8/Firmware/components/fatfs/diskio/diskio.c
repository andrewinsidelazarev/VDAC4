/*-----------------------------------------------------------------------*/
/* Low level disk I/O module skeleton for FatFs     (C)ChaN, 2016        */
/* ESP-IDF port Copyright 2016-2025 Espressif Systems (Shanghai) PTE LTD */
/*-----------------------------------------------------------------------*/
/* If a working storage control module is available, it should be        */
/* attached to the FatFs via a glue function rather than modifying it.   */
/* This is an example of glue functions to attach various existing       */
/* storage control modules to the FatFs module with a defined API.       */
/*-----------------------------------------------------------------------*/

#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <sys/time.h>
#include "diskio_impl.h"
#include "private_include/diskio_private.h"
#include "ffconf.h"
#include "ff.h"

static ff_diskio_impl_t * s_impls[FF_VOLUMES] = { NULL };

/* VDAC4 (0.1.179): предел одной передачи диска — см. ff_disk_read. 0 — без предела. */
static UINT s_max_sectors[FF_VOLUMES] = { 0 };
static UINT s_sector_bytes[FF_VOLUMES] = { 0 };

#if FF_MULTI_PARTITION		/* Multiple partition configuration */
PARTITION VolToPart[FF_VOLUMES] = {
    {0, 0},    /* Logical drive 0 ==> Physical drive 0, auto detection */
#if FF_VOLUMES > 1
    {1, 0},    /* Logical drive 1 ==> Physical drive 1, auto detection */
#endif
#if FF_VOLUMES > 2
    {2, 0},     /* Logical drive 2 ==> Physical drive 2, auto detection */
#endif
#if FF_VOLUMES > 3
    {3, 0},     /* Logical drive 3 ==> Physical drive 3, auto detection */
#endif
#if FF_VOLUMES > 4
    {4, 0},     /* Logical drive 4 ==> Physical drive 4, auto detection */
#endif
#if FF_VOLUMES > 5
    {5, 0},     /* Logical drive 5 ==> Physical drive 5, auto detection */
#endif
#if FF_VOLUMES > 6
    {6, 0},     /* Logical drive 6 ==> Physical drive 6, auto detection */
#endif
#if FF_VOLUMES > 7
    {7, 0},     /* Logical drive 7 ==> Physical drive 7, auto detection */
#endif
#if FF_VOLUMES > 8
    {8, 0},     /* Logical drive 8 ==> Physical drive 8, auto detection */
#endif
#if FF_VOLUMES > 9
    {9, 0},     /* Logical drive 9 ==> Physical drive 9, auto detection */
#endif
};
#endif

esp_err_t ff_diskio_get_drive(BYTE* out_pdrv)
{
    BYTE i;
    for(i=0; i<FF_VOLUMES; i++) {
        if (!s_impls[i]) {
            *out_pdrv = i;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

void ff_diskio_register(BYTE pdrv, const ff_diskio_impl_t* discio_impl)
{
    assert(pdrv < FF_VOLUMES);

    if (s_impls[pdrv]) {
        ff_diskio_impl_t* im = s_impls[pdrv];
        s_impls[pdrv] = NULL;
        free(im);
    }
    /* Новый (или никакой) диск на этом номере — предел прежнего ему не годится */
    s_max_sectors[pdrv] = 0;
    s_sector_bytes[pdrv] = 0;

    if (!discio_impl) {
        return;
    }

    ff_diskio_impl_t * impl = (ff_diskio_impl_t *)malloc(sizeof(ff_diskio_impl_t));
    assert(impl != NULL);
    memcpy(impl, discio_impl, sizeof(ff_diskio_impl_t));
    s_impls[pdrv] = impl;
}

DSTATUS ff_disk_initialize (BYTE pdrv)
{
    return s_impls[pdrv]->init(pdrv);
}
DSTATUS ff_disk_status (BYTE pdrv)
{
    return s_impls[pdrv]->status(pdrv);
}
void ff_diskio_set_max_transfer(BYTE pdrv, UINT max_sectors, UINT sector_bytes)
{
    assert(pdrv < FF_VOLUMES);
    s_max_sectors[pdrv] = sector_bytes != 0 ? max_sectors : 0;
    s_sector_bytes[pdrv] = sector_bytes;
}

/* VDAC4 (0.1.179). Чтение и запись — кусками не длиннее предела диска.
 *
 * Зачем. FatFs читает и пишет целые секторы файла прямо в буфер программы одной командой диска:
 * fread на 8 КБ — одна передача на 8 КБ. Драйвер флешек USB (espressif usb_host_msc, msc_bulk_transfer)
 * держит один буфер передачи во внутреннем ОЗУ и для передачи длиннее буфера сначала ОСВОБОЖДАЕТ старый,
 * потом выделяет новый. Если нового не выделить (внутреннего ОЗУ мало — так в игре Quake 2), в драйвере
 * остаётся указатель на освобождённый буфер: следующая передача пишет в чужую память или освобождает
 * буфер второй раз. 27.11.2026 так упала прошивка 0.1.177: «tlsf_free: block already marked as free»,
 * задача zm-coreload, при чтении нового ядра Q2 с флешки кусками по 8 КБ (буфер драйвера — 4 КБ).
 *
 * Как. Прошивка (src/usb_drive.cpp) задаёт предел = длине буфера драйвера. Длинное чтение здесь
 * делится на куски не длиннее — драйвер буфер не трогает никогда. Делить можно: секторы подряд, и
 * кусок за куском — то же самое, что одной командой, только команд больше. */
DRESULT ff_disk_read (BYTE pdrv, BYTE* buff, LBA_t sector, UINT count)
{
    const UINT max = s_max_sectors[pdrv];
    if (max == 0 || count <= max) {
        return s_impls[pdrv]->read(pdrv, buff, sector, count);
    }
    while (count > 0) {
        const UINT n = count < max ? count : max;
        const DRESULT res = s_impls[pdrv]->read(pdrv, buff, sector, n);
        if (res != RES_OK) {
            return res;
        }
        buff += (size_t)n * s_sector_bytes[pdrv];
        sector += n;
        count -= n;
    }
    return RES_OK;
}
DRESULT ff_disk_write (BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count)
{
    const UINT max = s_max_sectors[pdrv];
    if (max == 0 || count <= max) {
        return s_impls[pdrv]->write(pdrv, buff, sector, count);
    }
    /* То же деление, что у чтения (запись длиннее буфера драйвер растит так же) */
    while (count > 0) {
        const UINT n = count < max ? count : max;
        const DRESULT res = s_impls[pdrv]->write(pdrv, buff, sector, n);
        if (res != RES_OK) {
            return res;
        }
        buff += (size_t)n * s_sector_bytes[pdrv];
        sector += n;
        count -= n;
    }
    return RES_OK;
}
DRESULT ff_disk_ioctl (BYTE pdrv, BYTE cmd, void* buff)
{
    return s_impls[pdrv]->ioctl(pdrv, cmd, buff);
}

DWORD get_fattime(void)
{
    time_t t = time(NULL);
    struct tm tmr;
    localtime_r(&t, &tmr);
    int year = tmr.tm_year < 80 ? 0 : tmr.tm_year - 80;
    return    ((DWORD)(year) << 25)
            | ((DWORD)(tmr.tm_mon + 1) << 21)
            | ((DWORD)tmr.tm_mday << 16)
            | (WORD)(tmr.tm_hour << 11)
            | (WORD)(tmr.tm_min << 5)
            | (WORD)(tmr.tm_sec >> 1);
}

bool ff_diskio_is_registered(BYTE ldrv)
{
    if (ldrv < FF_VOLUMES && s_impls[ldrv]) {
        return true;
    }
    return false;
}

inline DRESULT ff_diskio_get_sector_size(BYTE ldrv, UINT* out_bytes_per_sector)
{
    return ff_disk_ioctl(ldrv, GET_SECTOR_SIZE, (void*) out_bytes_per_sector);
}
