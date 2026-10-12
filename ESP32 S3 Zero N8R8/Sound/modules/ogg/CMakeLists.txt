# OGG.ELF — декодер Ogg Vorbis (stb_vorbis, components/stb_vorbis) для плеера
# VDAC4, модуль ESP из /WC/ESP/: файлы .OGG/.OGA и потоки интернет-радио Ogg.
# Сборка: tools/build_modules.ps1 (результат — modules/out/OGG.ELF).
cmake_minimum_required(VERSION 3.16)
# ZM_ROOT — папка прошивки (ESP32 S3 Zero N8R8/Firmware): загрузчик ELF, cmake_utilities,
# компонент tracker и скачанные менеджером компоненты; библиотеки кодеков — в Sound/components.
set(ZM_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../Firmware")
set(SOUND_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(EXTRA_COMPONENT_DIRS "${ZM_ROOT}/components/elf_loader"
                         "${ZM_ROOT}/components/cmake_utilities"
                         "${SOUND_ROOT}/components/stb_vorbis")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ogg)
set(ELF_COMPONENTS stb_vorbis)
include(elf_loader)
project_elf(ogg)
