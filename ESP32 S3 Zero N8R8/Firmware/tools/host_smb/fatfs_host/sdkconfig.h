#pragma once

// Настройки FatFs прошивки (build/config/sdkconfig.h, ESP-IDF menuconfig) для той же FatFs
// (components/fatfs) на стенде ПК: шара SMB «TANG» проверяется с теми же длинными именами,
// кодовой страницей 437 и размерами сектора, что на плате.
#define CONFIG_FATFS_VOLUME_COUNT 2
#define CONFIG_FATFS_LFN_HEAP 1
#define CONFIG_FATFS_SECTOR_4096 1
#define CONFIG_FATFS_CODEPAGE_437 1
#define CONFIG_FATFS_CODEPAGE 437
#define CONFIG_FATFS_MAX_LFN 255
#define CONFIG_FATFS_API_ENCODING_ANSI_OEM 1
#define CONFIG_FATFS_FS_LOCK 0
#define CONFIG_FATFS_TIMEOUT_MS 10000
#define CONFIG_FATFS_PER_FILE_CACHE 1
#define CONFIG_FATFS_USE_FASTSEEK 1
#define CONFIG_FATFS_USE_STRFUNC_NONE 1
#define CONFIG_FATFS_FAST_SEEK_BUFFER_SIZE 64
#define CONFIG_FATFS_USE_DYN_BUFFERS 1
#define CONFIG_FATFS_DONT_TRUST_FREE_CLUSTER_CNT 0
#define CONFIG_FATFS_DONT_TRUST_LAST_ALLOC 0
#define CONFIG_WL_SECTOR_SIZE 4096
