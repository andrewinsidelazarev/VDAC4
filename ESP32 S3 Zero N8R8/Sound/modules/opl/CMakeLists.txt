# OPL.ELF — декодер музыки микросхем Yamaha семейства OPL (OPL, MSX-AUDIO,
# OPL2, OPL3, OPL4, OPLL: VGM, VGZ, DRO, IMF) для плеера VDAC4, модуль ESP из
# /WC/ESP/; эмулятор — ymfm (components/ymfm).
# Сборка: tools/build_modules.ps1 (результат — modules/out/OPL.ELF).
cmake_minimum_required(VERSION 3.16)
# ZM_ROOT — папка прошивки (ESP32 S3 Zero N8R8/Firmware): загрузчик ELF, cmake_utilities,
# компонент tracker и скачанные менеджером компоненты; библиотеки кодеков — в Sound/components.
set(ZM_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../Firmware")
set(SOUND_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(EXTRA_COMPONENT_DIRS "${ZM_ROOT}/components/elf_loader"
                         "${ZM_ROOT}/components/cmake_utilities"
                         "${SOUND_ROOT}/components/ymfm")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(opl)
set(ELF_COMPONENTS ymfm)
include(elf_loader)
project_elf(opl)
