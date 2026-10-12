; Режим обновления прошивки ESP32-S3 платы VDAC4 по локальной сети.
;
; Программа просит прошивку открыть порт обновления (UPDATE_START) и показывает
; его адрес. Образ передаёт ПК: tools\esp_tool.py --net <адрес>. Тем же портом
; ПК забирает журнал наладки (--log файл), USB для этого не нужен. Любая
; клавиша закрывает порт (UPDATE_STOP).
;
; Wi-Fi к этому времени должен быть настроен: прошивка подключается с теми
; настройками, которые сохранила после любого сетевого плагина (zifi.ini).
;
; Сборка: tools\build_esp_info.bat, результат — update_vdac4.sna.

        DEVICE ZXSPECTRUM48
        ORG #8000

UPD_WAIT        equ 12000
UPD_RETRIES     equ 30
; Ответ на UPDATE_START приходит после подключения к Wi-Fi, а оно может занять
; десятки секунд: столько раз по 65536 опросов.
UPD_START_WAITS equ 24
ATTR_P          equ #5C8D
ATTR_T          equ #5C8F

Start:
        ld sp,#FF00
        ei
        call PrepareScreen
        ld a,1
        call SetBright
        ld a,5
        call SetInk
        ld hl,MsgTitle
        call PrintString
        xor a
        call SetBright
        ld a,7
        call SetInk

        ld hl,MsgSearching
        call PrintString
        call ZiFi_Init
        ld hl,MsgNoEsp
        jp c,Fail
        ; Остаток прежнего приёма и состояние разбора кадров — с чистого листа.
        call Proto_ResetRx
        xor a
        ld (ProtoBurstLeft),a
        ld (ProtoErr),a
        call WaitReady
        ld hl,MsgNoReply
        jp c,Fail

        ld hl,MsgOpening
        call PrintString
        ld a,CMD_UPDATE_START
        call Proto_SendEmpty
        ld hl,MsgNoReply
        jp c,Fail
        ld a,RESP_ACK
        ld de,UPD_WAIT
        call Proto_WaitCmd
        ld hl,MsgNoReply
        jp c,Fail
        ld b,UPD_START_WAITS
.wait:
        push bc
        ld a,RESP_UPDATE_START
        ld de,0
        call Proto_WaitCmd
        pop bc
        jr nc,.answer
        djnz .wait
        ld hl,MsgNoReply
        jp Fail
.answer:
        ; [1 — порт открыт][IPv4 x4][порт LE16]
        ld a,(ProtoBuf)
        or a
        ld hl,MsgRefused
        jp z,Fail

        ld a,4
        call SetInk
        ld hl,MsgServer
        call PrintString
        call PrintAddress
        ld a,':'
        call PrintChar
        ld hl,(ProtoBuf+5)
        call PrintDec16
        call PrintNewLine
        ld a,7
        call SetInk
        ld hl,MsgHowTo
        call PrintString
        call PrintAddress
        ld hl,MsgHowToTail
        call PrintString

; Порт открыт: ждём клавишу. Обмена с ESP в это время нет.
WaitKey:
        call AnyKeyPressed
        jr z,WaitKey
        ld a,CMD_UPDATE_STOP
        call Proto_SendEmpty
        jr c,.closed
        ld a,RESP_UPDATE_STOP
        ld de,UPD_WAIT
        call Proto_WaitCmd
.closed:
        ld hl,MsgClosed
        call PrintString
        jr WaitAgain

; HL — причина. Если ESP прислала текст ошибки, он печатается следом.
Fail:
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

; Повтор с начала по любой клавише (после её отпускания и нового нажатия).
WaitAgain:
        call AnyKeyPressed
        jr nz,WaitAgain
.press:
        call AnyKeyPressed
        jr z,.press
.release:
        call AnyKeyPressed
        jr nz,.release
        jp Start

; Несколько раз посылать PING, пока ESP не начнёт отвечать READY.
WaitReady:
        ld b,UPD_RETRIES
.attempt:
        push bc
        ld a,CMD_PING
        call Proto_SendEmpty
        jr c,.miss
        ld a,RESP_READY
        ld de,UPD_WAIT
        call Proto_WaitCmd
.miss:
        pop bc
        ret nc
        djnz .attempt
        scf
        ret

; Адрес из ответа UPDATE_START: четыре числа через точку.
PrintAddress:
        ld hl,ProtoBuf+1
        ld b,4
.part:
        push hl
        push bc
        ld l,(hl)
        ld h,0
        call PrintDec16
        pop bc
        pop hl
        inc hl
        dec b
        ret z
        ld a,'.'
        call PrintChar
        jr .part

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

; Z=0, если нажата хотя бы одна клавиша в любой из восьми полустрок.
AnyKeyPressed:
        ld bc,#FEFE
        ld d,8
.row:
        in a,(c)
        and #1F
        cp #1F
        ret nz
        rlc b
        dec d
        jr nz,.row
        xor a
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

MsgTitle:       db "VDAC4 firmware update",13
                db "---------------------",13,0
MsgSearching:   db "Looking for VDAC4 ESP...",13,0
MsgOpening:     db "Opening the update port...",13,0
MsgServer:      db "Server: ",0
MsgHowTo:       db 13,"On the PC, in the project:",13
                db "python tools\\esp_tool.py",13
                db " --net ",0
MsgHowToTail:   db 13,"Log instead of update: add",13
                db " --log vdac4.log",13,13
                db "ANY KEY - close the port",13,0
MsgClosed:      db 13,"The port is closed.",13
                db "ANY KEY - open it again",13,0
MsgNoEsp:       db "VDAC4 firmware not found",0
MsgNoReply:     db "ESP firmware does not reply",0
MsgRefused:     db "ESP did not open the port",0
MsgEspError:    db "ESP error: ",0
MsgAgain:       db 13,"ANY KEY - try again",13,0

        INCLUDE "zifi_link.asm"
        INCLUDE "proto.asm"

        ASSERT $ < #FF00,"update_vdac4.sna payload overlaps stack"
        SAVESNA "update_vdac4.sna",Start
