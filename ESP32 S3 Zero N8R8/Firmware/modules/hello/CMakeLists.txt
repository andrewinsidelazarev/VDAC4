# Пробный модуль ESP для загрузчика ELF (components/elf_loader): печатает строку
# и возвращает argc + 40. Прошивка с ZIFI_ELF_PROBE запускает его при старте
# и пишет итог в журнал наладки. Сборка: idf.py -C modules/hello set-target esp32s3 build
# (результат build/hello.app.elf; копия лежит рядом, её вшивает прошивка).
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/elf_loader"
                         "${CMAKE_CURRENT_LIST_DIR}/../../components/cmake_utilities")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hello)
include(elf_loader)
project_elf(hello)
