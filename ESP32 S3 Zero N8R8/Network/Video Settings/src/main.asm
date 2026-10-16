; VDAC4 Video Settings для ZX Evolution / TS-Config — настройки видеотракта Tang Nano 20K
; платы VDAC4 из Wild Commander: состояние входа (растр, PCLK, окно, границы картинки) и
; MUX PHASE/DELAY/CLOCK, поправки окна, AUTO ALIGN, ASPECT, SIZE. Регистры Tang плагин
; читает и пишет командами TANG_READ/TANG_WRITE прошивки ESP (vdac4-0.1.72+); ESP передаёт
; их в Tang по LINK и сама сохраняет изменённые настройки во flash Tang — как с кнопок Tang.
; Канал до ESP — SPI платы VDAC4 (zifi_link.asm → zifi_spi.asm, сборка с -DZIFI_SPI).
;
; Файл задаёт физическую компоновку WMF. Первые 512 байтов — заголовок формата
; #0A, код собирается с логическим адресом #8000 и занимает одну страницу.

        DEVICE ZXSPECTRUM128
        INCLUDE "wc_api.inc"

FN_LEFT     equ #13
FN_RIGHT    equ #14

startCode:
        ORG #0000
        INCLUDE "wc_header.inc"

        ALIGN 512                         ; код начинается после сектора заголовка
        DISP #8000
mainStart:
        INCLUDE "video.asm"
        INCLUDE "ui.asm"
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
WC_KEY_UP:
        ld a,FN_UP
        jp WC_API
WC_KEY_DOWN:
        ld a,FN_DOWN
        jp WC_API
WC_KEY_LEFT:
        ld a,FN_LEFT
        jp WC_API
WC_KEY_RIGHT:
        ld a,FN_RIGHT
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
        SAVEBIN "../build/vdac4/VDAC4VID.WMF",startCode,endCode-startCode
