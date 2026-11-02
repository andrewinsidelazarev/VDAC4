# Экран плагина VDAC4 Cores: обвязка (app.c) и отрисовка cores_ui (дизайн Codex) — без копии.
set(UI "../../cores_ui")
idf_component_register(SRCS "app.c" "${UI}/cores_ui.cpp" "${UI}/cui_card.cpp" "${UI}/assets/cui_art.cpp"
                            "${UI}/fonts/pui_font.cpp"
                       INCLUDE_DIRS "." "${ZM_SDK_INCLUDE}" "${UI}" "${UI}/assets" "${UI}/fonts")
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wall -Wextra -Werror)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
                                                $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17 -fno-exceptions -fno-rtti>)
