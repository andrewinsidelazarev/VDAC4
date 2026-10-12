# libsmb2 (сервер SMB2/3) как компонент ESP-IDF. Исходники — ../../lib/libsmb2, те же,
# что собирает PlatformIO по library.json; состав и определения повторяют его.

set(SMB2_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../lib/libsmb2")

file(GLOB SMB2_SOURCES "${SMB2_ROOT}/lib/*.c")
list(FILTER SMB2_SOURCES EXCLUDE REGEX "/(aes_apple|krb5-wrapper)\\.c$")

idf_component_register(
    SRCS ${SMB2_SOURCES}
    INCLUDE_DIRS "${SMB2_ROOT}/include"
    PRIV_INCLUDE_DIRS "${SMB2_ROOT}/include/smb2" "${SMB2_ROOT}/include/esp" "${SMB2_ROOT}/lib"
    REQUIRES lwip
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ESP_PLATFORM
    HAVE_CONFIG_H
    NEED_READV
    NEED_WRITEV
    NEED_GETLOGIN_R
    NEED_RANDOM
    NEED_SRANDOM
    "_U_=__attribute__((unused))"
)
# Сторонний код: предупреждения нового GCC не должны останавливать сборку.
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
