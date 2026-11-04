idf_component_register(SRCS app.c ps2_hid.c md_sound.cpp ymfm/ymfm_opn.cpp ymfm/ymfm_adpcm.cpp ymfm/ymfm_ssg.cpp
                       INCLUDE_DIRS "." "ymfm" "${ZM_SDK_INCLUDE}")
# Без ivopts: литерал «адрес минус смещение» загрузчик ESP переводит неверно (tests/check_elf_pointers.py).
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fno-ivopts $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
                       $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -fno-threadsafe-statics -w>)
target_compile_definitions(${COMPONENT_LIB} PRIVATE NDEBUG)
