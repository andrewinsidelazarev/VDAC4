# libxmp — плеер трекерной музыки Extended Module Player, полная сборка (MIT; см.
# README.md). В TRACKER.ELF играет IT и всё, чего не берёт components/tracker: MED и
# OctaMED, MultiTracker, Scream Tracker 2, 669, Ultra Tracker, Oktalyzer,
# Soundtracker на 15 сэмплов, упакованные форматы Amiga (ProWizard) и прочие.
# Исходники — sources.cmake (как в Makefile.vc libxmp); распаковщики не собраны:
# им нужны временные файлы и popen. esp/ — то, чего модулю не даёт ядро прошивки
# (xmp_esp_shim.c), и xmp_esp.h — сжатие отсчётов по велению TRACKER; имена из
# libc, которых у модуля нет, и malloc с прочими (память libxmp — в PSRAM)
# переименованы ключами -D только для исходников libxmp.
include("${CMAKE_CURRENT_LIST_DIR}/sources.cmake")
list(TRANSFORM XMP_SOURCES PREPEND "${CMAKE_CURRENT_LIST_DIR}/")
idf_component_register(SRCS ${XMP_SOURCES} "esp/xmp_esp_shim.c"
                       INCLUDE_DIRS "include" "esp"
                       PRIV_INCLUDE_DIRS "src")
target_compile_definitions(${COMPONENT_LIB} PUBLIC LIBXMP_STATIC
                           PRIVATE LIBXMP_NO_DEPACKERS LIBXMP_ESP_MODULE
                                   __PICOLIBC_ERRNO_FUNCTION=__errno
                                   fread=xmp_fread fseek=xmp_fseek ftell=xmp_ftell
                                   fclose=xmp_fclose feof=xmp_feof ferror=xmp_ferror
                                   fgetc=xmp_fgetc fwrite=xmp_fwrite fputc=xmp_fputc
                                   fflush=xmp_fflush fopen=xmp_fopen _ctype_b=xmp_ctype_b
                                   malloc=xmp_malloc calloc=xmp_calloc
                                   realloc=xmp_realloc free=xmp_free getenv=xmp_getenv)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -w)
