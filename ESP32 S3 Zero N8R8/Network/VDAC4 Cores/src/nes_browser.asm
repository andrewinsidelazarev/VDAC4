; Просмотр каталогов SD Tang. Выбор — индекс записи ESP, никогда экранное имя.
Nes_Tang:
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
        and 2
        ld hl,NesErrFirmware
        scf
        ret z
        call WC_WAITUP
        xor a                           ; корень
        ld c,a
.request:
        ld (NesRequest),a
        ld a,c
        ld (NesRequest+1),a
        ld hl,NesRequest
        ld bc,2
        ld a,#74
        call Proto_Send
        ld a,#F4
        call Nes_Ack
        ret c
        ld hl,NesReading
        call Ui_Status
        ld hl,1500                      ; до 30 секунд
        ld (NxTimer),hl
.wait:
        ei
        halt
        ld hl,(NxTimer)
        dec hl
        ld (NxTimer),hl
        ld a,h
        or l
        ld hl,NesErrTransfer
        scf
        ret z
        ld a,#75
        call Proto_SendEmpty
        ld a,#F5
        ld de,NX_WAIT
        call Proto_WaitCmd
        jr c,.wait
        ld a,(ProtoBuf)
        cp 255
        jr z,.error
        cp 2
        jr nz,.wait
        ld a,(ProtoBuf+1)
        cp 9
        ld hl,NesErrTransfer
        scf
        ret nc
        ld hl,ProtoBuf
        ld de,NesPageData
        ld bc,401
        ldir
        xor a
        ld (MenuSel),a
.draw:
        call Nes_DrawPage
.menu:
        ei
        halt
        call WC_ESC
        jr nz,.back
        call WC_KEY_ENTER
        jr nz,.enter
        call WC_KEY_DOWN
        jr nz,.down
        call WC_KEY_UP
        jr nz,.up
        ld a,FN_PGDN
        call WC_API
        jr nz,.next
        ld a,FN_PGUP
        call WC_API
        jr nz,.prev
        jr .menu
.error:
        call Nx_Status
        jp Nx_ErrorText
.back:
        ld a,(NesPageData+2)
        and 2
        jr z,.cancel
        ld a,2
        jp .request
.cancel:
        ld hl,0
        scf
        ret
.next:
        ld a,(NesPageData+2)
        and 1
        jr z,.menu
        ld a,3
        jp .request
.prev:
        ld a,(NesPageData+2)
        and 4
        jr z,.menu
        ld a,4
        jp .request
.down:
        ld a,(MenuSel)
        inc a
        ld hl,NesPageData+1
        cp (hl)
        jr nc,.menu
        ld (MenuSel),a
        jr .draw
.up:
        ld a,(MenuSel)
        or a
        jr z,.menu
        dec a
        ld (MenuSel),a
        jr .draw
.enter:
        ld a,(NesPageData+1)
        or a
        jr z,.menu
        ld a,(MenuSel)
        call Nes_Row
        bit 0,(hl)
        ld a,(MenuSel)
        ld c,a
        ld a,1
        jp nz,.request
        ld hl,MenuSel
        ld bc,1
        ld a,#76
        call Proto_Send
        ld a,#F6
        jp Nes_Ack

; A — строка (0..7), HL — её флаги и имя.
Nes_Row:
        ld hl,NesPageData+49
        or a
        ret z
        ld b,a
        ld de,44
.add:
        add hl,de
        djnz .add
        ret

Nes_DrawPage:
        ld hl,NesTangTitle
        ld de,UiTitle
        ld b,44
        call Ui_CopyField
        ld hl,NesPageData+5
        call Ui_SetStatus
        ld hl,NesHint
        call Ui_SetHint
        ld de,UiListText
        xor a
        ld (UiIdx),a
.line:
        push de
        ld hl,UiEmpty
        ld b,44
        call Ui_CopyField
        pop de
        ld a,(UiIdx)
        ld hl,NesPageData+1
        cp (hl)
        jr nc,.next
        ld hl,MenuSel
        cp (hl)
        jr nz,.entry
        ld a,'>'
        ld (de),a
.entry:
        push de
        ld a,(UiIdx)
        call Nes_Row
        pop de
        push de
        inc de
        bit 0,(hl)
        jr z,.file
        ld a,'/'
        ld (de),a
.file:
        inc hl
        inc de
        ld b,42
        call Ui_CopyField
        pop de
.next:
        ld hl,45
        add hl,de
        ex de,hl
        ld hl,UiIdx
        inc (hl)
        ld a,(hl)
        cp 8
        jr c,.line
        jp Ui_Draw

NesRequest:     ds 2
NesPageData:    ds 401
NesHint:        db "Enter: open  Esc: up/back  PgUp/PgDn: page",0
NesReading:     db "Reading cartridges on Tang SD...",0
NesErrFirmware: db "ERROR: Tang browsing needs ESP 0.1.93+",0
