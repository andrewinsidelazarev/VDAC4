; Проверка обмена с ESP32-S3 платы VDAC4 через DMA (наладка).
;
; Кроме общего буфера, у ведомого SPI есть передача блоков через DMA: команда
; #04 — ведущий читает блок, #03 — пишет, #08 и #07 завершают их. Evo при этом
; гоняет байты своим DMA (порты #19AF..#28AF), не тратя такты Z80 на каждый
; байт. Порядок действий взят из TS-Labs (esp_recv_dma и esp_send_dma в
; ft812sdk/lib/esp32/esp32.c).
;
; Программа проверяет обе стороны на известном узоре:
;   1. ESP выставляет PROBE_LEN байтов узора, Evo забирает их DMA в память и
;      сверяет;
;   2. Evo кладёт узор в память и отдаёт его DMA, сверяет ESP.
; Команды прошивке — LINK_PROBE (#3E), см. SpiLink::dmaProbe.
;
; Данные идут в окно #C000, для которого программа сама выбирает страницу ОЗУ
; (PROBE_PAGE): DMA адресует память номером страницы, а прочитать, какая
; страница сейчас в окне, нельзя. Поэтому стек и код держатся ниже #C000.
;
; Сборка: tools\build_esp_info.bat, результат — dma_probe_vdac4.sna.

        DEVICE ZXSPECTRUM48
        ORG #8000

PROBE_LEN       equ 1024                ; кратно 512: блок DMA — 256 слов
PROBE_PAGE      equ 0
PROBE_BUF       equ #C000
PROBE_WAIT      equ 12000
PROBE_RETRIES   equ 30
PROBE_SEED_IN   equ #A7                 ; узор, который выставляет ESP
PROBE_SEED_OUT  equ #3D                 ; узор, который отдаёт Evo

CMD_LINK_PROBE  equ #3E
RESP_LINK_PROBE equ #BE
PROBE_SEND      equ 1
PROBE_RECEIVE   equ 2
PROBE_FINISH    equ 3

TS_PAGE3        equ #13AF
TS_DMASADDRL    equ #1AAF
TS_DMASADDRH    equ #1BAF
TS_DMASADDRX    equ #1CAF
TS_DMADADDRL    equ #1DAF
TS_DMADADDRH    equ #1EAF
TS_DMADADDRX    equ #1FAF
TS_DMALEN       equ #26AF
TS_DMACTR       equ #27AF
TS_DMANUM       equ #28AF
DMA_SPI_RAM     equ #02
DMA_RAM_SPI     equ #82
SPI_CMD_WRDMA   equ #03
SPI_CMD_RDDMA   equ #04
SPI_CMD_WR_DONE equ #07
SPI_CMD_RD_DONE equ #08

ATTR_P          equ #5C8D
ATTR_T          equ #5C8F

Start:
        ld sp,#BF00
        ei
        ld bc,TS_PAGE3
        ld a,PROBE_PAGE
        out (c),a
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

        call ZiFi_Init
        ld hl,MsgNoEsp
        jp c,Fail
        call Proto_ResetRx
        xor a
        ld (ProtoBurstLeft),a
        ld (ProtoErr),a
        call WaitReady
        ld hl,MsgNoReply
        jp c,Fail

; --- 1. ESP -> Evo ---------------------------------------------------------------
        ld hl,MsgRead
        call PrintString
        ; Память заранее заполняется не узором: видно, пришло ли что-нибудь.
        ld hl,PROBE_BUF
        ld de,PROBE_BUF+1
        ld bc,PROBE_LEN-1
        ld (hl),#EE
        ldir
        ld a,PROBE_SEND
        ld e,PROBE_SEED_IN
        call ProbeCommand
        ld hl,MsgNoReply
        jp c,Fail
        ld a,(ProtoBuf)
        dec a
        ld hl,MsgRefused
        jp nz,Fail
        call DmaRead
        push af
        ; DMA пишет в память мимо кэша процессора: 512 чтений подряд вытесняют
        ; из него устаревшие строки (приём из драйвера TS-Labs).
        ld hl,#7E00
        ld de,#7E00
        ld bc,#0200
        ldir
        pop af
        ld hl,MsgDmaStuck
        call nz,PrintLine
        ld a,PROBE_FINISH
        ld e,0
        call ProbeCommand
        ld hl,MsgNoReply
        jp c,Fail
        ld a,(ProtoBuf)
        dec a
        ld hl,MsgNoEnd
        call nz,PrintLine               ; ESP не увидела конца транзакции
        call CheckBuffer                ; DE — несовпавших, HL — первый
        call PrintVerdict
        ld hl,PROBE_BUF
        call PrintHead

; --- 2. Evo -> ESP ---------------------------------------------------------------
        ld hl,MsgWrite
        call PrintString
        ld a,PROBE_SEED_OUT
        call FillBuffer
        ld a,PROBE_RECEIVE
        ld e,PROBE_SEED_OUT
        call ProbeCommand
        ld hl,MsgNoReply
        jp c,Fail
        ld a,(ProtoBuf)
        dec a
        ld hl,MsgRefused
        jp nz,Fail
        call DmaWrite
        ld hl,MsgDmaStuck
        call nz,PrintLine
        ld a,PROBE_FINISH
        ld e,0
        call ProbeCommand
        ld hl,MsgNoReply
        jp c,Fail
        ld a,(ProtoBuf)
        dec a
        jr z,.finished
        ld hl,MsgNoEnd
        call PrintLine
        jr Done
.finished:
        ; [итог][действие][принято LE16][несовпавших LE16][первый LE16]
        ld hl,MsgGot
        call PrintString
        ld hl,(ProtoBuf+2)
        call PrintDec16
        call PrintNewLine
        ld de,(ProtoBuf+4)
        ld hl,(ProtoBuf+6)
        call PrintVerdict

Done:
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

; HL — причина. Если ESP прислала текст ошибки, он печатается следом.
Fail:
        ld a,2
        call SetInk
        call PrintLine
        ld a,(ProtoErr)
        or a
        jr z,.plain
        ld hl,MsgEspError
        call PrintString
        ld hl,ProtoErrText
        call PrintLine
.plain:
        ld a,7
        call SetInk
        jr Done

; Команда проверки: A — действие, E — зерно узора.
; Выход: CF=1 — ответа нет; иначе ответ лежит в ProtoBuf.
ProbeCommand:
        ld (ProbeBody),a
        ld a,e
        ld (ProbeBody+3),a
        ld a,CMD_LINK_PROBE
        ld hl,ProbeBody
        ld bc,4
        call Proto_Send
        ret c
        ld a,RESP_LINK_PROBE
        ld de,PROBE_WAIT
        jp Proto_WaitCmd
ProbeBody:
        db 0
        dw PROBE_LEN
        db 0

; Принять PROBE_LEN байтов от ESP через DMA в PROBE_BUF.
; Выход: Z=1 — DMA закончил, Z=0 — не дождались.
DmaRead:
        xor a
        ld bc,TS_DMADADDRL
        out (c),a
        ld bc,TS_DMADADDRH
        out (c),a
        ld bc,TS_DMADADDRX
        ld a,PROBE_PAGE
        out (c),a
        call DmaLength
        ld a,SPI_SELECT_ESP
        out (ZIFI_SPI_CFG),a
        ld a,SPI_CMD_RDDMA
        out (ZIFI_SPI_DATA),a
        in a,(ZIFI_SPI_DATA)            ; пошёл байт адреса
        in a,(ZIFI_SPI_DATA)            ; пошёл пустой байт
        in a,(ZIFI_SPI_DATA)            ; пошёл первый байт данных: DMA берёт уже принятое
        ld a,DMA_SPI_RAM
        call DmaRun
        push af
        ld a,SPI_CMD_RD_DONE
        call SpiDone
        pop af
        ret

; Отдать PROBE_LEN байтов из PROBE_BUF в ESP через DMA.
; Выход: Z=1 — DMA закончил, Z=0 — не дождались.
DmaWrite:
        xor a
        ld bc,TS_DMASADDRL
        out (c),a
        ld bc,TS_DMASADDRH
        out (c),a
        ld bc,TS_DMASADDRX
        ld a,PROBE_PAGE
        out (c),a
        call DmaLength
        ld a,SPI_SELECT_ESP
        out (ZIFI_SPI_CFG),a
        ld a,SPI_CMD_WRDMA
        out (ZIFI_SPI_DATA),a
        in a,(ZIFI_SPI_DATA)            ; байт адреса
        in a,(ZIFI_SPI_DATA)            ; пустой байт
        ld a,DMA_RAM_SPI
        call DmaRun
        push af
        ld a,SPI_CMD_WR_DONE
        call SpiDone
        pop af
        ret

; Блок DMA — 256 слов (512 байтов), блоков — PROBE_LEN/512.
DmaLength:
        ld bc,TS_DMALEN
        ld a,#FF
        out (c),a
        ld bc,TS_DMANUM
        ld a,PROBE_LEN/512-1
        out (c),a
        ret

; Запустить DMA (A — режим), дождаться конца и снять выбор ESP.
; Выход: Z=1 — закончил, Z=0 — не дождались.
DmaRun:
        ld bc,TS_DMACTR
        out (c),a
        ld de,0
.wait:
        in a,(c)
        and #80
        jr z,.done
        dec de
        ld a,d
        or e
        jr nz,.wait
        inc a                           ; Z=0: DMA всё ещё занят
.done:
        ld a,SPI_SELECT_NONE
        out (ZIFI_SPI_CFG),a            ; OUT флагов не меняет
        ret

; Отдельная транзакция из одной команды A: конец чтения (#08) или записи (#07).
SpiDone:
        ld b,a
        ld a,SPI_SELECT_ESP
        out (ZIFI_SPI_CFG),a
        ld a,b
        out (ZIFI_SPI_DATA),a
        in a,(ZIFI_SPI_DATA)
        ld a,SPI_SELECT_NONE
        out (ZIFI_SPI_CFG),a
        ret

; Байт узора: младший байт номера + 3 * старший + зерно (как dmaProbeByte).
; Заполнить PROBE_BUF узором с зерном A.
FillBuffer:
        ld (.seed+1),a
        ld hl,PROBE_BUF
        ld de,0
.next:
        ld a,d
        add a,a
        add a,d
        add a,e
.seed:
        add a,0
        ld (hl),a
        inc hl
        inc de
        ld a,d
        cp PROBE_LEN/256
        jr nz,.next
        ret

; Сверить PROBE_BUF с узором PROBE_SEED_IN.
; Выход: DE — сколько байтов не совпало, HL — номер первого из них.
CheckBuffer:
        ld hl,0
        ld (CheckErrors),hl
        ld (CheckFirst),hl
        ld hl,PROBE_BUF
        ld de,0
.next:
        ld a,d
        add a,a
        add a,d
        add a,e
        add a,PROBE_SEED_IN
        cp (hl)
        jr z,.same
        push hl
        ld hl,(CheckErrors)
        ld a,h
        or l
        jr nz,.counted
        ld (CheckFirst),de
.counted:
        inc hl
        ld (CheckErrors),hl
        pop hl
.same:
        inc hl
        inc de
        ld a,d
        cp PROBE_LEN/256
        jr nz,.next
        ld de,(CheckErrors)
        ld hl,(CheckFirst)
        ret
CheckErrors:    dw 0
CheckFirst:     dw 0

; DE — несовпавших байтов, HL — номер первого. Напечатать итог строкой.
PrintVerdict:
        ld a,d
        or e
        jr nz,.bad
        ld a,4
        call SetInk
        ld hl,MsgOk
        call PrintLine
        ld a,7
        jp SetInk
.bad:
        push hl
        ld a,2
        call SetInk
        ld a,' '
        call PrintChar
        ex de,hl
        call PrintDec16
        ld hl,MsgErrors
        call PrintString
        pop hl
        call PrintDec16
        call PrintNewLine
        ld a,7
        jp SetInk

; Первые восемь байтов по HL — шестнадцатеричными числами.
PrintHead:
        ld b,8
.byte:
        ld a,' '
        call PrintChar
        ld a,(hl)
        rrca
        rrca
        rrca
        rrca
        call .digit
        ld a,(hl)
        call .digit
        inc hl
        djnz .byte
        jp PrintNewLine
.digit:
        and #0F
        add a,'0'
        cp '9'+1
        jr c,.print
        add a,'A'-'9'-1
.print:
        jp PrintChar

; Несколько раз посылать PING, пока ESP не начнёт отвечать READY.
WaitReady:
        ld b,PROBE_RETRIES
.attempt:
        push bc
        ld a,CMD_PING
        call Proto_SendEmpty
        jr c,.miss
        ld a,RESP_READY
        ld de,PROBE_WAIT
        call Proto_WaitCmd
.miss:
        pop bc
        ret nc
        djnz .attempt
        scf
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

; Строка HL и перевод строки.
PrintLine:
        call PrintString
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

MsgTitle:       db "VDAC4 DMA probe",13
                db "---------------",13,0
MsgRead:        db "ESP -> Evo, 1024 bytes:",13,0
MsgWrite:       db "Evo -> ESP, 1024 bytes:",13,0
MsgOk:          db " OK",0
MsgErrors:      db " bad bytes, first at ",0
MsgGot:         db " ESP got ",0
MsgDmaStuck:    db " Evo DMA did not finish",0
MsgNoEnd:       db " ESP did not see the end",0
MsgNoEsp:       db "VDAC4 firmware not found",0
MsgNoReply:     db "ESP firmware does not reply",0
MsgRefused:     db "ESP refused the probe",0
MsgEspError:    db "ESP error: ",0
MsgAgain:       db 13,"ANY KEY - again",13,0

        INCLUDE "zifi_link.asm"
        INCLUDE "proto.asm"

        ASSERT $ < #BE00,"dma_probe_vdac4.sna payload overlaps stack"
        SAVESNA "dma_probe_vdac4.sna",Start
