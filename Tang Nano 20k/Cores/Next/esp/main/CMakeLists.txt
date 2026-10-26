idf_component_register(SRCS app.c ps2_hid.c esp_at.c INCLUDE_DIRS "." "${ZM_SDK_INCLUDE}")
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -O2)
