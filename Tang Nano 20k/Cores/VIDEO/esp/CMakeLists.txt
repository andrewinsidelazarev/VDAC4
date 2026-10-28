# VIDEO.ELF — слой ESP ядра видео VDAC4 (личность 05): приложение ESP (ABI модулей 1.9+).
# Сборка — tools/build.ps1 (idf.py, цель elf; итог build/video_vdac4.app.elf → dist/VIDEO.ELF).
cmake_minimum_required(VERSION 3.16)
set(VDAC4_ROOT "E:/zx/Tang Nano 20k VDAC/VDAC4" CACHE PATH "VDAC4 source tree")
set(ZM_ROOT "${VDAC4_ROOT}/ESP32 S3 Zero N8R8/Firmware")
set(ZM_SDK_INCLUDE "${ZM_ROOT}/modules/sdk/include" CACHE PATH "zm_app.h — ABI приложений ESP")
set(PUI_FONT_DIR "${VDAC4_ROOT}/ESP32 S3 Zero N8R8/Sound/player_ui/fonts" CACHE PATH "шрифт панели плеера")
set(EXTRA_COMPONENT_DIRS "${ZM_ROOT}/components/elf_loader" "${ZM_ROOT}/components/cmake_utilities")
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(video_vdac4)
include("${ZM_ROOT}/components/elf_loader/elf_loader.cmake")
project_elf(video_vdac4)
