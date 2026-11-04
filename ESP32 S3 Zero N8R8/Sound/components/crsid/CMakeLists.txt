# cRSID 1.58 (Hermit) — исходниками: модуль SID.ELF включает libcRSID.c в свой файл (sid_source.c),
# как это делает сам libcRSID.c с C64/*.c и host/*.c.
idf_component_register(INCLUDE_DIRS ".")
