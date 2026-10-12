# libxmp-lite — плеер трекерной музыки (MOD, S3M, XM, IT), MIT; см. README.md.
# В TRACKER.ELF играет IT (MOD, XM и S3M — components/tracker). esp/ — то, чего
# модулю не даёт ядро прошивки (xmp_esp_shim.c); имена из libc, которых у
# модуля нет, переименованы ключами -D только для исходников libxmp.
file(GLOB XMP_LITE_SRCS "${CMAKE_CURRENT_LIST_DIR}/src/lite/lite-*.c")
idf_component_register(SRCS ${XMP_LITE_SRCS} "esp/xmp_esp_shim.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src")
target_compile_definitions(${COMPONENT_LIB} PUBLIC LIBXMP_STATIC
                           PRIVATE LIBXMP_CORE_PLAYER LIBXMP_ESP_MODULE
                                   __PICOLIBC_ERRNO_FUNCTION=__errno
                                   fread=xmp_fread fseek=xmp_fseek ftell=xmp_ftell
                                   fclose=xmp_fclose feof=xmp_feof ferror=xmp_ferror
                                   fgetc=xmp_fgetc _ctype_b=xmp_ctype_b)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -w)
