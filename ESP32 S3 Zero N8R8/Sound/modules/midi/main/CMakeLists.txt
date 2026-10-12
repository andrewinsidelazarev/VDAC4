idf_component_register(SRCS "midi_source.c"
                       PRIV_INCLUDE_DIRS "../../../../Firmware/modules/sdk/include"
                       PRIV_REQUIRES tsf)
# Эффекты (Freeverb, хорус, эхо) и мягкое ограничение — на каждом отсчёте: -O2, а не
# отладочный -Og проекта модуля (с ним эффекты стоили 2644 такта на кадр).
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
