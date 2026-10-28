# Ядро видео (MJPEG AVI плагина FTVIEW4 декодирует ПЛИС, rtl/vp_core.sv): HAS_EVO = 0, HAS_FT = 0,
# HAS_VIDEO = 1; 720p50, SDRAM на clk_mem. Переход (RECONFIG, по тишине при M_SEL = 0) — в слот 0.
# Запуск из fpga/vdac2:  gw_sh scripts/gowin_fb_video.tcl  → impl/pnr/ts_fb_video.fs
set boot_slot 3
set core_kind video
source scripts/gowin_fb.tcl
