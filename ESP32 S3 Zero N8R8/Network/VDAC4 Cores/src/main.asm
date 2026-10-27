; VDAC4 Cores для ZX Evolution / TS-Config — ядра ПЛИС Tang Nano 20K платы VDAC4 с microSD самой
; Tang (ZX Next и другие, которых нет во flash Tang; docs/next-core.md проекта VDAC4).
;
; Плагин спрашивает у ESP список ядер (\CORES\ на SD Tang), даёт выбрать одно и просит ESP
; запустить его слой (ИМЯ.ELF рядом с ядром или общий слой прошивки). Слой грузит ядро в ПЛИС
; Tang из кэша BL616; плагин раз в кадр шлёт ему очередь PS/2 клавиатуры Evo, мышь и джойстик
; Kempston. F12 — назад в Wild Commander (Tang — снова TS-Config). Канал до ESP — SPI платы
; VDAC4 (zifi_link.asm → zifi_spi.asm, сборка с -DZIFI_SPI).
;
; Файл задаёт физическую компоновку WMF: заголовок #0A (512 байтов), код — с логическим
; адресом #8000, одна страница.

        DEVICE ZXSPECTRUM128
        INCLUDE "wc_api.inc"

startCode:
        ORG #0000
        INCLUDE "wc_header.inc"

        ALIGN 512                         ; код начинается после сектора заголовка
        DISP #8000
mainStart:
        INCLUDE "cores.asm"
        INCLUDE "../../shared/weather/rtc.asm"   ; часы Gluk Evo (TIME для часов ядра Next)
        INCLUDE "ui.asm"
        INCLUDE "nes.asm"
        INCLUDE "evo_menu.asm"
        INCLUDE "evo_sd.asm"                     ; карта Evo — SD1 ZX Next
        INCLUDE "nes_save.asm"
        INCLUDE "nes_browser.asm"
        INCLUDE "proto.asm"
        INCLUDE "zifi_link.asm"

; Переходники API Wild Commander. Номер функции — в A, параметр, который функция ждёт в A, —
; в A' (диспетчер WC меняет AF и AF' местами).
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
WC_KEY_ENTER:
        ld a,FN_ENTER
        jp WC_API
WC_INT_PL:
        ex af,af'
        ld a,FN_INT_PL
        jp WC_API
WC_GVMOD:
        ex af,af'
        ld a,FN_GVMOD
        jp WC_API
WC_MNGV_PL:
        ex af,af'
        ld a,FN_MNGV_PL
        jp WC_API

mainEnd:
        ASSERT mainEnd <= #C000, "plugin code exceeds the #8000 page"
        ENT
        ALIGN 512                         ; WC читает страницу целыми секторами
endCode:
        SAVEBIN "../build/vdac4/VDAC4COR.WMF",startCode,endCode-startCode
