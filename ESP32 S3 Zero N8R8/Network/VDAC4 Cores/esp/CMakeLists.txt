# COREMENU.ELF — экран плагина VDAC4 Cores на HDMI Tang: приложение ESP (ABI модулей 1.10).
# Лежит на microSD Tang в \CORES\ рядом с ядрами, плагин запускает его через CORE_APP COREMENU.
# Сборка — tools/build.ps1 (idf.py, цель elf; итог build/coremenu.app.elf → dist/COREMENU.ELF).
cmake_minimum_required(VERSION 3.16)
set(VDAC4_ROOT "E:/zx/Tang Nano 20k VDAC/VDAC4" CACHE PATH "VDAC4 source tree")
set(ZM_ROOT "${VDAC4_ROOT}/ESP32 S3 Zero N8R8/Firmware")
set(ZM_SDK_INCLUDE "${ZM_ROOT}/modules/sdk/include" CACHE PATH "zm_app.h — ABI приложений ESP")
set(EXTRA_COMPONENT_DIRS "${ZM_ROOT}/components/elf_loader" "${ZM_ROOT}/components/cmake_utilities")
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(coremenu)
include("${ZM_ROOT}/components/elf_loader/elf_loader.cmake")
project_elf(coremenu)
