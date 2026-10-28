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
    set_source_files_properties(../copro/ft_3d.c PROPERTIES COMPILE_OPTIONS "-O2")
    # с прошивки 34: декод JPEG/видео и запись байтов — тоже -O2 (с -Os кадр видео ~60 мс)
    set_source_files_properties(../copro/ft_image.c ../copro/ft_copro.c PROPERTIES COMPILE_OPTIONS "-O2")
endif()

sdk_set_main_file(main.c)

project(vdac4_copro)
