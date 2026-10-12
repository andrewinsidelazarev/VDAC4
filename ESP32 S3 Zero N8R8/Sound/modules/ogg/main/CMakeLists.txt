idf_component_register(SRCS "ogg_source.c"
                       PRIV_INCLUDE_DIRS "../../../../Firmware/modules/sdk/include"
                       PRIV_REQUIRES stb_vorbis)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
