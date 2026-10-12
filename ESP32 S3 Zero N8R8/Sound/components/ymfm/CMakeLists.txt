# Эмуляторы микросхем Yamaha семейства OPL для декодера OPL.ELF (modules/opl):
# ymfm Аарона Джайлса (BSD-3), только OPL-часть, см. README.md.

idf_component_register(
    SRCS "src/ymfm_opl.cpp" "src/ymfm_pcm.cpp" "src/ymfm_adpcm.cpp" "src/fast_opl.cpp"
    INCLUDE_DIRS "include"
)

# Чужой код: предупреждения в нём не правим; -O2 — цикл синтеза, assert не
# нужен (в модуле ESP его функции нет). NDEBUG — и тем, кто включает заголовки.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -w -fno-threadsafe-statics)
target_compile_definitions(${COMPONENT_LIB} PUBLIC NDEBUG)
