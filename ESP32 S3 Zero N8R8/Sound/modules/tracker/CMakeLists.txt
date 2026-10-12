# TRACKER.ELF — декодер MOD/XM/S3M (components/tracker), IT и прочих форматов
# (components/libxmp, полная) для плеера VDAC4, модуль ESP.
# Сборка: tools/build_modules.ps1 (результат — modules/out/TRACKER.ELF).
cmake_minimum_required(VERSION 3.16)
# ZM_ROOT — папка прошивки (ESP32 S3 Zero N8R8/Firmware): загрузчик ELF, cmake_utilities,
# компонент tracker и скачанные менеджером компоненты; библиотеки кодеков — в Sound/components.
set(ZM_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../Firmware")
set(SOUND_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(EXTRA_COMPONENT_DIRS "${ZM_ROOT}/components/elf_loader"
                         "${ZM_ROOT}/components/cmake_utilities"
                         "${ZM_ROOT}/components/tracker"
                         "${SOUND_ROOT}/components/libxmp")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tracker_module)
# В модуле без IRAM_ATTR: секции .iram1 загрузчик не кладёт (tracker.cpp).
idf_component_get_property(zm_tracker_lib tracker COMPONENT_LIB)
target_compile_definitions(${zm_tracker_lib} PRIVATE TRACKER_ELF_MODULE=1)
set(ELF_COMPONENTS tracker libxmp)
include(elf_loader)
project_elf(tracker_module)
