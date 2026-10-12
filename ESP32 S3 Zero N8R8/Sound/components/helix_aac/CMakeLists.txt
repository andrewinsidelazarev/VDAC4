# Декодер AAC для AAC.ELF (modules/aac): Helix HE-AAC (RealNetworks, RPSL/RCSL)
# из arduino-libhelix с правками порта, см. README.md.

idf_component_register(
    SRCS "aacdec.c" "aactabs.c" "bitstream.c" "buffers.c" "dct4.c" "decelmnt.c" "dequant.c"
         "fft.c" "filefmt.c" "huffman.c" "hufftabs.c" "imdct.c" "noiseless.c" "pns.c" "sbr.c"
         "sbrfft.c" "sbrfreq.c" "sbrhfadj.c" "sbrhfgen.c" "sbrhuff.c" "sbrimdct.c" "sbrmath.c"
         "sbrqmf.c" "sbrside.c" "sbrtabs.c" "stproc.c" "tns.c" "trigtabs.c"
    INCLUDE_DIRS "."
)

# Чужой код: предупреждения не правим; -O2 — декодирование кадра; NDEBUG — без
# __assert_func (ядро его модулям не даёт).
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -w -DNDEBUG)
