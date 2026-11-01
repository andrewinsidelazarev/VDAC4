# Без ivopts: литерал «адрес минус смещение» загрузчик ESP переводит неверно (см. tests/check_elf_pointers.py).
idf_component_register(SRCS app.c ps2_hid.c INCLUDE_DIRS "." "${ZM_SDK_INCLUDE}")
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -O2 -fno-ivopts)
