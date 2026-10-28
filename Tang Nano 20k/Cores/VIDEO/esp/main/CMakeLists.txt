idf_component_register(SRCS app.c avi_info.c audio.c hud.cpp "${PUI_FONT_DIR}/pui_font.cpp"
                       INCLUDE_DIRS "." "${ZM_SDK_INCLUDE}" "${PUI_FONT_DIR}")
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=gnu11> -O2)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17 -fno-exceptions -fno-rtti>)
