# Синтезатор SoundFont2 и разбор MIDI для декодера MIDI.ELF (modules/midi):
# TinySoundFont v0.9 (MIT) и TinyMidiLoader v0.7 (zlib) с правками, см. README.md.

idf_component_register(
    SRCS "tsf.c"
    INCLUDE_DIRS "include"
)

# Чужой код: предупреждения GCC 15 в нём не правим; -O2 — цикл синтеза.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -w)
