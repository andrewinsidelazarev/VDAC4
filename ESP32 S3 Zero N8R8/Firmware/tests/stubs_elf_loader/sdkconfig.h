#pragma once

// Настройки загрузчика ELF как у прошивки (sdkconfig): перенос .text на шину
// команд ESP32-S3 и раздельные буферы команд и данных.

#define CONFIG_IDF_TARGET_ESP32S3 1
#define CONFIG_ELF_LOADER_BUS_ADDRESS_MIRROR 1
#define CONFIG_ELF_LOADER_CACHE_OFFSET 1
