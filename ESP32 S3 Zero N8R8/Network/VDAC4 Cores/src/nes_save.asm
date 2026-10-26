; После успешного старта запомнить размер сохранения и для браузера Tang.
Nes_SaveTrack:
        ld hl,CrName
        ld de,NesName
        ld bc,4
        call Ns_Compare
        ret c
        ld a,1
        ld (NsStarted),a
        ld a,#7B
        call Proto_SendEmpty
        ld a,#FB
        ld de,NX_WAIT
        call Proto_WaitCmd
        ret c
        ld a,(ProtoBuf)
        cp 1
        ret nz
        ld hl,ProtoBuf+2
        ld de,NsSize
        ld bc,4
        ldir
        ret

; Сохранения на томе/в каталоге исходного .nes. STREAM_CLONE фиксирует панель
; до запуска. FILEX заменяет .sav проверенным .svt без удаления старой цепочки.
Nes_SavePrepare:
        ld a,#7B
        call Proto_SendEmpty
        ld a,#FB
        ld de,NX_WAIT
        call Proto_WaitCmd
        ld hl,NsErrVersion
        ret c
        ld a,(ProtoBuf)
        cp 1
        scf
        ret nz
        ld hl,ProtoBuf+2
        ld de,NsSize
        ld bc,4
        ldir
        ld hl,NsSize
        call Ns_IsZero
        ret z
        ld d,STREAM_CLONE
        ld a,FN_STREAM
        call WC_API
        call Ns_Select
        call Ns_FilexReset
        call Ns_Filex
        ld hl,NsErrWc
        scf
        ret nz
        ld a,(NsFx+24)
        and #53                         ; READ_AT, WRITE_AT, MOVE, CURRENT_DIR
        cp #53
        scf
        ret nz
        ld hl,(NesNamePtr)
        ld a,h
        or l
        ld de,NsEntry+1
        ld bc,0
        jp z,Ns_BadName
.name:
        ld a,(hl)
        ld (de),a
        inc hl
        inc de
        inc bc
        or a
        jr z,.named
        ld a,b
        or a
        jp nz,Ns_BadName
        jr .name
.named:
        ld a,c
        cp 5
        jp c,Ns_BadName
        inc bc                          ; атрибут + имя + NUL
        ld (NsNameLen),bc
        ex de,hl
        ld de,-4
        add hl,de                       ; три байта расширения
        ld (hl),'s'
        inc hl
        ld (hl),'a'
        inc hl
        ld (hl),'v'
        ld hl,NsEntry
        ld de,NsTemp
        ld bc,(NsNameLen)
        ldir
        dec de
        dec de
        ld a,'t'
        ld (de),a                       ; .sat -> исправить второй символ
        dec de
        ld a,'v'
        ld (de),a                       ; .svt
        ld hl,NsEntry
        ld a,FN_FENTRY
        call WC_API
        jr z,.missing
        ld (NsFoundSize),hl
        ld (NsFoundSize+2),de
        ld hl,NsFoundSize
        ld de,NsSize
        ld bc,4
        call Ns_Compare
        ld hl,NsErrRead
        ret c
        xor a
        ld (NsPacket),a
        ld hl,NsSize
        ld de,NsPacket+1
        ld bc,4
        ldir
        ld hl,NsPacket
        ld bc,5
        call Ns_SendRestore
        ret c
        call Ns_OffsetZero
.read:
        ld a,1                          ; READ_AT
        call Ns_SectorIo
        ld hl,NsErrRead
        scf
        ret nz
        ld a,1
        ld (NsPacket),a
        ld hl,NsOffset
        ld de,NsPacket+1
        ld bc,4
        ldir
        ld hl,NsSector
        ld de,NsPacket+5
        ld bc,512
        ldir
        ld hl,NsPacket+1
        ld bc,516
        call Nes_Crc16
        ld (hl),e
        inc hl
        ld (hl),d
        ld hl,NsPacket
        ld bc,519
        call Ns_SendRestore
        ret c
        call Ns_OffsetNext
        jr nz,.read
        ld a,2
        jr .one
.missing:
        ld a,3                          ; нет .sav — новая игра
.one:
        ld (NsPacket),a
        ld hl,NsPacket
        ld bc,1
        jp Ns_SendRestore

Ns_SendRestore:
        ld a,#7C
        call Proto_Send
        ld a,#FC
        ld de,NX_WAIT
        call Proto_WaitCmd
        ld hl,NsErrRead
        ret c
        ld a,(ProtoBuf)
        cp 1
        ret z
        scf
        ret

Ns_BadName:
        ld hl,NsErrName
        scf
        ret
Ns_IsZero:
        ld a,(hl)
        inc hl
        or (hl)
        inc hl
        or (hl)
        inc hl
        or (hl)
        ret
Ns_Select:
        ld d,1
        ld bc,#FFFF
        ld a,FN_STREAM
        jp WC_API
Ns_FilexReset:
        ld hl,NsFx
        ld de,NsFx+1
        ld bc,31
        ld (hl),0
        ldir
        ld a,32
        ld (NsFx),a
        ld a,1
        ld (NsFx+1),a
        ret
Ns_Filex:
        ld hl,NsFx
        ld a,FN_FILEX
        call WC_API
        or a
        ret
; A — READ_AT/WRITE_AT, общий буфер NsSector, позиция NsOffset.
Ns_SectorIo:
        push af
        call Ns_FilexReset
        pop af
        ld (NsFx+2),a
        ld hl,NsOffset
        ld de,NsFx+4
        ld bc,4
        ldir
        ld hl,NsSector
        ld (NsFx+8),hl
        ld hl,512
        ld (NsFx+10),hl
        call Ns_Filex
        ret nz
        ld hl,(NsFx+24)
        ld de,512
        or a
        sbc hl,de
        ret nz
        ld hl,(NsFx+26)
        ld a,h
        or l
        ret
Ns_OffsetZero:
        ld hl,0
        ld (NsOffset),hl
        ld (NsOffset+2),hl
        ret
Ns_OffsetNext:
        ld hl,(NsOffset)
        ld de,512
        add hl,de
        ld (NsOffset),hl
        ld hl,(NsOffset+2)
        ld de,0
        adc hl,de
        ld (NsOffset+2),hl
        ld hl,NsOffset
        ld de,NsSize
        ld bc,4
        jp Ns_Compare
; CF=1/NZ — различаются; CF=0/Z — равны. HL,DE,BC разрушены.
Ns_Compare:
        ld a,(de)
        cp (hl)
        jr nz,.bad
        inc hl
        inc de
        dec bc
        ld a,b
        or c
        jr nz,Ns_Compare
        ret
.bad:
        scf
        ret

; Снимок ESP готов, Tang уже TS-Config. До подтверждения ESP хранит копию.
Nes_SaveWrite:
        call Nx_VideoBack
        call Ui_Open
.retry:
        ld hl,NsWriting
        call Ui_Status
        call Ns_WriteOnce
        jr nc,.ack
        ld hl,NsErrWrite
        call Ui_Status
        call Ns_AskRetry
        cp 1
        jr z,.retry
        xor a                           ; явный отказ пользователя
        jr .confirm
.ack:
        ld a,1
.confirm:
        ld (NsPacket),a
        ld hl,NsPacket
        ld bc,1
        ld a,#7E
        call Proto_Send
        ld a,#FE
        ld de,NX_WAIT
        call Proto_WaitCmd
        ret c
        ld a,(ProtoBuf)
        cp 1
        ret z
        scf
        ret

Ns_WriteOnce:
        call Ns_Select
        ld a,(NsTempOwned)
        or a
        jr z,.create
        ld hl,NsTemp
        ld a,FN_DELETE
        call WC_API
        jr nz,.create
        scf
        ret
.create:
        xor a
        ld (NsTempOwned),a
        ld hl,NsSize
        ld de,NsCreate+1
        ld bc,4
        ldir
        ld hl,NsTemp+1
        ld de,NsCreate+5
        ld bc,(NsNameLen)
        dec bc
        ldir
        ld hl,NsCreate
        ld a,FN_MKFILE
        call WC_API
        jr z,.created
        scf
        ret
.created:
        ld a,1
        ld (NsTempOwned),a
        ld hl,NsTemp
        ld a,FN_FENTRY
        call WC_API
        jr nz,.found
        scf
        ret
.found:
        call Ns_OffsetZero
.sector:
        ld hl,NsOffset
        ld bc,4
        ld a,#7D
        call Proto_Send
        ld a,#FD
        ld de,NX_WAIT
        call Proto_WaitCmd
        ret c
        ld a,(ProtoBuf)
        cp 1
        scf
        ret nz
        ld hl,ProtoBuf+1
        ld de,NsOffset
        ld bc,4
        call Ns_Compare
        ret c
        ld hl,ProtoBuf+1
        ld bc,516
        call Nes_Crc16
        ld a,(hl)
        cp e
        scf
        ret nz
        inc hl
        ld a,(hl)
        cp d
        scf
        ret nz
        ld hl,ProtoBuf+5
        ld de,NsSector
        ld bc,512
        ldir
        ld hl,NsSector
        ld de,NsVerify
        ld bc,512
        ldir
        ld a,2                          ; WRITE_AT, ровно 512 байтов
        call Ns_SectorIo
        scf
        ret nz
        ld a,1                          ; чтение обратно каждого сектора
        call Ns_SectorIo
        scf
        ret nz
        ld hl,NsSector
        ld de,NsVerify
        ld bc,512
        call Ns_Compare
        ret c
        call Ns_OffsetNext
        jp nz,.sector
        call Ns_FilexReset
        ld a,5
        ld (NsFx+2),a
        ld a,3                          ; REPLACE | CURRENT_DIR
        ld (NsFx+3),a
        ld hl,NsTemp
        ld (NsFx+8),hl
        ld hl,NsEntry
        ld (NsFx+12),hl
        ld hl,(NsNameLen)
        ld (NsFx+10),hl
        ld (NsFx+14),hl
        call Ns_Filex
        jr z,.committed
        cp FILEX_STATUS_COMMITTED_CLEANUP
        scf
        ret nz
.committed:
        xor a
        ld (NsTempOwned),a
        ret

; Ошибка записи на карту: приложение удерживает данные до повтора/отказа.
Core_SaveFailure:
Nes_SaveFailure:
        call Nx_ErrorText
        push hl
        call Nx_VideoBack
        call Ui_Open
        pop hl
        call Ui_Status
        call Ns_AskRetry
        cp 1
        ld a,0
        jr nz,.send
        ld a,2
.send:
        ld (NsPacket),a
        ld hl,NsPacket
        ld bc,1
        ld a,#7E
        call Proto_Send
        ld a,#FE
        ld de,NX_WAIT
        jp Proto_WaitCmd

Ns_AskRetry:
        ld hl,NsRetryHint
        call Ui_Hint
.wait:
        ei
        halt
        ld bc,#EFF7
        ld a,#80
        out (c),a
        ld b,#DF
        ld a,#F0
        out (c),a
        ld b,#BF
        in a,(c)
        or a
        jr z,.wait
        ld e,a
        ld a,(Ps2Prefix)
        or a
        jr nz,.plain
        ld a,e
        cp #5A
        jr z,.yes
        cp #76
        jr z,.no
.plain:
        ld a,e
        call Nx_Key
        jr .wait
.yes:
        ld a,1
        ret
.no:
        ld a,2
        ret

NsSize:         ds 4
NsFoundSize:    ds 4
NsOffset:       ds 4
NsNameLen:      dw 0
NsTempOwned:    db 0
NsStarted:      db 0
NsEntry:        ds 257
NsTemp:         ds 257
NsCreate:       ds 261
NsFx:           ds 32
NsPacket:       ds 519
NsSector:       ds 512
NsVerify:       ds 512
NsWriting:      db "Saving progress to the Evo SD...",0
NsRetryHint:    db "ENTER = retry     ESC = discard changes",0
NsErrVersion:   db "ERROR: saves need NES.ELF 0.3+",0
NsErrWc:        db "ERROR: update WC for safe save replacement",0
NsErrRead:      db "ERROR: save unreadable / wrong game or CRC",0
NsErrWrite:     db "ERROR: save failed; previous file kept",0
NsErrName:      db "ERROR: cartridge name too long for save",0
NsErrTimeout:   db "Core exit unconfirmed; changes may be unsaved",0
