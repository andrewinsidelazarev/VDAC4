#pragma once
/* Заглушка sdkconfig.h для стенда diskio.c на ПК (tests/test_usb_drive.py): только то, что читает
 * components/fatfs/src/ffconf.h, — значения как у прошивки VDAC4. */
#include <assert.h>    /* diskio.c зовёт assert, а в ESP-IDF его приносят другие заголовки */
#include <stdbool.h>   /* diskio_private.h: bool (в C MSVC — только через stdbool.h) */
#include <time.h>
#define CONFIG_FATFS_VOLUME_COUNT 2
#define CONFIG_FATFS_LFN_HEAP 1
#define CONFIG_FATFS_MAX_LFN 255
#define CONFIG_FATFS_API_ENCODING_UTF_8 1
#define CONFIG_FATFS_CODEPAGE 437
#define CONFIG_FATFS_FS_LOCK 0
#define CONFIG_FATFS_TIMEOUT_MS 10000
#define CONFIG_FATFS_PER_FILE_CACHE 1
#define CONFIG_FATFS_USE_STRFUNC_NONE 1
#define CONFIG_WL_SECTOR_SIZE 4096
/* MSVC: localtime_r нет — то же через localtime_s (только для get_fattime в diskio.c) */
#define localtime_r(t, tm) localtime_s((tm), (t))
