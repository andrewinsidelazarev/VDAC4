# MD.ELF — слой ESP ядра Mega Drive на VDAC4: приложение ESP (ABI 1.9+). Звук — ymfm (C++, BSD-3).
# Сборка — tools/build.ps1 (idf.py, цель elf; итог build/md_vdac4.app.elf → dist/MD.ELF).
cmake_minimum_required(VERSION 3.16)
set(VDAC4_ROOT "E:/zx/Tang Nano 20k VDAC/VDAC4" CACHE PATH "VDAC4 source tree")
set(ZM_ROOT "${VDAC4_ROOT}/ESP32 S3 Zero N8R8/Firmware")
set(ZM_SDK_INCLUDE "${ZM_ROOT}/modules/sdk/include" CACHE PATH "zm_app.h — ABI приложений ESP")
set(EXTRA_COMPONENT_DIRS "${ZM_ROOT}/components/elf_loader" "${ZM_ROOT}/components/cmake_utilities")
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(md_vdac4)
include("${ZM_ROOT}/components/elf_loader/elf_loader.cmake")
project_elf(md_vdac4)
