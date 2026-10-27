; Окно плагина: меню ядер, затем — пока ядро грузится, и для сообщения об ошибке; когда ядро
; работает, окна нет (картинка ядра — на HDMI Tang). Текст английский: экран Wild Commander
; использует свою однобайтовую кодировку.

UI_STATUS_WIDTH equ 44
MENU_LINES      equ 8

Ui_Open:
        ld ix,NxWindow
        call WC_PRWOW
        ld a,1
        ld (NxWindowOpen),a
        jr Ui_Draw

Ui_Close:
        ld a,(NxWindowOpen)
        or a
        ret z
        xor a
        ld (NxWindowOpen),a
        ld ix,NxWindow
        jp WC_RRESB

Ui_Draw:
        ld a,(NxWindowOpen)
        or a
        ret z
        ld ix,NxWindow
        ld hl,UiText
        ld de,#0101
        jp WC_TXTPR

; Строка HL — в поле состояния (Ui_Status — и окно заново).
Ui_Status:
        call Ui_SetStatus
        jr Ui_Draw
Ui_SetStatus:
        ld de,UiStatusField
        ld b,UI_STATUS_WIDTH
        jp Ui_CopyField

; Строка HL — в поле подсказки (Ui_Hint — и окно заново).
Ui_Hint:
        call Ui_SetHint
        jr Ui_Draw
Ui_SetHint:
        ld de,UiHintField
        ld b,UI_STATUS_WIDTH
        jp Ui_CopyField

; Строки меню: MENU_LINES ядер с MenuTop, у выбранного — «>»; имя, размер в КБ, «own ESP layer»,
; если у ядра свой ИМЯ.ELF.
Ui_MakeList:
        ld de,UiListText
        ld a,(MenuTop)
        ld (UiIdx),a
        ld b,MENU_LINES
.line:
        push bc
        push de                          ; начало строки
        ld h,d
        ld l,e
        ld b,UI_STATUS_WIDTH
.clear:
        ld (hl),' '
        inc hl
        djnz .clear
        ld a,(UiIdx)
        ld hl,CrCount
        cp (hl)
        jr nc,.next                      ; строк больше, чем ядер
        ld hl,MenuSel
        cp (hl)
        jr nz,.name
        ld a,'>'
        ld (de),a
.name:
        ld a,(UiIdx)
        push de
        call Cr_Entry                    ; HL — запись
        pop de
        push hl
        inc de
        inc de                           ; имя — с колонки 2
        ld b,15
.copy:
        ld a,(hl)
        or a
        jr z,.size
        ld (de),a
        inc de
        inc hl
        djnz .copy
.size:
        pop hl                           ; запись
        pop de                           ; начало строки
        push de
        push hl
        ld bc,18
        ex de,hl
        add hl,bc
        ex de,hl                         ; DE — колонка 18
        ld bc,16+1                       ; КБ = длина >> 10: байты 1 и 2 длины LE32
        add hl,bc
        ld a,(hl)
        inc hl
        ld h,(hl)
        ld l,a
        srl h
        rr l
        srl h
        rr l
        call Ui_Dec5
        ld hl,UiKb
        call Ui_Append
        pop hl                           ; запись
        ld bc,ENTRY_FLAGS
        add hl,bc
        bit 0,(hl)
        jr z,.next
        ld hl,UiOwnLayer
        call Ui_Append
.next:
        pop de
        ld hl,UI_STATUS_WIDTH+1
        add hl,de
        ex de,hl                         ; следующая строка
        ld hl,UiIdx
        inc (hl)
        pop bc
        djnz .line
        ret

; HL — 5 знаков в DE (ведущие нули — пробелами), DE — за ними.
Ui_Dec5:
        ld a,' '
        ld (UiLead),a
        ld bc,-10000
        call .digit
        ld bc,-1000
        call .digit
        ld bc,-100
        call .digit
        ld bc,-10
        call .digit
        ld a,'0'                         ; последняя цифра — всегда
        ld (UiLead),a
        ld bc,-1
.digit:
        ld a,'0'-1
.sub:
        inc a
        add hl,bc
        jr c,.sub
        sbc hl,bc                        ; вернуть лишнее вычитание (CF=0)
        cp '0'
        jr nz,.put
        ld a,(UiLead)
        jr .store
.put:
        push af
        ld a,'0'
        ld (UiLead),a
        pop af
.store:
        ld (de),a
        inc de
        ret

; Строку HL без нуля — в DE, DE — за ней.
Ui_Append:
        ld a,(hl)
        or a
        ret z
        ld (de),a
        inc de
        inc hl
        jr Ui_Append

; Скопировать строку HL с нулём в поле DE шириной B, остаток — пробелы; управляющие коды
; заменяются точкой.
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

NxWindow:
        db #81
        db 0
        db 13,4
        db 54,17
        db #17
        db 0
        dw 0
        db 0,0
        dw UiHeader
        dw 0
        dw 0

NxWindowOpen:   db 0
UiIdx:          db 0
UiLead:         db ' '

UiHeader:
        db #0E,9," VDAC4 cores ",0

UiText:
        db #0E
UiTitle:       ds 44,' '
        db #0D,#0D
UiListText:
        REPT MENU_LINES
        ds UI_STATUS_WIDTH,' '
        db #0D
        ENDR
        db #0D
UiStatusField:
        ds UI_STATUS_WIDTH,' '
        db #0D,#0D
UiHintField:
        ds UI_STATUS_WIDTH,' '
        db 0

UiEmpty:        db 0
UiKb:           db " KB",0
UiOwnLayer:     db "  own ESP layer",0
UiLoadPre:      db "Loading ",0
UiLoadPost:     db " into the Tang...",0
UiStLoad:       ds 44                   ; «Loading » + имя до 15 + « into the Tang...» + 0
UiStConnect:    db "Connecting to the ESP...",0
UiStList:       db "Reading the Tang SD card...",0
UiStChoose:     db "Choose a core",0
UiStStart:      db "Starting the ESP layer of the core...",0
UiStEsc:        db "ESC - return to Wild Commander",0
UiHintMenu:     db "Up/Down, Enter - run, ESC - back",0
UiHintLoad:     db "F12 - back to Wild Commander",0
UiErrNoEsp:     db "ERROR: no ESP on the SPI",0
UiErrNoAnswer:  db "ERROR: the ESP does not answer (0.1.84+)",0
UiErrNoCores:   db "ERROR: no cores in \\CORES on the Tang SD",0
UiErrApp:       db "ERROR: the ESP layer did not start",0
UiErrTimeout:   db "ERROR: the core did not start in 180 s",0
UiErrCore:      db "ERROR: the core did not start",0
UiErrLost:      db "ERROR: the ESP stopped answering",0
UiErrEsp:       db "ERR: "
NxErrField:     ds UI_STATUS_WIDTH-5,' '
                db 0

UiCoresTitle: db "FPGA cores from the Tang SD, /CORES",0
NesEvoTitle: db "NES cartridge from the Evo SD",0
