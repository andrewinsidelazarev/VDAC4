# NEXT.ELF — слой ESP ядра ZX Next на VDAC4: приложение ESP (ABI модулей 1.4, прошивка ESP 0.1.84+).
# Сборка — tools/build.ps1 (idf.py, цель elf; итог build/next_vdac4.app.elf → dist/NEXT.ELF).
cmake_minimum_required(VERSION 3.16)
set(VDAC4_ROOT "E:/zx/Tang Nano 20k VDAC/VDAC4" CACHE PATH "VDAC4 source tree")
set(ZM_ROOT "${VDAC4_ROOT}/ESP32 S3 Zero N8R8/Firmware")
set(ZM_SDK_INCLUDE "${ZM_ROOT}/modules/sdk/include" CACHE PATH "zm_app.h — ABI приложений ESP")
set(EXTRA_COMPONENT_DIRS "${ZM_ROOT}/components/elf_loader" "${ZM_ROOT}/components/cmake_utilities")
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(next_vdac4)
include("${ZM_ROOT}/components/elf_loader/elf_loader.cmake")
project_elf(next_vdac4)
