; Окно плагина — только для сообщения об ошибке; в обычной работе плагин показывает
; экранное меню Tang поверх картинки HDMI и окна не открывает. Текст английский: экран
; Wild Commander использует свою однобайтовую кодировку.

UI_STATUS_WIDTH equ 44

Ui_Open:
        ld ix,VideoWindow
        call WC_PRWOW
        ld a,1
        ld (VideoWindowOpen),a
        ret

Ui_Draw:
        ld ix,VideoWindow
        ld hl,UiText
        ld de,#0101
        jp WC_TXTPR

Ui_SetStatus:
        ld de,UiStatusField
        ld b,UI_STATUS_WIDTH
        jp Ui_CopyField

; Версия ядра Tang "M.m" — в текст ошибки о старом ядре
Ui_SetCore:
        ld a,(TangMajor)
        and #0F
        add a,'0'
        ld (UiCoreField),a
        ld a,(TangMinor)
        and #0F
        add a,'0'
        ld (UiCoreField+2),a
        ret

; Скопировать строку HL с нулём в поле DE шириной B, остаток — пробелы;
; управляющие коды заменяются точкой.
Ui_CopyField:
        push hl
        push de
        push bc
        ld a,' '
.clear:
        ld (de),a
        inc de
        djnz .clear
        pop bc
        pop de
        pop hl
.copy:
        ld a,b
        or a
        ret z
        ld a,(hl)
        or a
        ret z
        cp 32
        jr nc,.printable
        ld a,'.'
.printable:
        ld (de),a
        inc de
        inc hl
        dec b
        jr .copy

VideoWindow:
        db #81
        db 0
        db 13,9
        db 54,8
        db #17
        db 0
        dw 0
        db 0,0
        dw UiHeader
        dw 0
        dw 0

UiHeader:
        db #0E,9," VDAC4 video settings ",0

UiText:
        db #0E,"Tang on-screen menu from the Evo keyboard",#0D,#0D
        db "Status: "
UiStatusField:
        ds UI_STATUS_WIDTH,' '
        db #0D,#0D
        db "ESC - return to Wild Commander",0

UiErrNoEsp:     db "ERROR: no ESP on the SPI",0
UiErrNoLink:    db "ERROR: ESP has no LINK to the Tang",0
UiErrNotVdc4:   db "ERROR: the Tang core is not VDC4",0
UiErrLost:      db "ERROR: no answer from the Tang",0
UiErrOldCore:   db "ERROR: Tang core "
UiCoreField:    db "?.? is older than 1.3",0
