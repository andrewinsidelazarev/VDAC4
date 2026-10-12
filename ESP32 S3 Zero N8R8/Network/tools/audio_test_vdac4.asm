; Проверка звукового тракта платы VDAC4: тоны 1 кГц в каждый канал ЦАП и тест
; линий BCK/LRCK/DIN. Раньше это жило в плагине проигрывателя (F10); теперь
; проигрыватель только играет, а проверка — отдельная программа, без WC.
;
; Команды прошивки: AUDIO_TONE, AUDIO_PROBE, AUDIO_STATUS, AUDIO_VOLUME
; (Docs/VDAC4.md, «Команды звука»). Тест линий ESP делает только в тишине.
;
; Сборка: tools\build_esp_info.bat, результат — audio_test_vdac4.sna.

        DEVICE ZXSPECTRUM48
        ORG #8000

AT_WAIT         equ 12000
AT_RETRIES      equ 30
AT_VOLUME_STEP  equ 16
ATTR_P          equ #5C8D
ATTR_T          equ #5C8F
LAST_K          equ #5C08               ; код последней клавиши (опрос ПЗУ в прерывании)
FLAGS           equ #5C3B               ; бит 5 — нажата новая клавиша

TONE_LEFT       equ 1
TONE_RIGHT      equ 2
TONE_BOTH       equ 3

ST_STATE        equ 0                   ; 4 — I2S не запущен
ST_VOLUME       equ 23
AUDIO_OFFLINE   equ 4

Start:
        ld sp,#FF00
        ei
        call PrepareScreen
        call PrintTitle
        ld hl,MsgSearching
        call PrintString
        call ZiFi_Init
        ld hl,MsgNoEsp
        jp c,Fail
        ; Остаток прежнего приёма и состояние разбора кадров — с чистого листа.
        call Proto_ResetRx
        ld hl,0
        ld (ProtoBurstLeft),hl
        xor a
        ld (ProtoErr),a
        call WaitReady
        ld hl,MsgNoReply
        jp c,Fail
        call ReadStatus
        ld hl,MsgNoReply
        jp c,Fail
        ld a,(Status+ST_STATE)
        cp AUDIO_OFFLINE
        ld hl,MsgOffline
        jp z,Fail
        ld hl,MsgReadyState
        ld (StateText),hl

Menu:
        call PrepareScreen
        call PrintTitle
        ld hl,MsgMenu
        call PrintString
        ld hl,MsgVolume
        call PrintString
        ld a,(Status+ST_VOLUME)
        ld l,a
        ld h,0
        call PrintDec16
        call PrintNewLine
        ld hl,MsgState
        call PrintString
        ld hl,(StateText)
        call PrintString
        call PrintNewLine

; Клавиша: опрос клавиатуры делает ПЗУ в прерывании.
.key:
        call GetKey
        or #20                          ; строчная
        ld c,TONE_LEFT
        ld hl,MsgToneLeft
        cp 'l'
        jr z,Tone
        ld c,TONE_RIGHT
        ld hl,MsgToneRight
        cp 'r'
        jr z,Tone
        ld c,TONE_BOTH
        ld hl,MsgToneBoth
        cp 'b'
        jr z,Tone
        cp 'q'
        ld c,AT_VOLUME_STEP
        jr z,Volume
        cp 'a'
        ld c,-AT_VOLUME_STEP
        jr z,Volume
        cp 't'
        jp z,Lines
        jr .key

; C — каналы, HL — что показать, если ESP поставила тон в очередь звука.
Tone:
        ld (StateText),hl
        ld a,c
        ld (ToneBody),a
        ld a,CMD_AUDIO_TONE
        ld hl,ToneBody
        ld bc,ToneBodyEnd-ToneBody
        call Proto_Send
        jp c,NoAnswer
        ld a,RESP_AUDIO_TONE
        ld de,AT_WAIT
        call Proto_WaitCmd
        jp c,NoAnswer
        ; 1 — тон поставлен в очередь звука, 0 — отказ (например, идёт поток).
        ld a,(ProtoBuf)
        or a
        jp nz,Menu
        ld hl,MsgToneRefused
        ld (StateText),hl
        jp Menu

; C — шаг громкости со знаком.
Volume:
        ld a,(Status+ST_VOLUME)
        bit 7,c
        jr nz,.down
        add a,c
        jr nc,.set
        ld a,255
        jr .set
.down:
        add a,c
        jr c,.set
        xor a
.set:
        ld (Status+ST_VOLUME),a
        ld (VolumeBody),a
        ld a,CMD_AUDIO_VOLUME
        ld hl,VolumeBody
        ld bc,1
        call Proto_Send
        jp c,NoAnswer
        ld a,RESP_AUDIO_VOLUME
        ld de,AT_WAIT
        call Proto_WaitCmd
        jp c,NoAnswer
        jp Menu

; Тест линий ЦАП: ESP меряет напряжение на своих выводах BCK, LRCK и DIN при
; разных уровнях на двух других и отдаёт 24 числа в единицах 20 мВ.
Lines:
        ld a,CMD_AUDIO_PROBE
        call Proto_SendEmpty
        jp c,NoAnswer
        ld a,RESP_AUDIO_PROBE
        ld de,AT_WAIT
        call Proto_WaitCmd
        jp c,NoAnswer
        call PrepareScreen
        call PrintTitle
        ld a,(ProtoBuf)
        or a
        jr nz,.table
        ld hl,MsgLinesBusy
        call PrintString
        jr .done
.table:
        ld hl,MsgLinesHead
        call PrintString
        ld hl,ProtoBuf+1
        ld de,LineNames
        ld b,6                          ; три линии, у каждой две строки
.row:
        push bc
        ex de,hl
        call PrintString                ; имя строки
        inc hl
        ex de,hl
        ld b,4
.value:
        ld a,(hl)
        inc hl
        call PrintVolts
        djnz .value
        call PrintNewLine
        pop bc
        djnz .row
        ld hl,MsgLinesNote
        call PrintString
.done:
        ld hl,MsgLinesKeys
        call PrintString
.key:
        call GetKey
        or #20
        cp 't'
        jr z,Lines
        jp Menu

; ESP не ответила. Если она успела прислать причину (#EE), показать её.
NoAnswer:
        ld hl,MsgNoReply
; HL — причина. Повтор с начала по любой клавише.
Fail:
        push hl
        call PrepareScreen
        call PrintTitle
        pop hl
        ld a,2
        call SetInk
        call PrintString
        call PrintNewLine
        ld a,(ProtoErr)
        or a
        jr z,.plain
        ld hl,MsgEspError
        call PrintString
        ld hl,ProtoErrText
        call PrintString
        call PrintNewLine
.plain:
        ld a,7
        call SetInk
        ld hl,MsgAgain
        call PrintString
        call GetKey
        jp Start

; Запросить запись состояния звука (громкость, состояние I2S).
; Выход: CF=1 — ESP не ответила.
ReadStatus:
        ld a,CMD_AUDIO_STATUS
        call Proto_SendEmpty
        ret c
        ld a,RESP_AUDIO_STATUS
        ld de,AT_WAIT
        call Proto_WaitCmd
        ret c
        ld hl,ProtoBuf
        ld de,Status
        ld bc,ST_VOLUME+1
        ldir
        or a
        ret

; Несколько раз посылать PING, пока ESP не начнёт отвечать READY.
WaitReady:
        ld b,AT_RETRIES
.attempt:
        push bc
        ld a,CMD_PING
        call Proto_SendEmpty
        jr c,.miss
        ld a,RESP_READY
        ld de,AT_WAIT
        call Proto_WaitCmd
.miss:
        pop bc
        ret nc
        djnz .attempt
        scf
        ret

; Ждать новую клавишу. Выход: A — её код.
GetKey:
        ld hl,FLAGS
        res 5,(hl)
.wait:
        ei
        halt
        bit 5,(hl)
        jr z,.wait
        res 5,(hl)
        ld a,(LAST_K)
        ret

; A — напряжение в единицах 20 мВ: печать " d.d".
PrintVolts:
        push bc
        push hl
        ld c,0
.fifth:
        sub 5                           ; 100 мВ — пять единиц
        jr c,.tenths
        inc c
        jr .fifth
.tenths:
        ld a,c                          ; десятые доли вольта
        ld c,'0'-1
.units:
        inc c
        sub 10
        jr nc,.units
        add a,10+'0'
        ld b,a
        ld a,' '
        call PrintChar
        ld a,c
        call PrintChar
        ld a,'.'
        call PrintChar
        ld a,b
        call PrintChar
        pop hl
        pop bc
        ret

; Напечатать HL десятичным числом без ведущих нулей.
PrintDec16:
        ld e,0                          ; ещё ни одной цифры
        ld bc,-10000
        call .digit
        ld bc,-1000
        call .digit
        ld bc,-100
        call .digit
        ld bc,-10
        call .digit
        ld a,l
        add a,'0'
        jp PrintChar
.digit:
        ld a,'0'-1
.count:
        inc a
        add hl,bc
        jr c,.count
        sbc hl,bc                       ; вернуть лишнее вычитание
        cp '0'
        jr nz,.print
        inc e
        dec e
        ret z                           ; ведущий ноль
.print:
        ld e,1
        jp PrintChar

PrintTitle:
        ld a,1
        call SetBright
        ld a,5
        call SetInk
        ld hl,MsgTitle
        call PrintString
        xor a
        call SetBright
        ld a,7
        jp SetInk

PrepareScreen:
        ld a,2
        call #1601                      ; верхнее окно экрана
        ld a,#07                        ; белый текст на чёрном фоне
        ld (ATTR_P),a
        ld (ATTR_T),a
        call #0DAF                      ; полный CLS и сброс позиции печати
        xor a
        out (#FE),a                     ; чёрная рамка
        ret

SetInk:
        push af
        ld a,16
        call PrintChar
        pop af
        jp PrintChar

SetBright:
        push af
        ld a,19
        call PrintChar
        pop af
        jp PrintChar

PrintNewLine:
        ld a,13
        jp PrintChar

PrintString:
        ld a,(hl)
        or a
        ret z
        call PrintChar
        inc hl
        jr PrintString

PrintChar:
        push bc
        push de
        push hl
        rst #10
        pop hl
        pop de
        pop bc
        ret

Status:         ds ST_VOLUME+1
StateText:      dw MsgReadyState
VolumeBody:     db 0
; Тело AUDIO_TONE: [каналы][частота LE16][длительность, мс LE16][уровень].
ToneBody:       db TONE_BOTH
                dw 1000
                dw 700
                db 160
ToneBodyEnd:

MsgTitle:       db "VDAC4 audio test",13
                db "----------------",13,0
MsgSearching:   db "Looking for VDAC4 ESP...",13,0
MsgMenu:        db "L - tone 1 kHz, LEFT channel",13
                db "R - tone 1 kHz, RIGHT channel",13
                db "B - tone 1 kHz, both channels",13
                db "Q/A - volume up/down",13
                db "T - DAC lines test",13,13,0
MsgVolume:      db "Volume: ",0
MsgState:       db "State:  ",0
MsgReadyState:  db "ready",0
MsgToneLeft:    db "1 kHz tone, LEFT",0
MsgToneRight:   db "1 kHz tone, RIGHT",0
MsgToneBoth:    db "1 kHz tone, both",0
MsgToneRefused: db "ESP refused the tone",0
MsgLinesHead:   db "DAC lines, volts at ESP pins",13
                db "others at:  00  01  10  11",13,0
; Имена строк таблицы, каждое с нулём.
LineNames:      db "BCK  drive",0
                db "BCK  pull ",0
                db "LRCK drive",0
                db "LRCK pull ",0
                db "DIN  drive",0
                db "DIN  pull ",0
MsgLinesNote:   db 13,"others: BCK - LRCK,DIN",13
                db "LRCK - BCK,DIN; DIN - BCK,LRCK",13
                db "A free line: 3.0-3.3 in every",13
                db "column of both rows",13,0
MsgLinesBusy:   db "Sound is playing:",13
                db "no measurement",13,0
MsgLinesKeys:   db 13,"T - again, other key - menu",13,0
MsgNoEsp:       db "VDAC4 firmware not found",0
MsgNoReply:     db "ESP firmware does not reply",0
MsgOffline:     db "Sound is off: I2S not started",0
MsgEspError:    db "ESP error: ",0
MsgAgain:       db 13,"ANY KEY - try again",13,0

        INCLUDE "zifi_link.asm"
        INCLUDE "proto.asm"

        ASSERT $ < #FF00,"audio_test_vdac4.sna payload overlaps stack"
        SAVESNA "audio_test_vdac4.sna",Start
