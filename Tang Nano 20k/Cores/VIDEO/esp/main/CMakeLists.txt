# Декодер MP3 — тот же, что MP3.ELF плеера (Sound/modules/mp3, Helix), но вкомпилирован сюда: MP3.ELF
# в кэш модулей ESP кладёт только плагин плеера, и после других модулей его там могло не быть
# (27.10.2026: «MP3.ELF not available» — ролик с MP3 шёл без звука). Вход модуля main — mp3_main:
# сборка ELF задаёт всем main=app_main, здесь app_main=mp3_main (main → app_main → mp3_main).
set(VIDEO_MP3_DIR "${VDAC4_ROOT}/ESP32 S3 Zero N8R8/Sound/modules/mp3/main" CACHE PATH "mp3_source.c — декодер MP3.ELF")
set(VIDEO_HELIX_DIR "${VDAC4_ROOT}/ESP32 S3 Zero N8R8/Firmware/managed_components/chmorgan__esp-libhelix-mp3/libhelix-mp3"
    CACHE PATH "Helix MP3")
file(GLOB VIDEO_HELIX_SRCS "${VIDEO_HELIX_DIR}/*.c" "${VIDEO_HELIX_DIR}/real/*.c")
idf_component_register(SRCS app.c avi_info.c audio.c hud.cpp "${PUI_FONT_DIR}/pui_font.cpp"
                            "${VIDEO_MP3_DIR}/mp3_source.c" ${VIDEO_HELIX_SRCS}
                       INCLUDE_DIRS "." "${ZM_SDK_INCLUDE}" "${PUI_FONT_DIR}" "${VIDEO_HELIX_DIR}/pub"
                       PRIV_INCLUDE_DIRS "${VIDEO_HELIX_DIR}/real")
set_source_files_properties("${VIDEO_MP3_DIR}/mp3_source.c" PROPERTIES COMPILE_DEFINITIONS "app_main=mp3_main")
set_source_files_properties(${VIDEO_HELIX_SRCS} PROPERTIES COMPILE_OPTIONS "-Wno-unused-but-set-variable")
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=gnu11> -O2)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17 -fno-exceptions -fno-rtti>)
