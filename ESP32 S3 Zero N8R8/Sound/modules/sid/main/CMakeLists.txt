# SID.ELF: музыка C64 (PSID, RSID) на cRSID 1.58; libcRSID.c включается в sid_source.c одним файлом.
idf_component_register(SRCS "sid_source.c"
                       INCLUDE_DIRS "../../../../Firmware/modules/sdk/include"
                       PRIV_REQUIRES crsid)
# cRSID написан под gnu99: в C23 (так собирает IDF) «void f()» — функция без параметров, и его вызовы
# с аргументами не собираются. Его предупреждения (register, static в inline) — не ошибки; -O2, как у
# прочих модулей.
# IDF собирает с -fno-jump-tables: switch команд 6502 (256 случаев) становится цепочкой сравнений. В
# модуле таблица переходов — в .text сразу за jx, смещениями от своего начала; адрес начала — литерал
# с R_XTENSA_RELATIVE, загрузчик переносит его на шину команд, и l32i читает таблицу оттуда (на плате
# работает с 1.1). tests/check_elf_pointers.py (Cores/AMIGA) показывает такие адреса как «не начало
# функции» — в SID.ELF 1.3 все 17 стоят сразу за jx, запись по ним не идёт.
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wno-error -O2 -fjump-tables -ftree-switch-conversion
                       -Wno-unused -Wno-unused-parameter
                       -Wno-implicit-fallthrough -Wno-sign-compare -Wno-missing-field-initializers)
