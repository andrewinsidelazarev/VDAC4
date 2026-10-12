# MP3.ELF — декодер MP3 (Helix) для плеера VDAC4, модуль ESP из /WC/ESP/.
# Сборка: tools/build_modules.ps1 (результат — modules/out/MP3.ELF).
cmake_minimum_required(VERSION 3.16)
# ZM_ROOT — папка прошивки (ESP32 S3 Zero N8R8/Firmware): загрузчик ELF, cmake_utilities,
# компонент tracker и скачанные менеджером компоненты; библиотеки кодеков — в Sound/components.
set(ZM_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../Firmware")
set(SOUND_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(EXTRA_COMPONENT_DIRS "${ZM_ROOT}/components/elf_loader"
                         "${ZM_ROOT}/components/cmake_utilities"
                         "${ZM_ROOT}/managed_components/chmorgan__esp-libhelix-mp3")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mp3)
set(ELF_COMPONENTS chmorgan__esp-libhelix-mp3)
include(elf_loader)
project_elf(mp3)
