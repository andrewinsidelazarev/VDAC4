cmake_minimum_required(VERSION 3.15)

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

sdk_add_include_directories(. ../copro)

target_sources(app PRIVATE
    fpga_link.c
    fpga_jtag.c
    fw_update.c
    fw_commit.c
    sha256.c
)
# VDAC4_COPRO_BRIDGE в окружении — прошивка 10, мост: только обновление по сети и перезагрузка
# ПЛИС, без сопроцессора (FTC_BRIDGE) — маленький образ, который прошивка 9 принимает (её куча
# мала); с моста ставится полная
if(DEFINED ENV{VDAC4_COPRO_BRIDGE})
    sdk_add_compile_definitions(-DFTC_BRIDGE)
else()
    target_sources(app PRIVATE ../copro/ft_copro.c ../copro/ft_inflate.c ../copro/ft_image.c ../copro/ft_3d.c
        core_cache.c core_jtag.c)
    # с прошивки 36: горячий код растеризатора, с 37 — и подготовка кадра, TEXMESH — в ITCM (FTC3D_TCM в ft_3d.c)
    set_source_files_properties(../copro/ft_3d.c PROPERTIES COMPILE_OPTIONS "-O2;-DFTC3D_TCM")
    # с прошивки 34: декод JPEG/видео и запись байтов — тоже -O2 (с -Os кадр видео ~60 мс)
    set_source_files_properties(../copro/ft_image.c ../copro/ft_copro.c PROPERTIES COMPILE_OPTIONS "-O2")
    # с прошивки 40: WebP — декодер libwebp 1.5.0 (copro/third_party/libwebp, правки — README.VDAC4.md,
    # список файлов — vdac4_sources.txt; только код C, без потоков, масштаба и видов точек кроме RGBA)
    file(STRINGS ../copro/third_party/libwebp/vdac4_sources.txt WEBP_SRCS)
    list(TRANSFORM WEBP_SRCS PREPEND ../copro/third_party/libwebp/)
    target_sources(app PRIVATE ${WEBP_SRCS})
    sdk_add_include_directories(../copro/third_party/libwebp)
    sdk_add_compile_definitions(-DHAVE_CONFIG_H -DWEBP_REDUCE_SIZE -DWEBP_REDUCE_CSP)
    # горячие файлы декодера — -O2 (модель BL616: VP8 1280×720 — 1,36 → 0,86 с, код +17 КБ), прочие — -Os SDK
    set(WEBP_O2 dec/frame_dec dec/vp8_dec dec/vp8l_dec dsp/dec dsp/lossless dsp/upsampling dsp/yuv utils/bit_reader_utils)
    list(TRANSFORM WEBP_O2 PREPEND ../copro/third_party/libwebp/src/)
    list(TRANSFORM WEBP_O2 APPEND .c)
    set_source_files_properties(${WEBP_O2} PROPERTIES COMPILE_OPTIONS "-O2")
endif()

sdk_set_main_file(main.c)

project(vdac4_copro)
