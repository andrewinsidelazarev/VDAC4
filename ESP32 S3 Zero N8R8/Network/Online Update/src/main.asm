; VDAC4 Online Update для ZX Evolution / TS-Config — обновление прошивки ESP32-S3
; платы VDAC4 с GitHub (репозиторий VDAC4, Firmware/firmware/vdac4). Образ качает и
; проверяет сама ESP; через Z80 идут только команды и индикатор. Канал до ESP —
; SPI платы VDAC4 (zifi_link.asm → zifi_spi.asm, сборка с -DZIFI_SPI, без DMA).
; Перенос плагина ZIFIUPD.WMF переходника ZiFi (UART) на канал SPI.
;
; Файл задаёт физическую компоновку WMF. Первые 512 байтов — заголовок формата
; #0A, код собирается с логическим адресом #8000 и занимает одну страницу.

        DEVICE ZXSPECTRUM128
        INCLUDE "wc_api.inc"

startCode:
        ORG #0000
        INCLUDE "wc_header.inc"

        ALIGN 512                         ; код начинается после сектора заголовка
        DISP #8000
mainStart:
        INCLUDE "updater.asm"
        ; zifi.ini передаётся целиком, до 1024 байтов (а не один сектор).
        DEFINE CONFIG_FULL_INI
        INCLUDE "config.asm"
        INCLUDE "proto.asm"
        INCLUDE "zifi_link.asm"

; Переходники API Wild Commander. Номер функции передаётся в A и выполняется
; общим входом WC_API. Диспетчер WC меняет AF и AF' местами, поэтому параметр,
; который функция ждёт в A, кладётся в A' через EX AF,AF'.
WC_PRWOW:
        ld a,FN_PRWOW
        jp WC_API
WC_RRESB:
        ld a,FN_RRESB
        jp WC_API
WC_TXTPR:
        ld a,FN_TXTPR
        jp WC_API
WC_GEDPL:
        ld a,FN_GEDPL
        jp WC_API
WC_ESC:
        ld a,FN_ESC
        jp WC_API
WC_STREAM:
        ld a,FN_STREAM
        jp WC_API
WC_FENTRY:
        ld a,FN_FENTRY
        jp WC_API
WC_GFILE:
        ld a,FN_GFILE
        jp WC_API
WC_GDIR:
        ld a,FN_GDIR
        jp WC_API
WC_LOAD512:
        ld a,FN_LOAD512
        jp WC_API
WC_ADIR:
        ex af,af'
        ld a,FN_ADIR
        jp WC_API
WC_FINDNEXT:
        ex af,af'
        ld a,FN_FINDNEXT
        jp WC_API
WC_INT_PL:
        ex af,af'
        ld a,FN_INT_PL
        jp WC_API

mainEnd:
        ; Код и постоянные данные обязаны поместиться в окно #8000..#BFFF.
        ASSERT mainEnd <= #C000, "plugin code exceeds the #8000 page"
        ENT
endCode:
        SAVEBIN "../build/vdac4/VDAC4UPD.WMF",startCode,endCode-startCode
