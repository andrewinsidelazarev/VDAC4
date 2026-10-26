; VFSRES.WMF — резидент VDAC4 VFS (тип #06): страница с драйвером устройства 6 и обменом с ESP.
; Раскладка страницы и связь с переключателем VDAC4VFS.WMF — vfs_res.inc. Сам резидент ничего
; в WC не меняет: при старте только запоминает свою физическую страницу. Переход SD2 -> VFS и
; обратно ставит и снимает переключатель.

        DEVICE ZXSPECTRUM128
        INCLUDE "wc111i.inc"
        INCLUDE "vfs_res.inc"

startCode:
        ORG #0000
        ds 16
        db "WildCommanderMDL"
        db #0A
        db 0                            ; +33: ранний вызов не нужен
        db 1                            ; страниц: одна
        db 0                            ; страница 0 — в окне #8000 на время вызова
        db 0,(pageEnd-pageStart+511)/512
        ds 10
        ds 16
        ds 96                           ; расширений нет
        db 0
        dd 0
.name:  db "VDAC4 VFS resident"
        ds 32-($-.name),' '
        db #06                          ; один раз при старте WC
        ds 512-$,0

pageStart:
; --- установка: окно #8000, A=#06 ------------------------------------------------------------
        DISP #8000
RES_INIT:
        ld a,(#6002)                    ; своя физическая страница
        ld (#8000+RES_SIG+SIG_PAGE),a
        ld (#8000+RES_IMG+(D_PAGE-#3800)),a
        ld d,a
        xor a
        ld (#8000+RES_SIG+SIG_STATE),a  ; SD2 штатный, пока переключатель не скажет иного
        ; Карта Tang могла остаться у прошлого сеанса WC (сброс Evo при SD2 = карта Tang): вернуть её
        ; ESP. Код обмена собран для окна #C000 — туда на время вызова та же страница.
        ld a,i
        di
        push af
        ld bc,#13AF
        in e,(c)
        push de
        out (c),d
        ld a,RES_RELEASE
        call RES_IO
        ld b,RES_INIT_TRIES
.wait:
        jr nc,.done
        cp RES_PENDING
        jr nz,.done                     ; старая прошивка без MODE не отвечает — ждать недолго
        push bc
        ld a,RES_WAIT
        call RES_IO
        pop bc
        djnz .wait
.done:
        pop de
        ld bc,#13AF
        out (c),e
        pop af
        ret po
        ei
        ret
RES_INIT_TRIES  equ 4                   ; по RES_POLLS опросов: не держать старт WC
        ENT

; --- подпись ---------------------------------------------------------------------------------
        ORG pageStart+RES_SIG
        db "VDAC4VFS"
        db RES_LAYOUT
        db 0                            ; SIG_PAGE
        db 0                            ; SIG_STATE
        dw RES_SWAP
        dw RES_IO
        dw 0                            ; SIG_SWAPS
        dw 0                            ; SIG_CALLS
        db 0,DIAG_OK,0                  ; SIG_OP, SIG_WHY, SIG_RES
        dw 0                            ; SIG_WAITS
        ds SIG_ERR_LEN,0                ; SIG_ERR
        ASSERT $-(pageStart+RES_SIG) == SIG_SIZE

; --- код окна #C000 --------------------------------------------------------------------------
        ORG pageStart+RES_CODE
        DISP RES_WINDOW+RES_CODE

; Из переходника DOS_SWP (A=6): прерывания закрыты, на стеке AF с прежним IFF, E — прежняя
; страница #C000. Образ драйвера — в #3800 окна #0000, дальше его хвост D_TAIL.
RES_SWAP:
        pop hl
        push ix                         ; штатный конец DOS_SWP снимает IX
        push hl
        ld a,e
        ld hl,(DIAG+SIG_SWAPS)
        inc hl
        ld (DIAG+SIG_SWAPS),hl
        ld hl,RES_WINDOW+RES_IMG
        ld de,#3800
        ld bc,D_END-#3800
        ldir
        ld e,a
        ld bc,#13AF
        jp D_TAIL

; Подпись в окне #C000: диагностика пишется туда же, где её читает переключатель
DIAG            equ RES_WINDOW+RES_SIG

; Операция A для драйвера (vfs_res.inc): прерывания закрыты, окно #0000 — WildDOS панели.
; Каждый вызов и итог каждой операции остаются в подписи (SIG_CALLS…SIG_ERR).
RES_IO:
        ld hl,(DIAG+SIG_CALLS)
        inc hl
        ld (DIAG+SIG_CALLS),hl
        cp RES_WAIT
        jr z,.wait_more
        cp RES_RELEASE+1
        jr nc,.bad
        ld (DIAG+SIG_OP),a
        ld hl,0
        ld (DIAG+SIG_WAITS),hl
        ld l,a
        xor a
        ld (DIAG+SIG_WHY),a
        ld (DIAG+SIG_RES),a
        ld (DIAG+SIG_ERR),a
        ld a,l
        cp RES_BEGIN
        jr z,.begin
        cp RES_INFO
        jr z,.info
        cp RES_WRITE
        jr z,.write
        cp RES_RELEASE
        jr z,.release
        jp .read
.bad:
        ld a,RES_FAIL
        scf
        ret
.why:                                   ; A — DIAG_*: записать и отказать
        ld (DIAG+SIG_WHY),a
        ld a,RES_FAIL
        scf
        ret
.begin:
        call ZiFi_Init
        ld a,DIAG_LINK
        jr c,.why
        ld a,RES_FAIL                   ; CF=0: A не смотрят
        ret
.info:
        xor a
        ld (ProtoErr),a
        ld a,RESP_VDISK_INFO
        ld (ResWant),a
        ld a,CMD_VDISK_INFO
        call Proto_SendEmpty
        ld a,DIAG_SEND
        jr c,.why
        jp .wait
.wait_more:
        ld hl,(DIAG+SIG_WAITS)
        inc hl
        ld (DIAG+SIG_WAITS),hl
        jp .wait
.write:
        ; сектор целиком одной командой: [lba LE32][512 байт из D_BOUNCE]
        ld hl,(D_LBA)
        ld (ResWr),hl
        ld hl,(D_LBA+2)
        ld (ResWr+2),hl
        ld hl,D_BOUNCE
        ld de,ResWr+4
        ld bc,512
        ldir
        xor a
        ld (ProtoErr),a
        ld a,RESP_VDISK_WRITE
        ld (ResWant),a
        ld hl,ResWr
        ld bc,4+512
        ld a,CMD_VDISK_WRITE
        call Proto_Send
        ld a,DIAG_SEND
        jp c,.why
        jr .wait
.release:
        ; MODE 0 — со своим началом сеанса: зовёт и установка при старте WC
        call ZiFi_Init
        ld a,DIAG_LINK
        jp c,.why
        xor a
        ld (ProtoErr),a
        ld (ResReq),a
        ld a,RESP_VDISK_MODE
        ld (ResWant),a
        ld hl,ResReq
        ld bc,1
        ld a,CMD_VDISK_MODE
        call Proto_Send
        ld a,DIAG_SEND
        jp c,.why
        jr .wait
.read:
        ; часть D_PART: смещение 0 / 248 / 496, длина 248 / 248 / 16
        ld hl,(D_LBA)
        ld (ResReq),hl
        ld hl,(D_LBA+2)
        ld (ResReq+2),hl
        ld a,(D_PART)
        ld hl,0
        ld de,VDISK_CHUNK
        ld c,VDISK_CHUNK
        or a
        jr z,.part
        add hl,de
        dec a
        jr z,.part
        add hl,de
        ld c,512-2*VDISK_CHUNK
.part:
        ld (ResReq+4),hl
        ld a,c
        ld (ResReq+6),a
        xor a
        ld (ProtoErr),a
        ld a,RESP_VDISK_READ
        ld (ResWant),a
        ld hl,ResReq
        ld bc,7
        ld a,(DIAG+SIG_STATE)           ; SD2 = карта Tang: сектор только из неё (байт режима 1);
        cp STATE_TANG                   ; ESP после перезагрузки — в томе VFS и откажет
        jr nz,.send_read
        ld a,1
        ld (ResReq+7),a
        inc c
.send_read:
        ld a,CMD_VDISK_READ
        call Proto_Send
        ld a,DIAG_SEND
        jp c,.why
.wait:
        ld a,(ResWant)
        ld de,RES_POLLS
        call Proto_WaitCmd
        jr nc,.got
        ld a,(ProtoErr)                 ; прошивка ответила отказом (нет команды) — не ждать
        or a
        jr nz,.esperr
        ld a,DIAG_WAIT
        ld (DIAG+SIG_WHY),a
        ld a,RES_PENDING
        scf
        ret
.esperr:
        ld hl,ProtoErrText
        ld de,DIAG+SIG_ERR
        ld bc,SIG_ERR_LEN-1
        ldir
        xor a
        ld (de),a
        ld a,DIAG_ESPERR
        jp .why
.got:
        ld a,(ProtoBuf)
        ld (DIAG+SIG_RES),a
        or a
        ld a,DIAG_RESULT
        jp nz,.why                      ; итог не 0: неверный запрос или источник не ответил
        ld a,(ResWant)
        cp RESP_VDISK_INFO
        jr z,.got_info
        cp RESP_VDISK_READ
        jr z,.got_read
        xor a                           ; WRITE и MODE: итог 0 — всё
        ld (DIAG+SIG_WHY),a
        ret
.got_read:
        ld a,(ResReq+6)
        ld c,a
        ld b,0
        ld hl,(ProtoRxLen)
        ld a,l
        ld (DIAG+SIG_RES),a
        dec hl
        or a
        sbc hl,bc
        ld a,DIAG_LENGTH
        jp nz,.why                      ; данных не столько, сколько просили
        ld hl,(ResReq+4)
        ld de,D_BOUNCE
        add hl,de
        ex de,hl
        ld hl,ProtoBuf+1
        ldir
        xor a
        ld (DIAG+SIG_WHY),a
        ret
.got_info:
        ld a,(ProtoBuf+1)
        ld (DIAG+SIG_RES),a
        and VDISK_READY
        ld a,DIAG_NOTREADY
        jp z,.why
        xor a
        ld (DIAG+SIG_WHY),a
        ret

ResWant:        db 0                    ; какой ответ ждём
ResReq:         ds 8                    ; READ [lba LE32][смещение LE16][длина][режим]; MODE — [режим]
ResWr:          ds 4+512                ; WRITE: [lba LE32][сектор]

        INCLUDE "zifi_link.asm"
        INCLUDE "proto.asm"
RES_CODE_END:
        ENT
        ASSERT RES_CODE_END-RES_WINDOW <= RES_IMG, код резидента налез на образ драйвера

; --- образ драйвера: логический #3800 --------------------------------------------------------
        ORG pageStart+RES_IMG
        DISP #3800
        INCLUDE "driver.asm"
D_END:
        ENT
        ASSERT D_END-#3800 <= RES_IMG_MAX, образ драйвера налез на буфер сектора
pageEnd:
        ORG pageEnd
        ALIGN 512
endCode:
        SAVEBIN "../build/VFSRES.WMF",startCode,endCode-startCode
