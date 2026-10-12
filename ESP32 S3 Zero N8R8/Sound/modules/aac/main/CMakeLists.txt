idf_component_register(SRCS "aac_source.c"
                       PRIV_INCLUDE_DIRS "../../../../Firmware/modules/sdk/include"
                       PRIV_REQUIRES helix_aac)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
