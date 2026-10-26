# Прошивка BL616 модуля Tang Nano 20K: сопроцессор FT812 для VDAC4.
#   BL_SDK_BASE — bouffalo_sdk; в PATH — riscv64-unknown-elf-gcc (T-Head), make, ninja, cmake.
#   make            → build/build_out/vdac4_copro_bl616.bin
SDK_DEMO_PATH ?= $(abspath .)
BL_SDK_BASE ?= $(abspath ../../../bl616/bouffalo_sdk)

export BL_SDK_BASE

CHIP ?= bl616
BOARD ?= bl616dk
CROSS_COMPILE ?= riscv64-unknown-elf-

include $(BL_SDK_BASE)/project.build
