; .nes с панели WC: поток 0 уже открыт Wild Commander. Картридж уходит в PSRAM
; ESP целиком, пока Tang остаётся TS-Config. Только после проверки вызывается START.
Nes_File:
        ld bc,2                         ; передача файла и CRC — Z80 14 МГц
        ld a,FN_TURBOPL
        call WC_API
        ld hl,(EvoTitle)                ; «… from the Evo SD» по ядру: NES, Amiga, Mega Drive
        ld de,UiTitle
        ld b,44
        call Ui_CopyField
        ld hl,(NesNamePtr)
        ld a,h
        or l
        jr z,.no_name
        ld de,UiListText
        ld b,44
        call Ui_CopyField
.no_name:
        ld a,(ScrFile)                  ; 0.19: образ уже принял экран ядер — слою только «возьми»
        or a
        jr nz,.take
        ld a,#7A
        call Proto_SendEmpty
        ld a,#FA
        ld de,NX_WAIT
        call Proto_WaitCmd
        ld hl,NesErrVersion
        ret c
        ld a,(ProtoBuf)
        cp 1
        scf
        ret nz
        ld a,(ProtoBuf+1)
        and 1
        scf
        ret z
        ld hl,NesLoading
        call Ui_Status
        call Nes_Send
        jr .sent
.take:
        call Nes_Take                   ; 0.20: и NES — его сохранения дальше, как после #79
.sent:
        ret c
        ld a,(AdfCall)                   ; у дискеты сохранений .sav нет
        or a
        ret nz
        jp Nes_SavePrepare

; Файл панели WC — образ Mega Drive (.md, .gen, .smd, .bin)? CF=0 — да. Без имени — нет.
Md_IsFile:
        ld hl,(NesNamePtr)
        ld a,h
        or l
        scf
        ret z
        ld b,0
.len:
        ld a,(hl)
        or a
        jr z,Ext_IsMd
        inc hl
        inc b
        jr nz,.len
        scf
        ret

; Имя — образ Mega Drive? HL — на нуле в конце имени, B — длина. CF=0 — .md (от 4 знаков), .bin .gen .smd
; (от 5). Регистры BC, DE портит.
Ext_IsMd:
        ld a,b
        cp 4
        ret c
        push hl
        dec hl
        ld a,(hl)
        or #20
        ld c,a                          ; последняя буква
        dec hl
        ld a,(hl)
        or #20
        ld d,a                          ; предпоследняя
        dec hl
        ld a,(hl)
        cp '.'
        jr nz,.three
        ld a,d
        cp 'm'
        jr nz,.no
        ld a,c
        cp 'd'
        jr nz,.no
        pop hl
        or a
        ret
.three:
        ld a,b
        cp 5
        jr c,.no
        ld a,(hl)
        or #20
        ld e,a                          ; первая из трёх
        dec hl
        ld a,(hl)
        cp '.'
        jr nz,.no
        ld hl,ExtMdList
        ld b,3
.cmp:
        ld a,(hl)
        cp e
        jr nz,.skip2
        inc hl
        ld a,(hl)
        cp d
        jr nz,.skip1
        inc hl
        ld a,(hl)
        cp c
        jr z,.yes
        jr .next
.skip2:
        inc hl
.skip1:
        inc hl
.next:
        inc hl
        djnz .cmp
.no:
        pop hl
        scf
        ret
.yes:
        pop hl
        or a
        ret

ExtMdList:      db "bingensmd"

; Файл панели WC — .adf? HL — имя от WC, 0 в конце. Выход: CF=0 — да. Без имени — нет.
Adf_IsFile:
        ld hl,(NesNamePtr)
        ld a,h
        or l
        scf
        ret z
        ld b,0
.len:
        ld a,(hl)
        or a
        jr z,.ext
        inc hl
        inc b
        jr nz,.len
        scf
        ret
.ext:
        ld a,b
        cp 5
        ret c                            ; короче "x.adf": CF=1
        dec hl
        ld a,(hl)
        or #20
        cp 'f'
        scf
        ret nz
        dec hl
        ld a,(hl)
        or #20
        cp 'd'
        scf
        ret nz
        dec hl
        ld a,(hl)
        or #20
        cp 'a'
        scf
        ret nz
        dec hl
        ld a,(hl)
        cp '.'
        scf
        ret nz
        or a
        ret

; Файл открытого потока (NesSize байт) — в PSRAM ESP: #77 размер, #78 блоки по 512 с CRC16,
; #79 конец (и для меню F1 — evo_menu.asm). Выход: CF=0 — принят; CF=1, HL — текст ошибки
; (HL=0 — отменён Esc; NesNoEsc ≠ 0 — Esc не проверяется).
Nes_Send:
        xor a
        ld (ScrRaw),a                   ; слою — только пакеты
        ld a,#77                        ; слою ядра: #77 длина, #78 пакеты, #79 конец (A — команда Proto_Send)
        ld (NesCmd),a
        ld hl,NesSize
        ld bc,4
        call Proto_Send
        ld a,#F7
        call Nes_Ack
        ret c
; 0.19: тело файла — пакетами NesCmd+1 и конец NesCmd+2 (ответы — код | #80); BEGIN (NesCmd) уже
; принят. Слою ядра — #78/#79, экрану ядер (COREMENU 0.2) — #4B/#4C.
; 0.20: экрану с ScrRaw = 1 (COREMENU 0.3) — порциями DMA до RAW_MAX (Scr_RawRead, Scr_RawPut): ответ
; CD [1][принято LE32], как у пакета; конец — тот же #4C. Пакет по 512 стоил ~16 мс (2 МБ — минута с
; лишним, «как с магнитофона»), порция в 8 КБ — одно чтение 16 секторов и пара обменов.
Nes_SendBody:
        ld hl,NesSize
        ld de,NesLeft
        ld bc,4
        ldir
        ld hl,0
        ld (NesSent),hl
        ld (NesSent+2),hl
.loop:
        ld a,(NesNoEsc)
        or a
        jr nz,.no_esc
        call WC_ESC
        ld hl,0
        scf
        ret nz
.no_esc:
        ld hl,(NesLeft)
        ld de,(NesLeft+2)
        ld a,d
        or e
        or h
        or l
        jp z,.finish
        ld bc,512                       ; пакет — сектор; порция DMA — до RAW_MAX (младший байт 0)
        ld a,(ScrRaw)
        or a
        jr z,.max
        ld b,high RAW_MAX
.max:
        ld a,d
        or e
        jr nz,.full
        ld a,h
        cp b
        jr c,.size
.full:
        ld h,b
        ld l,c
.size:
        ld (NesCount),hl
        ld de,(NesSent)                 ; NesNext = NesSent + NesCount
        add hl,de
        ld (NesNext),hl
        ld hl,(NesSent+2)
        ld de,0
        adc hl,de
        ld (NesNext+2),hl
        ld a,(ScrRaw)
        or a
        jr z,.packet_read
        call Scr_RawRead                ; порция — в RAW_BUF, управление raw media — в ScrRawCtl
        ld hl,NesErrRead
        ret c
        jr .tries
.packet_read:
        ld hl,NesPacket+4
        ld b,1
        ld a,FN_LOAD512
        call WC_API
        ei
        ld hl,NesErrRead
        ret c
        ld hl,NesSent
        ld de,NesPacket
        ld bc,4
        ldir
        ld hl,(NesCount)
        ld de,4
        add hl,de
        ld b,h
        ld c,l
        ld hl,NesPacket
        call Nes_Crc16                  ; DE — CRC, HL — сразу за данными
        ld (hl),e
        inc hl
        ld (hl),d
.tries:
        ld a,3
        ld (NesTries),a
.send:
        ld a,(ScrRaw)
        or a
        jr z,.packet
        call Scr_RawPut                 ; DMA; ответ CD — обычным путём
        jr c,.retry
        jr .answer
.packet:
        ld hl,(NesCount)
        ld de,6
        add hl,de
        ld b,h
        ld c,l
        ld hl,NesPacket
        ld a,(NesCmd)
        inc a                           ; #78 / #4B
        call Proto_Send
.answer:
        ld a,(NesCmd)
        inc a
        ld hl,ScrRaw                    ; порция DMA: ответ #CD (NesCmd + 3)
        add a,(hl)
        add a,(hl)
        or #80
        ld de,NX_WAIT
        call Proto_WaitCmd
        jr c,.retry
        ld a,(ProtoBuf)
        cp 1
        jr nz,.retry
        ld hl,ProtoBuf+1
        ld de,NesNext
        ld b,4
.compare:
        ld a,(de)
        cp (hl)
        jr nz,.retry
        inc hl
        inc de
        djnz .compare
        ld hl,NesNext
        ld de,NesSent
        ld bc,4
        ldir
        ld hl,(NesLeft)
        ld de,(NesCount)
        or a
        sbc hl,de
        ld (NesLeft),hl
        ld hl,(NesLeft+2)
        ld de,0
        sbc hl,de
        ld (NesLeft+2),hl
        jp .loop
.retry:
        ld hl,NesTries
        dec (hl)
        jr nz,.send
        ld hl,NesErrTransfer
        scf
        ret
.finish:
        ld a,(NesCmd)
        add a,2                         ; #79 / #4C
        call Proto_SendEmpty
        ld a,(NesCmd)
        add a,2
        or #80
        jp Nes_Ack

; 0.19: образ принял экран ядер (COREMENU 0.2) и оставил его слою (прошивка 0.1.123+, ABI 1.14): слою —
; #7A [1] «возьми». Ответ [1][возможности, бит 8 — умеет брать][1 — взял]. Слой старый (MD.ELF до 1.5,
; NES.ELF до 0.10, AMIGA.ELF до 0.27 — бита нет) — образ второй раз не прочитать: файл WC уже дочитан, —
; ошибка с подсказкой. Не взял — его текст ошибки (заголовок образа и т. п.). Взял — слой проверил образ
; и (NES) приготовил сохранения, как после #79. Выход: CF=0 — образ у слоя; CF=1, HL — текст.
Nes_Take:
        ld hl,NesTake
        ld bc,1
        ld a,#7A
        call Proto_Send
        ld a,#FA
        ld de,NX_WAIT
        call Proto_WaitCmd
        ld hl,NesErrVersion
        ret c
        ld a,(ProtoBuf+1)
        and 8
        ld hl,NesErrTake
        scf
        ret z
        ld a,(ProtoBuf+2)
        or a
        ret nz                          ; взял: CF=0
        jr Nes_AckFail                  ; не взял: его текст ошибки или «transfer failed»

Nes_Ack:
        ld de,NX_WAIT
        call Proto_WaitCmd
        ld hl,NesErrTransfer
        ret c
        ld a,(ProtoBuf)
        cp 1
        ret z
Nes_AckFail:
        call Nx_Status
        ld hl,NesErrTransfer
        ret c
        cp NX_ERROR
        jp z,Nx_ErrorText
        scf
        ret

; CCITT-FALSE: CRC16(смещение LE32 + данные), начальное FFFF, полином 1021.
; HL/BC — вход, HL за концом, DE — результат.
; 0.17: таблицей (crc16_tab.inc) — около 90 тактов на байт вместо ~450 побитно: ROM Mega Drive на 4 МБ
; с карты Evo иначе шёл бы минуты на одном CRC.
Nes_Crc16:
        call Page1_Map                  ; 0.20: таблицы — в странице 1 (окно #C000)
        ld de,#FFFF
.byte:
        ld a,(hl)
        inc hl
        xor d                           ; индекс — старший байт CRC ^ байт
        push hl
        ld l,a
        ld h,high Crc16Hi
        ld a,e
        xor (hl)
        ld d,a                          ; старший = младший ^ T.hi
        inc h
        ld e,(hl)                       ; младший = T.lo
        pop hl
        dec bc
        ld a,b
        or c
        jr nz,.byte
        ret

NesCall:        db 3
NesMenu:        db 0                    ; 1 — NES из меню: START без образа, игра — из меню F1 ядра
NesNamePtr:     dw 0
NesSize:        ds 4
NesLeft:        ds 4
NesSent:        ds 4
NesNext:        ds 4
NesCount:       dw 0
NesTries:       db 0
NesNoEsc:       db 0
NesPacket:      ds 4+512+2
NesName:        db "NES",0
AdfName:        db "AMIGA",0
MdName:         db "MD",0
AdfCall:        db 0                    ; 1 — .adf или образ Mega Drive с панели WC: без шага .sav (#7B)
EvoTitle:       dw NesEvoTitle          ; заголовок окна WC файла с панели — по ядру (0.18)
NesCmd:         db #77                  ; BEGIN передачи (Nes_SendBody): #77 — слою ядра, #4A — экрану
NesTake:        db 1                    ; #7A [1]: «возьми образ у экрана» (0.19)
NesErrTake:     db "ERROR: update the core's ELF in CORES",0
NesLoading:     db "Reading from the Evo SD...",0
NesCancelled:   db "Loading cancelled",0
NesErrRead:     db "ERROR: could not read the file",0
NesErrTransfer: db "ERROR: file transfer failed",0
NesErrVersion:  db "ERROR: update CORES/NES.ELF to 0.2+",0
NesErrMenu:     db "ERROR: update CORES/NES.ELF to 0.7+",0
