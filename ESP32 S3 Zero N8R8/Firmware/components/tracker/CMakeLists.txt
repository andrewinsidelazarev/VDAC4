# Трекер TS-Labs: XM (libxm), MOD и S3M в одном контексте xm_context_t.
# Библиотечная часть pentevo/esp32/src/main/tracker.cpp (ревизия r140g) без
# задачи проигрывания и консоли TS-Labs; см. README.md.

idf_component_register(
    SRCS "tracker.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES heap log esp_common
)

# Чужой код как есть: предупреждения GCC 15 в нём не правим.
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17 -Wno-error -w)
