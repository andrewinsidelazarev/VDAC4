idf_component_register(SRCS "foxen-flac.c" INCLUDE_DIRS ".")
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -DNDEBUG)
