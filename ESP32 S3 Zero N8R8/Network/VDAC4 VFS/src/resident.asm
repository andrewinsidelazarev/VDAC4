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
        cp RES_NEXTN+1
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
        cp RES_READN
        jp z,.readn
        cp RES_NEXTN
        jp z,.nextn
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
        or a
        jp z,.sec_go                    ; ждём сектор серии DMA
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
        ld a,(ResWant)
        cp RESP_VDISK_DATA
        jp z,.got_data
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
        ; READN — в живой драйвер (#3800…) и в его образ: RES_SWAP кладёт образ заново на каждый DOS_SWP
        and VDISK_READN|VDISK_READDMA
        ld (D_READN),a
        ld (RES_WINDOW+RES_IMG+(D_READN-#3800)),a
        ld a,(ProtoBuf+1)
        and VDISK_READY
        ld a,DIAG_NOTREADY
        jp z,.why
        xor a
        ld (DIAG+SIG_WHY),a
        ret

; READN: серия с D_LBA на D_COUNT секторов (D_COUNT — сколько осталось вместе с этим), первый сектор
.readn:
        ld hl,(D_LBA)
        ld (ResReq),hl
        ld hl,(D_LBA+2)
        ld (ResReq+2),hl
        ld a,(D_COUNT)
        ld (ResReq+4),a
        ld a,(DIAG+SIG_STATE)           ; режим, как у READ: SD2 = карта Tang — 1
        cp STATE_TANG
        ld a,0
        jr nz,.readn_mode
        inc a
.readn_mode:
        ld b,a
        xor a
        ld (ResSec),a
        ld a,(D_READN)
        and VDISK_READDMA
        ld (ResDma),a
        jr z,.readn_frames
        set 7,b                         ; READN_DMA: секторы — в очередь DMA, не кадрами
.readn_frames:
        ld a,b
        ld (ResReq+5),a
        xor a
        ld (ProtoErr),a
        ld hl,ResReq
        ld bc,6
        ld a,CMD_VDISK_READN
        call Proto_Send
        ld a,DIAG_SEND
        jp c,.why
; следующий сектор серии: DMA (ESP 0.1.128) либо два кадра по 256 байт -> D_BOUNCE, D_BOUNCE+256
.nextn:
        ld a,(ResDma)
        or a
        jp nz,.sec_start
        inc a
        ld (D_BOUNCED),a                ; кадрами: сектор в D_BOUNCE
        xor a
        ld (ResHalf),a
        ld a,RESP_VDISK_DATA
        ld (ResWant),a
        jp .wait
.got_data:
        ld hl,(ProtoRxLen)
        dec h                           ; 256 байт: H = 1, L = 0
        jr nz,.data_refused
        ld a,l
        or a
        jr nz,.data_refused
        ld a,(ResHalf)
        add a,high D_BOUNCE
        ld d,a
        ld e,low D_BOUNCE
        ld hl,ProtoBuf
        ld bc,256
        ldir
        ld a,(ResHalf)
        or a
        jr nz,.data_done
        inc a
        ld (ResHalf),a
        jp .wait                        ; вторая половина — тем же ожиданием
.data_done:
        xor a
        ld (DIAG+SIG_WHY),a
        ret
.data_refused:                          ; кадр из одного байта — итог отказа ESP, серия кончилась
        ld a,(ProtoBuf)
        ld (DIAG+SIG_RES),a
        ld a,DIAG_RESULT
        jp .why
        ASSERT low D_BOUNCE == 0, половины сектора READN кладутся по старшему байту адреса

; --- сектор серии через DMA (ESP 0.1.128) ----------------------------------------------------
; Обмен «сектор?» (флаг LINK_F_SECTOR) берёт сектор из очереди секторов ESP; ESP выставляет его
; блоком DMA ведомого SPI, Z80 забирает его DMA TS-Config прямо в D_DEST и тем же DMA отсылает
; обратно; итог сверки — в ответе на следующий «сектор?», и в том же ответе ESP обычно выставляет
; уже следующий сектор. Байты сектора процессор не трогает.
; ResSec: 0 — спросить; 1 — выставлен, читать; 2 — прочитан и отослан, ждём итог.
.sec_start:
        ld hl,(D_DEST)
        call Res_SecPlace
        xor a
        ld (ResWant),a                  ; RES_WAIT — сюда же
.sec_go:
        ld de,RES_SEC_TURNS
.sec_step:
        ld a,(ResSec)
        dec a
        jr z,.sec_read
        push de
        ld a,LINK_F_SECTOR
        call SpiLink_Send
        pop de
        ld a,DIAG_LINK
        jp c,.why                       ; ESP не ответила
        ld a,(SpiRsp)
        ld c,a
        ld a,(ResSec)
        or a
        jr nz,.sec_verdict
        bit 1,c                         ; LINK_S_BULK
        jr nz,.sec_offer
        ld a,c
        and LINK_S_STALE|LINK_S_ECHO_BAD
        jr nz,.sec_failed               ; отказ источника, DMA нет, копия трижды неверна
        dec de
        ld a,d
        or e
        jr nz,.sec_step
        ld a,DIAG_WAIT                  ; ESP ещё читает карту: прерывания — драйверу
        ld (DIAG+SIG_WHY),a
        ld a,RES_PENDING
        scf
        ret
.sec_offer:
        bit 2,c                         ; LINK_S_STALE
        jr z,.sec_fresh
        call Res_SecRead                ; чужой старый блок: прочитать и выбросить
        jr .sec_step
.sec_fresh:
        ld a,1
        ld (ResSec),a
.sec_read:
        call Res_SecRead
        call Res_SecEcho
        ld a,2
        ld (ResSec),a
        jr .sec_step
.sec_verdict:
        bit 3,c                         ; LINK_S_ECHO_BAD
        jr z,.sec_good
        bit 1,c
        jr z,.sec_failed                ; трижды неверно: серия оборвана
        ld a,1                          ; тот же сектор выставлен снова
        ld (ResSec),a
        jr .sec_step
.sec_good:
        ld a,c
        and LINK_S_BULK|LINK_S_STALE
        cp LINK_S_BULK
        ld a,1
        jr z,.sec_next                  ; следующий сектор уже выставлен: читать без вопроса
        xor a
.sec_next:
        ld (ResSec),a
        ld a,(D_BOUNCED)
        or a
        call nz,Res_SecBounce
        ld a,(D_COUNT)
        dec a
        call z,Res_Sweep                ; последний сектор вызова: кэш процессора — заново
        xor a
        ld (DIAG+SIG_WHY),a
        ret
.sec_failed:
        ld a,c
        ld (DIAG+SIG_RES),a
        ld a,DIAG_RESULT
        jp .why

; Куда класть сектор с адреса HL. Прямо туда — если адрес чётный (DMA ходит словами), в окне
; #8000 или #C000 и сектор целиком в этом окне; страница — окна #C000 вызывающего (D_CPAGE, у
; резидента там своя) или #8000 (чтением VR_PAGE2). Иначе — в свой буфер ResSecBuf, дальше через
; D_BOUNCE (D_BOUNCED=1).
Res_SecPlace:
        ld a,l
        rra
        jr c,.bounce
        ld a,h
        cp #80
        jr c,.bounce
        and #3F
        cp #3E
        jr c,.direct
        jr nz,.bounce
        ld a,l
        or a
        jr nz,.bounce                   ; хвост сектора ушёл бы в следующее окно
.direct:
        ld (ResDst),hl
        ld a,h
        cp #C0
        ld a,(D_CPAGE)
        jr nc,.page
        ld bc,VR_PAGE2
        in a,(c)
.page:
        ld (ResDstPage),a
        xor a
        ld (D_BOUNCED),a
        ret
.bounce:
        ld hl,ResSecBuf
        ld (ResDst),hl
        ld a,(DIAG+SIG_PAGE)
        ld (ResDstPage),a
        ld a,1
        ld (D_BOUNCED),a
        ret

; Свой буфер -> D_BOUNCE (окно #0000 — WildDOS панели, драйвер оттуда перенесёт в HL). DMA писал
; буфер мимо кэша процессора — сначала вытеснить кэш.
Res_SecBounce:
        call Res_Sweep
        ld hl,ResSecBuf
        ld de,D_BOUNCE
        ld bc,512
        ldir
        ret

; Забрать выставленный блок DMA в (ResDst) страницы ResDstPage — как SpiLink_BulkRead.
Res_SecRead:
        ld bc,VR_DMADADDRL
        ld a,VR_SPI_RDDMA
        call Res_SecOpen
        in a,(ZIFI_SPI_DATA)            ; пошёл первый байт данных: DMA берёт уже принятое
        ld a,VR_DMA_SPI_RAM
        call Res_DmaRun
        ld a,VR_SPI_RD_DONE
        jr Res_DmaDone

; Отослать его обратно на сверку — как SpiLink_BulkEcho.
Res_SecEcho:
        ld bc,VR_DMASADDRL
        ld a,VR_SPI_WRDMA
        call Res_SecOpen
        ld a,VR_DMA_RAM_SPI
        call Res_DmaRun
        ld a,VR_SPI_WR_DONE
; Конец чтения или записи — отдельная транзакция из одной команды A.
Res_DmaDone:
        ld b,a
        ld a,SPI_SELECT_ESP
        out (ZIFI_SPI_CFG),a
        ld a,b
        out (ZIFI_SPI_DATA),a
        in a,(ZIFI_SPI_DATA)
        ld a,SPI_SELECT_NONE
        out (ZIFI_SPI_CFG),a
        ret

; Адрес (ResDst)/ResDstPage — в регистры DMA с порта BC, блок 512 байт; выбрать ESP, отдать
; команду A, байт адреса и пустой байт.
Res_SecOpen:
        push af
        ld hl,(ResDst)
        out (c),l
        inc b
        ld a,h
        and #3F
        out (c),a
        inc b
        ld a,(ResDstPage)
        out (c),a
        ld b,high VR_DMALEN
        ld a,LINK_BULK/2-1
        out (c),a
        ld b,high VR_DMANUM
        xor a
        out (c),a
        ld a,SPI_SELECT_ESP
        out (ZIFI_SPI_CFG),a
        pop af
        out (ZIFI_SPI_DATA),a
        in a,(ZIFI_SPI_DATA)            ; пошёл байт адреса
        in a,(ZIFI_SPI_DATA)            ; пошёл пустой байт
        ret

; Запустить DMA в режиме A, дождаться конца и снять выбор ESP.
Res_DmaRun:
        ld bc,VR_DMACTR
        out (c),a
.wait:
        in a,(c)
        rla
        jr c,.wait
        ld a,SPI_SELECT_NONE
        out (ZIFI_SPI_CFG),a
        ret

; Вытеснить кэш процессора TS-Config: DMA пишет в память мимо него, а окно вызывающего могло быть с
; кэшем. Кэш один на все окна, строки помечены физическим адресом: 512 чтений подряд через любое
; окно с кэшем занимают все строки. Какие окна с кэшем, отсюда не узнать (CacheConfig не читается),
; поэтому — по 512 байт через #4000, #8000 и #C000 (WC включает кэш окну #4000, плагины — и своему
; окну). Читает POP: стек чужой, прерывания закрыты (RES_IO). Портит AF, BC, DE, HL.
Res_Sweep:
        ld (.stack+1),sp
        ld hl,#4000
        ld c,3
.window:
        ld sp,hl
        ld b,512/16
.pop:
        pop de
        pop de
        pop de
        pop de
        pop de
        pop de
        pop de
        pop de
        djnz .pop
        ld de,#4000
        add hl,de
        dec c
        jr nz,.window
.stack:
        ld sp,0
        ret

ResSec:         db 0                    ; серия DMA: 0 — спросить, 1 — читать, 2 — ждать итог
ResDma:         db 0                    ; <>0 — эта серия идёт DMA
ResDst:         dw 0                    ; куда DMA кладёт сектор (окно #8000…#FFFF)
ResDstPage:     db 0                    ; физическая страница ResDst
        ALIGN 2
ResSecBuf:      ds 512                  ; сектор, который в буфер вызывающего DMA не положить

ResHalf:        db 0                    ; READN: половина сектора, которую ждём
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
