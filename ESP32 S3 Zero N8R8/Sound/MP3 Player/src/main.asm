; Медиапроигрыватель платы VDAC4.
; Плагин Wild Commander: по Enter на файле MP3, WAV, MOD, XM, S3M, IT, MIDI или музыки
; OPL (VGM, VGZ, DRO, IMF, WLF, MWM) читает его с карты и кадрами отдаёт на ESP32-S3;
; там MP3 декодируется, модуль играет трекер, MIDI — синтезатор по банку звуков,
; OPL — эмулятор микросхем Yamaha, звук уходит в PCM5102 по I2S.
; Проверка ЦАП тонами — отдельная программа tools/audio_test_vdac4.sna.
;
; Работает только с платой VDAC4: у прошивки переходника ZiFi звука нет.
; Канал до ESP — SPI (Network/shared/z80/zifi_spi.asm), сборка всегда с ключом ZIFI_SPI.
;
; Файл задаёт физическую компоновку WMF. Первые 512 байтов содержат заголовок
; формата #11, за ним с границы сектора — список расширений (wc_header.inc), за
; ним с границы сектора — код страницы 0 (логический адрес #8000, там WC его и
; запускает), за ним с границы сектора — код страницы 1 (логический адрес #C000,
; см. Page1_Call).

        DEVICE ZXSPECTRUM128
        INCLUDE "wc_api.inc"

startCode:
        ORG #0000
        INCLUDE "wc_header.inc"
        WC_EXTENSION_LIST                 ; список с отступа 512, код — после него

        DISP #8000
mainStart:
        INCLUDE "player.asm"
        INCLUDE "cover.asm"
        INCLUDE "esp_data.asm"
        INCLUDE "ui.asm"
        INCLUDE "proto.asm"
        INCLUDE "zifi_link.asm"

; Вторая страница плагина (относительная 1) — код, собранный для окна #C000
; (MODULE P1 в конце файла). Окно #C000 WC переключает сам (TXTPR — на текстовый
; экран, файловые функции — на свои страницы) и обратно не ставит, поэтому:
; - в страницу 1 входят только через Page1_Call (метки P1.* снаружи модуля иначе
;   не видны: прямой вызов не соберётся);
; - все вызовы API WC плагина идут через Wc_Api: пока работает код страницы 1
;   (Page1Depth > 0, в том числе в процедурах страницы 0, которые он вызвал),
;   после вызова страница 1 снова подключается к #C000;
; - всё, что читает или пишет WC (тексты, имена, буферы), и всё, что нужно коду
;   страницы 0, лежит в странице 0: страница 1 видна только пока работает её код.

; Вход страницы 0 в процедуру страницы 1 (esp_data.asm):
;         CALL Page1_Call
;         DW P1.адрес
; Это последние команды входа: процедура вернётся прямо к тому, кто вызвал вход.
; Регистры (A, флаги, BC, DE, HL, IX) — как есть туда и обратно, AF' и BC' не
; сохраняются.
Page1_Call:
        ex (sp),hl                       ; HL — слово с адресом, на стеке — HL вызывающего
        push af
        ld a,(hl)
        ld (.call+1),a
        inc hl
        ld a,(hl)
        ld (.call+2),a
        ld a,(Page1Depth)
        inc a
        ld (Page1Depth),a
        pop af
        pop hl                           ; HL — как был, на стеке — возврат из входа
        call Page1_Map
.call:
        call 0                           ; адрес ставится выше (вложенный вызов его
                                         ; меняет уже после этого CALL)
        push af
        ld a,(Page1Depth)
        dec a
        ld (Page1Depth),a
        pop af
        ret

; Подключить страницу 1 к окну #C000 (API 0). Все регистры, кроме AF' и BC'
; (их портит диспетчер WC), — как были.
Page1_Map:
        push af
        ld a,1
        ex af,af'
        xor a                            ; FN_MNGC_PL
        call WC_API
        pop af
        ret

; Вход в API WC для всего плагина (кроме самих переключений окна #C000:
; Page1_Map и Cover_PutPart). Регистры и флаги — как вернул WC.
Wc_Api:
        call WC_API
        push af
        ld a,(Page1Depth)
        or a
        jr nz,.page1
        pop af
        ret
.page1:
        pop af
        jr Page1_Map

Page1Depth:     db 0                    ; сколько вызовов Page1_Call ещё не вернулось

; Переходники API Wild Commander. Номер функции передаётся в A и выполняется
; общим входом Wc_Api. Для функций, где вызывающий код уже использует A как
; параметр, значение временно переносится в альтернативный AF через EX AF,AF'.
; Окна и текст — только при окне на Evo: экран Tang показан — их нет (screen.asm).
WC_PRWOW:
        ld a,(UiHidden)
        or a
        ret nz
        ld a,FN_PRWOW
        jr Wc_Api
WC_RRESB:
        ld a,(UiHidden)
        or a
        ret nz
        ld a,FN_RRESB
        jr Wc_Api
WC_TXTPR:
        ld a,(UiHidden)
        or a
        ret nz
        ld a,FN_TXTPR
        jr Wc_Api
WC_GEDPL:
        ld a,FN_GEDPL
        jr Wc_Api
WC_ESC:
        ld a,FN_ESC
        jr Wc_Api
WC_LOAD512:
        ld a,FN_LOAD512
        jr Wc_Api
WC_STREAM:
        ld a,FN_STREAM
        jr Wc_Api
WC_FENTRY:
        ld a,FN_FENTRY
        jr Wc_Api
WC_GFILE:
        ld a,FN_GFILE
        jr Wc_Api
WC_GDIR:
        ld a,FN_GDIR
        jr Wc_Api
WC_KBSCN:
        ex af,af'
        ld a,FN_KBSCN
        jr Wc_Api

mainEnd:
        ; За концом кода в той же странице (с карты не грузится):
        ; #B000..#B3FF — окно FMADDR (CRAM и SFILE TS-Conf): описатели спрайта
        ;   обложки (cover.asm) пишутся в SFILE с #B200 и ложатся и в ОЗУ, поэтому
        ;   там — только временное: строка окна (UiLine, ui.asm) и за ней тело AUDIO_STATUS
        ;   (UiBlock: собирается перед самой отправкой);
        ; #B400..#B5FF — сектор потока 1 (обложка, cover.asm);
        ; #B600..#B9FF — блок файла: два сектора — тело одного кадра AUDIO_DATA.
        ;   С чётного адреса: оттуда его прямо, без копии, забирает DMA (Proto_Send).
SpriteFile      equ #B200
UiLine          equ SpriteFile
UI_LINE_SIZE    equ 200
UiBlock         equ UiLine+UI_LINE_SIZE
S1Buffer        equ #B400
Mp3Block        equ #B600
Mp3ReadBuffer   equ #D000                ; свободная часть страницы 1, только данные
        ASSERT Mp3ReadBuffer + MP3_READ_SIZE <= #10000
        ASSERT mainEnd <= SpriteFile, "plugin code runs into the FMADDR window"
        ASSERT UiBlock + UI_BLOCK_SIZE <= S1Buffer
        ASSERT S1Buffer + 512 <= Mp3Block
        ASSERT Mp3Block + MP3_BLOCK_SIZE <= #C000
        ENT

        ; Страница 1: свой блок файла с границы сектора, исполняется в окне #C000.
        ALIGN 512
        DISP #C000
page1Start:
        MODULE P1
        INCLUDE "esp_modules.asm"
        INCLUDE "track.asm"
        INCLUDE "fast_media.asm"
        INCLUDE "screen.asm"
        ENDMODULE
page1End:
        ASSERT page1End <= Mp3ReadBuffer, "page 1 code overlaps SD read buffer"
        ENT
endCode:
        SAVEBIN "../build/VDAC4MP3.WMF",startCode,endCode-startCode
