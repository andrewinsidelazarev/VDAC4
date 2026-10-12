# Приложение, обложка (разбор метки и миниатюра — исходники прошивки, Firmware/src) и плейлист радио.
idf_component_register(SRCS "player.c" "view.c" "cover_glue.cpp" "radio_list.c"
                            "../../../../Firmware/src/cover_art.cpp" "../../../../Firmware/src/cover_jpeg.cpp"
                       PRIV_INCLUDE_DIRS "../../../../Firmware/modules/sdk/include" "../../../../Firmware/include")
# cover_jpeg.cpp собирается для платы VDAC4 (TJpgDec из ПЗУ ESP32-S3).
target_compile_definitions(${COMPONENT_LIB} PRIVATE ZIFI_BOARD_VDAC4=1)
