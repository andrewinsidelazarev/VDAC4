idf_component_register(SRCS "tsfm_source.cpp" "ay_players.cpp" "tfc.cpp" "tfd.cpp"
                           "ayumi/ayumi.c" "ymfm/ymfm_opn.cpp" "ymfm/ymfm_ssg.cpp"
                       INCLUDE_DIRS "../../../../Firmware/modules/sdk/include" "ymfm"
                       PRIV_REQUIRES ymfm)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -w $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>)
target_compile_definitions(${COMPONENT_LIB} PRIVATE NDEBUG)
