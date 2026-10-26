# Приложение, обложка (разбор метки и миниатюра — исходники прошивки, Firmware/src), обложка из
# сети и плейлист радио.
# Экран на HDMI Tang (screen.cpp, 1.26): PlayerOverlay прошивки и отрисовка Sound/player_ui — без копии.
idf_component_register(SRCS "player.c" "view.c" "cover_glue.cpp" "radio_list.c" "online_cover.c" "screen.cpp"
                            "../../../../Firmware/src/cover_art.cpp" "../../../../Firmware/src/cover_jpeg.cpp"
                            "../../../../Firmware/src/player_overlay.cpp"
                            "../../../player_ui/player_ui.cpp" "../../../player_ui/fonts/pui_font.cpp"
                       PRIV_INCLUDE_DIRS "../../../../Firmware/modules/sdk/include" "../../../../Firmware/include"
                                         "../../../player_ui")
# cover_jpeg.cpp собирается для платы VDAC4 (TJpgDec из ПЗУ ESP32-S3).
target_compile_definitions(${COMPONENT_LIB} PRIVATE ZIFI_BOARD_VDAC4=1)
