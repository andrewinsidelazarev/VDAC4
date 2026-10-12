# Главный компонент: исходники ZiFi из ../src и ../include без копирования.

file(GLOB ZIFI_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../src/*.cpp"
                       "${CMAKE_CURRENT_LIST_DIR}/../src/apps/*.cpp")

# Сборка сужена до нужного (COMPONENTS в корневом CMakeLists.txt), поэтому зависимости
# перечислены явно. joltwallet__littlefs нужен Arduino-обёртке LittleFS: она подключает его,
# только если компонент попал в сборку. espressif__network_provisioning нужен ради одного
# заголовка: WiFi.h подключает его, раз компонент есть в сборке, а путь к нему Arduino-ядро
# отдаёт только вместе с библиотекой WiFiProv, которая здесь выключена.
idf_component_register(
    SRCS ${ZIFI_SOURCES}
    INCLUDE_DIRS "../include" "../modules/sdk/include"
    REQUIRES
        espressif__arduino-esp32
        espressif__network_provisioning
        joltwallet__littlefs
        chmorgan__esp-libhelix-mp3
        libsmb2
        tracker
        elf_loader
        app_update
        esp_adc
        esp_driver_gpio
        esp_driver_i2s
        esp_driver_pcnt
        esp_driver_spi
        esp_driver_tsens
        esp_driver_usb_serial_jtag
        esp_mm
        esp_psram
        esp_timer
        esp_wifi
        lwip
        mbedtls
        # Флешка в разъёме USB модуля: USB-хост, драйвер USB MSC и FatFs с exFAT
        # (components/fatfs — копия компонента ESP-IDF с одной правкой).
        espressif__usb
        espressif__usb_host_msc
        fatfs
        vfs
)

# Две догадки GCC 15 о snprintf, которые ESP-IDF превращает в ошибки:
# -Wformat-truncation — короткие тексты состояния прошивка намеренно обрезает до размера поля;
# -Wrestrict — копирование между разными полями одного объекта он считает наложением.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=gnu++17 -Wno-format-truncation -Wno-restrict)
# ZIFI_DIAGNOSTIC_LOG=1: постоянный журнал во flash. На этой плате он нужен для наладки —
# пока модуль стоит в плате, USB к нему не подключён (см. include/zifi/bringup_log.hpp).
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ZIFI_BUILD_VERSION="${ZIFI_VDAC4_VERSION}"
    ZIFI_BOARD_VDAC4=1
    ZIFI_DIAGNOSTIC_LOG=1
    BOARD_HAS_PSRAM
    ZIFI_ELF_PROBE=1
    ZIFI_CRASH_RECORD=1
)
# Проверка загрузчика модулей ESP: пробный модуль вшит в прошивку (src/elf_probe.cpp).
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../modules/hello/hello.app.elf" BINARY)
