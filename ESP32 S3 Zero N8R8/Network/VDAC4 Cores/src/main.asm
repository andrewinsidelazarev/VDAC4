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
; адресом #8000 (страница 0); с 0.20 за ним — страница 1 с логическим адресом #C000: таблицы CRC16
; и (не в файле) буфер порции DMA. Страницу 1 к окну #C000 подключает Page1_Map.

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
        INCLUDE "screen.asm"                     ; экран на HDMI Tang (COREMENU.ELF)
        INCLUDE "nes.asm"
        INCLUDE "evo_menu.asm"
        INCLUDE "evo_sd.asm"                     ; карта Evo — SD1 ZX Next
        INCLUDE "nes_save.asm"
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
WC_ANYK:                                 ; NZ — нажата любая клавиша (0.22, окно отказа)
        ld a,FN_ANYK
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

; 0.20: подключить страницу 1 плагина к окну #C000 (API 0, A' = 1): таблицы CRC16 (Nes_Crc16) и
; буфер порции DMA (Scr_SendRaw). Окно WC переключает сам (TXTPR, файловые функции) и обратно не
; ставит — подключать перед каждым обращением. Все регистры, кроме AF' и BC', — как были.
Page1_Map:
        push af
        ld a,1
        ex af,af'
        xor a                            ; FN_MNGC_PL
        call WC_API
        pop af
        ret

mainEnd:
        ASSERT mainEnd <= #C000, "plugin code exceeds the #8000 page"
        ENT
        ALIGN 512                         ; WC читает страницу целыми секторами

        ; Страница 1 (0.20): таблицы CRC16 с #C000 (выровнены на 256), буфер порции DMA — RAW_BUF
        DISP #C000
page1Start:
        INCLUDE "crc16_tab.inc"
        ; 0.22: окно отказа при включённом VDAC4 VFS (Ui_VfsRefuse копирует его в SdData)
VfsRefuseWin:
        db #81                         ; тень и рамка стиля 1
        db 0
        db #FF,#FF                     ; X,Y: центр текущего режима
        db 40,9                        ; ширина и высота: 7 строк текста
        db #2F                         ; окно ошибки: ярко-белые символы по красному
        db 0
        dw 0
        db 0,0
        dw UiHeader
        dw 0
        dw 0
VfsRefuseText:                         ; #0E — строка по центру окна
        db #0E,"Cores cannot start: VDAC4 VFS is on.",#0D
        db #0E,"SD2 is the Tang microSD or the VFS",#0D
        db #0E,"volume, both served by the ESP.",#0D,#0D
        db #0E,"Turn it off: F10 - VDAC4 VFS,",#0D
        db #0E,"then start VDAC4 Cores again.",#0D
        db #0E,"Any key - return to WC",0
VfsRefuseEnd:
page1End:
        ASSERT page1End <= RAW_BUF, "page 1 tables overlap the DMA portion buffer"
        ENT
        ALIGN 512
endCode:
        SAVEBIN "../build/vdac4/VDAC4COR.WMF",startCode,endCode-startCode
