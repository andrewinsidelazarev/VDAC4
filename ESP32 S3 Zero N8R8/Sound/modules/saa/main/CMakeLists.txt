idf_component_register(SRCS "saa_source.cpp" "etracker.cpp"
                       INCLUDE_DIRS "../../../../Firmware/modules/sdk/include"
                       PRIV_REQUIRES saasound)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fno-threadsafe-statics)
