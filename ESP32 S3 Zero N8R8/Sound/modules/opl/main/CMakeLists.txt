idf_component_register(SRCS "opl_source.cpp" "mwm.cpp" "mfm.cpp" "em_inflate.c"
                       PRIV_INCLUDE_DIRS "../../../../Firmware/modules/sdk/include"
                       PRIV_REQUIRES ymfm)
# Цикл синтеза и распаковка VGZ — с -O2; защита статических переменных не
# нужна (модуль однопоточный, функций __cxa_guard_* в ядре нет). em_inflate —
# чужой код: его предупреждения не правим.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>)
set_source_files_properties(em_inflate.c PROPERTIES COMPILE_OPTIONS "-w")
