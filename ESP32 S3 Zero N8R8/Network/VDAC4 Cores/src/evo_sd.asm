; Карта Evo как вторая SD ZX Next (SD1; ядро Next 0.39+, NEXT.ELF 0.9+). NextZXOS сам подключает её
; FAT-разделы дисками после карты Tang (D: и дальше), в том числе в Browser. Ядро отвечает на
; команды SD само, а сектор CMD17 / CMD24 просит у ESP; ESP кладёт запрос в ответ на ВВОД:
;   [1][7 — чтение / 8 — запись][номер][LBA LE32]
; Плагин исполняет его здесь же, между кадрами Nx_Run:
;   чтение — RDD драйвера WC -> SD_DONE (#76) [номер][итог][LBA][512 байтов] -> F6;
;   запись — сектор кусками SD_PART (#77) [номер][кусок 0…3] -> F7 [номер][кусок][128 байтов]
;            (кадр ESP -> Z80 — не больше 256 байтов), SDD драйвера WC -> SD_DONE [номер][итог][LBA].
; Итог: 0 — да, 2 — драйвер WC отказал (Next получит токен ошибки или ошибку CMD13).
; Сектор — драйвером потока 1, который Nx_SdOn создаёт на SD Z-Controller (API 57: D = 1, B = 0):
; не клон панели — в панели может стоять SD2 (VDAC4 VFS: карта Tang, та же, что у Next SD0).
; Доступ — внутренний вход ядра WildDOS, как обёртки API WC (MD20.ASM, XLD512): страница потока из
; #600B — в окно #0000 (MNG0: тень #6000, порт #10AF), RDD #400C / SDD #400F: DE:HL — LBA, BC —
; буфер, A — секторов; A = ABT, Z — вышло (docs/04-dos-layer.md WC Improved, 8.5). Окно — прежнее.
; После запроса SD_BURST ВВОДов идут без кадра (EvFast): NextZXOS просит сектора подряд, и
; следующий запрос ESP увидит, пока плагин опрашивает сразу.
; Next писал на карту — выход в WC с кодом 3: WC перечитает обе панели, а чтение каталога сбрасывает
; кэш FAT всех потоков (FAT_CACHE_BUMP).
SD_OP_READ      equ 7
SD_OP_WRITE     equ 8
SD_PIECE        equ 128
SD_BURST        equ 40
SD_FAIL         equ 2
CMD_NX_SD_ON    equ #75                 ; [1/0] -> F5 [1 — ядро с SD1, принято]
CMD_NX_SD_DONE  equ #76
CMD_NX_SD_PART  equ #77
RESP_NX_SD_ON   equ #F5
RESP_NX_SD_DONE equ #F6
RESP_NX_SD_PART equ #F7
SD_CORE         equ #0027               ; VERSION ядра Next с SD1
WC_PAGE0        equ #6000               ; тень страницы окна #0000
WC_FILE_PAGE    equ #600B               ; рабочая страница потока (после API 57)
WDOS_RDD        equ #400C
WDOS_SDD        equ #400F

; Включить карту Evo для Next: поток 1 на SD-ZC, затем SD_ON [1]. Не Next 0.39+, нет карты, слой
; не принял — SD1 у Next остаётся пустым слотом (SdOn = 0).
Nx_SdOn:
        xor a
        ld (SdOn),a
        ld (SdBurst),a
        ld de,SD_CORE
        call Nx_IsNext
        ret c
        ld d,1
        ld b,0                          ; SD Z-Controller
        ld c,0
        ld a,FN_STREAM
        call WC_API
        ret nz                          ; карты нет
        ld a,1
        ld (SdAsk),a
        ld hl,SdAsk
        ld bc,1
        ld a,CMD_NX_SD_ON
        call Proto_Send
        ld a,RESP_NX_SD_ON
        ld de,NX_WAIT
        call Proto_WaitCmd
        ret c
        ld a,(ProtoBuf)
        ld (SdOn),a
        ret

; Ответ на ВВОД в сеансе Next с картой Evo (Ev_Service). Выход: CF=0 всегда (сбой канала увидит
; следующий ВВОД, а ESP предложит запрос снова).
Sd_Service:
        ld hl,(ProtoRxLen)
        ld a,h
        or a
        jr nz,.idle
        ld a,l
        cp 7
        jr nz,.idle
        ld a,(ProtoBuf+1)
        cp SD_OP_READ
        jr z,.request
        cp SD_OP_WRITE
        jr nz,.idle
.request:
        ld (SdOp),a
        ld a,(ProtoBuf+2)
        ld (SdTag),a
        ld hl,ProtoBuf+3
        ld de,SdLba
        ld bc,4
        ldir
        ld a,(SdOp)
        cp SD_OP_READ
        call z,Sd_Read
        ld a,(SdOp)
        cp SD_OP_WRITE
        call z,Sd_Write
        ld a,SD_BURST
        ld (SdBurst),a
        jr .fast
.idle:
        ld a,(SdBurst)
        or a
        ret z                           ; CF=0
        dec a
        ld (SdBurst),a
.fast:
        ld a,1
        ld (EvFast),a
        or a
        ret

; Amiga (личность 05, AMIGA.ELF 0.18+): карта Evo — том EVO в Workbench, только чтение. ESP сам
; разбирает FAT и показывает Amiga готовый том FFS, сектора просит здесь (Ev_Service):
;   [1][7][номер][LBA LE32][число 1…AM_SD_MAX] -> на каждый сектор подряд SD_DONE #76
;   [номер][итог][LBA][512 байтов], как у Next; отказ — SD_DONE без сектора, дальше не читать.
; Поток 1 — на SD-ZC, как у Next (в панели может стоять SD2 — VFS, карта Tang). Меню F11 клонирует
; в поток 1 панель (Ev_Walk): поэтому SdStream — поток 1 сейчас наш; клон его снимает, и чтение
; создаёт поток заново. Записи нет: запрос 8 Amiga не шлёт, а пришёл бы — не исполняется.
AM_SD_MAX       equ 16

Am_SdRead:
        ld a,(NxVersion+3)
        cp 5
        jp nz,Am_Burst                  ; не Amiga: у других слоёв операция 7 — не своя
        ld hl,(ProtoRxLen)
        ld de,8
        or a
        sbc hl,de
        jp nz,Am_Burst
        ld a,(ProtoBuf+2)
        ld (SdTag),a
        ld hl,ProtoBuf+3
        ld de,SdLba
        ld bc,4
        ldir
        ld a,(ProtoBuf+7)
        or a
        jr nz,.some
        inc a                           ; ноль — один сектор
.some:
        cp AM_SD_MAX+1
        jr c,.count
        ld a,AM_SD_MAX                  ; больше предела — сколько можно, остальное ESP спросит снова
.count:
        ld (SdCount),a
        ld a,(SdStream)
        or a
        jr nz,.sector
        xor a
        ld (EvCloned),a                 ; меню потом клонирует панель заново
        ld d,1
        ld b,0                          ; SD Z-Controller
        ld c,0
        ld a,FN_STREAM
        call WC_API
        jr nz,.no_card
        ld a,1
        ld (SdStream),a
.sector:
        call Sd_Read
        jr c,.done                      ; ESP не ответил
        ld a,(SdFrame+1)
        or a
        jr nz,.done                     ; отказ драйвера: остальное ESP спросит снова
        ld a,(ProtoBuf)
        or a
        jr z,.done                      ; ESP запрос уже не ждёт
        ld hl,(SdLba)
        inc hl
        ld (SdLba),hl
        ld a,h
        or l
        jr nz,.next
        ld hl,(SdLba+2)
        inc hl
        ld (SdLba+2),hl
.next:
        ld hl,SdCount
        dec (hl)
        jr nz,.sector
.done:
        ld a,SD_BURST
        ld (SdBurst),a
        ld a,1
        ld (EvFast),a
        or a
        ret
.no_card:
        call Sd_Head
        ld a,SD_FAIL
        ld (SdFrame+1),a
        call Sd_SendHead
        jr .done

; Ответ на ВВОД без запроса у Amiga: после чтения ещё SD_BURST ВВОДов без кадра — Amiga читает
; сектора подряд. Выход: CF=0.
Am_Burst:
        ld a,(SdBurst)
        or a
        ret z
        dec a
        ld (SdBurst),a
        ld a,1
        ld (EvFast),a
        or a
        ret

; Чтение: сектор -> SdData, SD_DONE с сектором (отказ — без сектора).
Sd_Read:
        call Sd_Head
        xor a
        call Sd_Io
        ld bc,6+512
        jr z,.send
        ld a,SD_FAIL
        ld (SdFrame+1),a
        ld bc,6
.send:
        ld hl,SdFrame
        ld a,CMD_NX_SD_DONE
        call Proto_Send
        ld a,RESP_NX_SD_DONE
        ld de,NX_WAIT
        jp Proto_WaitCmd

; Запись: четыре куска сектора от ESP -> SdData, SDD, SD_DONE. Кусок не пришёл или не тот — не
; писать: ESP предложит запрос снова.
Sd_Write:
        call Sd_Head
        ld hl,SdData
        xor a
.piece:
        ld (SdPart),a
        ld (SdAsk+1),a
        push hl
        ld a,(SdTag)
        ld (SdAsk),a
        ld hl,SdAsk
        ld bc,2
        ld a,CMD_NX_SD_PART
        call Proto_Send
        ld a,RESP_NX_SD_PART
        ld de,NX_WAIT
        call Proto_WaitCmd
        pop de                          ; куда класть кусок
        ret c
        ld hl,(ProtoRxLen)
        ld bc,2+SD_PIECE
        or a
        sbc hl,bc
        ret nz                          ; не кусок: запроса уже нет
        ld hl,ProtoBuf
        ld a,(SdTag)
        cp (hl)
        ret nz
        inc hl
        ld a,(SdPart)
        cp (hl)
        ret nz
        inc hl
        ld bc,SD_PIECE
        ldir
        ex de,hl
        ld a,(SdPart)
        inc a
        cp 4
        jr c,.piece
        ld a,1
        ld (SdWrote),a                  ; карта меняется: при выходе WC перечитает панели
        call Sd_Io
        jr z,Sd_SendHead
        ld a,SD_FAIL
        ld (SdFrame+1),a
; SD_DONE без сектора: номер, итог, LBA.
Sd_SendHead:
        ld hl,SdFrame
        ld bc,6
        ld a,CMD_NX_SD_DONE
        call Proto_Send
        ld a,RESP_NX_SD_DONE
        ld de,NX_WAIT
        jp Proto_WaitCmd

; SdFrame: номер, итог 0, LBA.
Sd_Head:
        ld a,(SdTag)
        ld (SdFrame),a
        xor a
        ld (SdFrame+1),a
        ld hl,SdLba
        ld de,SdFrame+2
        ld bc,4
        ldir
        ret

; Сектор SdLba <-> SdData драйвером WC потока 1. Вход: A = 0 — чтение, 1 — запись. Выход: Z — вышло.
Sd_Io:
        ld (SdDir),a
        push ix
        push iy
        ld d,1
        ld bc,#FFFF
        ld a,FN_STREAM
        call WC_API                     ; поток 1: его страница — в #600B
        ld a,(WC_PAGE0)
        push af
        ld a,(WC_FILE_PAGE)
        call Sd_Map0
        ld hl,(SdLba)
        ld de,(SdLba+2)
        ld bc,SdData
        ld a,(SdDir)
        or a
        ld a,1
        jr nz,.write
        call WDOS_RDD
        jr .back
.write:
        call WDOS_SDD
.back:
        ld (SdAbt),a
        pop af
        call Sd_Map0
        pop iy
        pop ix
        ld a,(SdAbt)
        or a
        ret

; Страница A — в окно #0000 (как MNG0 WC).
Sd_Map0:
        ld (WC_PAGE0),a
        ld bc,#10AF
        out (c),a
        ret

SdOn:           db 0                    ; 1 — Next видит карту Evo как SD1
SdWrote:        db 0                    ; 1 — Next писал на карту Evo
SdBurst:        db 0
SdStream:       db 0                    ; 1 — поток 1 создан на SD-ZC для Amiga (клон меню его снимает)
SdCount:        db 0
SdOp:           db 0
SdTag:          db 0
SdPart:         db 0
SdDir:          db 0
SdAbt:          db 0
SdLba:          ds 4
SdAsk:          ds 2
SdFrame:        ds 6                    ; SD_DONE: номер, итог, LBA LE32 …
SdData:         ds 512                  ; … и сектор — подряд с заголовком
