# Сборка прошивки ZiFi под плату VDAC4 A3.1 в ESP-IDF 6.0 (idf.py build).
#
# Зачем ESP-IDF, а не PlatformIO/Arduino IDE. На плате VDAC ESP32-S3 со временем станет
# хостом ОС поверх FreeRTOS: нужны собственные настройки ядра (sdkconfig), свежие драйверы
# (SPI slave HD, I2S) и менеджер компонентов. Код ZiFi при этом остаётся прежним: Arduino-ядро
# подключено как обычный компонент ESP-IDF (main/idf_component.yml).
#
# Исходники — src/, include/, lib/; прежняя сборка PlatformIO для переходника ZiFi из
# этого репозитория снята.

cmake_minimum_required(VERSION 3.22)

# Версию прошивки видит и main/CMakeLists.txt (ZIFI_BUILD_VERSION), и упаковщик
# (через CMakeCache.txt). FORCE: значение в кэше уже настроенной сборки всегда
# повторяет записанное здесь, иначе правка версии не дошла бы до прошивки.
set(ZIFI_VDAC4_VERSION "vdac4-0.1.73" CACHE STRING "Версия прошивки для платы VDAC4" FORCE)

# Собирать только main и то, от чего он зависит, а не все компоненты ESP-IDF подряд.
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(zifi-vdac4)

# Arduino-ядро объявляет в манифесте десяток сторонних компонентов (Modbus, Zigbee, модемы,
# Ethernet-PHY), и менеджер компонентов делает их обязательными. В прошивку их код не попадает
# (компоновщик выбрасывает неиспользуемое), но собрать их приходится. ESP-IDF считает
# предупреждения ошибками, а GCC 15 находит в этом чужом коде новые предупреждения, поэтому
# для загруженных компонентов «предупреждение = ошибка» отключено. К коду ZiFi это не относится.
# Причина паники — в RTC-память (src/crash_record.cpp): обёртку обработчика паники
# даёт Arduino-ядро (set_arduino_panic_handler), ключ --wrap её включает.
idf_build_get_property(zifi_executable EXECUTABLE)
target_link_libraries(${zifi_executable} PRIVATE "-Wl,--wrap=esp_panic_handler")

idf_build_get_property(zifi_build_components BUILD_COMPONENTS)
foreach(zifi_component ${zifi_build_components})
    idf_component_get_property(zifi_component_dir ${zifi_component} COMPONENT_DIR)
    if(zifi_component_dir MATCHES "/managed_components/")
        idf_component_get_property(zifi_component_lib ${zifi_component} COMPONENT_LIB)
        get_target_property(zifi_component_type ${zifi_component_lib} TYPE)
        if(NOT zifi_component_type STREQUAL "INTERFACE_LIBRARY")
            target_compile_options(${zifi_component_lib} PRIVATE -Wno-error)
        endif()
    endif()
endforeach()
