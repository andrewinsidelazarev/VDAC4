# PLAYER.ELF — медиапроигрыватель VDAC4, приложение ESP из /WC/ESP/.
# Сборка: tools/build_modules.ps1 (результат — modules/out/PLAYER.ELF).
cmake_minimum_required(VERSION 3.16)
# ZM_ROOT — папка прошивки (ESP32 S3 Zero N8R8/Firmware): загрузчик ELF, cmake_utilities,
# компонент tracker и скачанные менеджером компоненты; библиотеки кодеков — в Sound/components.
set(ZM_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../Firmware")
set(SOUND_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(EXTRA_COMPONENT_DIRS "${ZM_ROOT}/components/elf_loader"
                         "${ZM_ROOT}/components/cmake_utilities")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(player)
include(elf_loader)
project_elf(player)
