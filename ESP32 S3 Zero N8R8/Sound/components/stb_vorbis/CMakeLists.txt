# Декодер Ogg Vorbis для OGG.ELF (modules/ogg): stb_vorbis v1.22 (public domain,
# Sean Barrett) без правок, настройки — stb_vorbis_vdac4.h, см. README.md.

idf_component_register(
    SRCS "stb_vorbis_vdac4.c"
    INCLUDE_DIRS "."
)

# Чужой код: предупреждения не правим; -O2 — декодирование пакета; NDEBUG — без
# __assert_func (ядро его модулям не даёт).
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -w -DNDEBUG)
