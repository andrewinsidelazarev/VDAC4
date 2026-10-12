idf_component_register(SRCS "flac_source.c"
                       INCLUDE_DIRS "../../../../Firmware/modules/sdk/include"
                       PRIV_REQUIRES foxen_flac)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
