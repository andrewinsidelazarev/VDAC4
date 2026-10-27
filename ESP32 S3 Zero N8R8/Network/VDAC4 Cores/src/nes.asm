; .nes с панели WC: поток 0 уже открыт Wild Commander. Картридж уходит в PSRAM
; ESP целиком, пока Tang остаётся TS-Config. Только после проверки вызывается START.
Nes_File:
        ld bc,2                         ; передача файла и CRC — Z80 14 МГц
        ld a,FN_TURBOPL
        call WC_API
        ld hl,NesEvoTitle
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
        ret c
        jp Nes_SavePrepare

; Файл открытого потока (NesSize байт) — в PSRAM ESP: #77 размер, #78 блоки по 512 с CRC16,
; #79 конец (и для меню F1 — evo_menu.asm). Выход: CF=0 — принят; CF=1, HL — текст ошибки
; (HL=0 — отменён Esc; NesNoEsc ≠ 0 — Esc не проверяется).
Nes_Send:
        ld hl,NesSize
        ld bc,4
        ld a,#77
        call Proto_Send
        ld a,#F7
        call Nes_Ack
        ret c
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
        ld a,d
        or e
        jr nz,.full
        ld a,h
        cp 2
        jr c,.size
.full:
        ld hl,512
.size:
        ld (NesCount),hl
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
        ld hl,(NesSent)
        ld de,(NesCount)
        add hl,de
        ld (NesNext),hl
        ld hl,(NesSent+2)
        ld de,0
        adc hl,de
        ld (NesNext+2),hl
        ld a,3
        ld (NesTries),a
.send:
        ld hl,(NesCount)
        ld de,6
        add hl,de
        ld b,h
        ld c,l
        ld hl,NesPacket
        ld a,#78
        call Proto_Send
        ld a,#F8
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
        ld a,#79
        call Proto_SendEmpty
        ld a,#F9
        jp Nes_Ack

Nes_Ack:
        ld de,NX_WAIT
        call Proto_WaitCmd
        ld hl,NesErrTransfer
        ret c
        ld a,(ProtoBuf)
        cp 1
        ret z
        call Nx_Status
        ld hl,NesErrTransfer
        ret c
        cp NX_ERROR
        jp z,Nx_ErrorText
        scf
        ret

; CCITT-FALSE: CRC16(смещение LE32 + данные), начальное FFFF, полином 1021.
; HL/BC — вход, HL за концом, DE — результат.
Nes_Crc16:
        ld de,#FFFF
.byte:
        ld a,(hl)
        inc hl
        xor d
        ld d,a
        push bc
        ld b,8
.bit:
        sla e
        rl d
        jr nc,.next
        ld a,d
        xor #10
        ld d,a
        ld a,e
        xor #21
        ld e,a
.next:
        djnz .bit
        pop bc
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
NesLoading:     db "Reading cartridge from the Evo SD...",0
NesCancelled:   db "Cartridge loading cancelled",0
NesErrRead:     db "ERROR: could not read cartridge file",0
NesErrTransfer: db "ERROR: cartridge transfer failed",0
NesErrVersion:  db "ERROR: update CORES/NES.ELF to 0.2+",0
NesErrMenu:     db "ERROR: update CORES/NES.ELF to 0.7+",0
