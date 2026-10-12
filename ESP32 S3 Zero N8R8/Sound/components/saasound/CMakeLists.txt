idf_component_register(SRCS "SAAAmp.cpp" "SAADevice.cpp" "SAAEnv.cpp" "SAAFreq.cpp" "SAANoise.cpp"
                       INCLUDE_DIRS ".")
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fno-threadsafe-statics -Wno-unused-parameter)
