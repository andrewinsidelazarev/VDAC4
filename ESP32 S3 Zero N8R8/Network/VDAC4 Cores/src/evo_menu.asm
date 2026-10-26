; Меню F1 и снимки F5/F6 ядра NES (NES.ELF 0.5+/0.6+): карта Evo во время игры. ESP кладёт запрос в ответ на ВВОД
; (RESP_NX_INPUT: [ok][операция]…), плагин его исполняет здесь же, между кадрами Nx_Run.
; Поток 1 — клон активной панели WC (как у сохранений, Nes_SavePrepare): каждый запрос идёт от
; корня его тома, панель WC и её поток 0 не трогаются. Имена и путь — байты WC как есть.
;   1 LIST [пропуск LE16][до EV_MAX][путь, 0] -> #6E [итог: 0 — да, 1 — нет пути][ещё][число]
;          [флаги (бит 0 — каталог), имя ≤ EV_NAME, 0]… — каталоги и *.nes без "." и ".."
;   2 READ [номер LE16][путь, 0] -> файл прежними #77/#78/#79 (Nes_Send); отказ — #6F [код]:
;          1 — нет такого файла, 2 — чтение или передача не вышли
; Снимки F5/F6 (NES.ELF 0.6+): файл <игра>.sst рядом с образом игры с карты Evo.
;   3 SS_OPEN [длина LE32] -> #6F [0 — открыт временный <игра>.sts; 1 — нет игры или каталога;
;          2 — не создать; 3 — WC без FILEX]
;   4 SS_PUT [смещение LE32][128][данные][CRC16] -> #6F [0; 2 — запись или проверка; 4 — не тот пакет]
;   5 SS_COMMIT -> #6F [0 — .sts заменил .sst; 2 — нет]
;   6 SS_READ -> .sst прежними #77/#78/#79 (Nes_Send) или #6F [1 — нет файла; 2 — чтение]
; Игра: GameKind 0 — каталог панели WC (запуск по ассоциации, имя — NesNamePtr), 1 — путь
; GamePath (образ из меню F1). После OPEN/PUT следующий ВВОД — сразу, без кадра (EvFast).
EV_LIST         equ 1
EV_READ         equ 2
EV_SS_OPEN      equ 3
EV_SS_PUT       equ 4
EV_SS_COMMIT    equ 5
EV_SS_READ      equ 6
EV_SS_CHUNK     equ 128
EV_MAX          equ 8
EV_NAME         equ 52
; Запись FINDNEXT (API 58) при маске FINDNEXT_SIZE: [размер 4][флаг][имя, 0] — поля даты и времени
; есть, только если их попросить (docs/04-dos-layer.md WC Improved, 7.16). До 0.10 флаг и имя
; читались с 8 и 9, как при размере + дате + времени: на плате имена теряли по 4 первых знака,
; а флаг брался из четвёртой буквы имени (часть каталогов пропадала, часть файлов шла каталогами).
EV_FLAG         equ 4
EV_ENAME        equ 5

; Вход в Nx_Run: поток 1 клонируется заново только при первом запросе меню в сеансе — без
; меню (ESP старше NES.ELF 0.5) поток 1 остаётся, каким его сделал Nes_SavePrepare.
Ev_Reset:
        xor a
        ld (EvCloned),a
        ld (EvFast),a
        ld (EvSsOpen),a
        ld (GameKind),a
        ld (GameName),a
        ld a,(NesCall)
        or a
        ret nz                          ; не по ассоциации файла — имени нет
        ld hl,(NesNamePtr)                ; игра, запущенная из панели WC
        ld a,h
        or l
        ret z
        ld de,GameName
        ld bc,255
.copy:
        ld a,(hl)
        ld (de),a
        or a
        ret z
        inc hl
        inc de
        dec bc
        ld a,b
        or c
        jr nz,.copy
        xor a
        ld (GameName),a                 ; длиннее буфера — снимков нет
        ret

; После ответа на ВВОД. Выход: CF=0 всегда (сбой канала увидит следующий ВВОД).
Ev_Service:
        xor a
        ld (EvFast),a
        ld hl,(ProtoRxLen)
        ld a,h
        or a
        jr nz,.has
        ld a,l
        cp 2
        jr c,.none                      ; [ok] без запроса
        ld a,(ProtoBuf+1)
        cp EV_SS_COMMIT
        jp z,Ev_SsCommit
        cp EV_SS_READ
        jp z,Ev_SsRead
        ld a,l
        cp 4
        jr c,.none
.has:
        ld a,(ProtoBuf+1)
        cp EV_LIST
        jp z,Ev_List
        cp EV_READ
        jp z,Ev_Read
        cp EV_SS_OPEN
        jp z,Ev_SsOpen
        cp EV_SS_PUT
        jp z,Ev_SsPut
.none:
        or a
        ret

; Путь запроса (HL) — в EvPath. CF=1 — длиннее буфера.
Ev_TakePath:
        ld de,EvPath
        ld bc,255
.copy:
        ld a,(hl)
        ld (de),a
        or a
        ret z
        inc hl
        inc de
        dec bc
        ld a,b
        or c
        jr nz,.copy
        scf
        ret

; Выбрать поток 1 (перед каждым файловым вызовом, как FTP-плагин)
Ev_Select:
        ld d,1
        ld bc,#FFFF
        ld a,FN_STREAM
        jp WC_API

; Поток 1 — в корень его тома, затем по компонентам EvPath (каталоги). CF=1 — нет пути.
Ev_Walk:
        ld a,(EvCloned)
        or a
        jr nz,.cloned
        inc a
        ld (EvCloned),a
        ld d,STREAM_CLONE
        ld a,FN_STREAM
        call WC_API
.cloned:
        call Ev_Select
        ld d,STREAM_ROOT
        ld bc,#FFFF
        ld a,FN_STREAM
        call WC_API
        ld hl,EvPath
.skip:
        ld a,(hl)
        cp '/'
        jr nz,.part
        inc hl
        jr .skip
.part:
        or a
        ret z                           ; CF=0: дошли
        ld de,EvEnt+1
        ld b,255
.copy:
        ld a,(hl)
        or a
        jr z,.end
        cp '/'
        jr z,.end
        ld (de),a
        inc de
        inc hl
        djnz .copy
        scf
        ret
.end:
        xor a
        ld (de),a
        push hl
        ld a,#10                        ; искать каталог
        ld (EvEnt),a
        call Ev_Select
        ld hl,EvEnt
        ld a,FN_FENTRY
        call WC_API
        jr z,.fail
        ld a,FN_GDIR
        call WC_API
        pop hl
        jr .skip
.fail:
        pop hl
        scf
        ret

; Начать перечисление каталога потока 1 (A=1 — как в FTP-плагине и WC)
Ev_OpenDir:
        call Ev_Select
        ld a,1
        ex af,af'
        ld a,FN_ADIR
        jp WC_API

; Следующая подходящая запись в EvDir: NZ — есть (EvFlag: 1 — каталог), Z — каталог кончился.
; Раскладка FINDNEXT с маской FINDNEXT_SIZE: 0..3 размер, EV_FLAG флаг, EV_ENAME имя с нулём.
Ev_Next:
        call Ev_Select
        ld de,EvDir
        ld a,FINDNEXT_SIZE
        ex af,af'
        ld a,FN_FINDNEXT
        call WC_API
        ret z
        ld a,(EvDir+EV_ENAME)
        cp '.'
        jr nz,.name
        ld a,(EvDir+EV_ENAME+1)
        or a
        jr z,Ev_Next                    ; "."
        cp '.'
        jr nz,.name
        ld a,(EvDir+EV_ENAME+2)
        or a
        jr z,Ev_Next                    ; ".."
.name:
        ld a,(EvDir+EV_FLAG)
        and #08                         ; метка тома
        jr nz,Ev_Next
        ld a,(EvDir+EV_FLAG)
        and #10
        jr z,.file
        ld a,1
        ld (EvFlag),a
        or a                            ; NZ
        ret
.file:
        xor a
        ld (EvFlag),a
        ld hl,EvDir+EV_ENAME
        ld b,0
.len:
        ld a,(hl)
        or a
        jr z,.ext
        inc hl
        inc b
        jr nz,.len
        jr Ev_Next                      ; имя длиннее 255 — не наше
.ext:
        ld a,b
        cp 5
        jr c,Ev_Next                    ; короче "x.nes"
        dec hl
        ld a,(hl)
        or #20
        cp 's'
        jr nz,Ev_Next
        dec hl
        ld a,(hl)
        or #20
        cp 'e'
        jr nz,Ev_Next
        dec hl
        ld a,(hl)
        or #20
        cp 'n'
        jr nz,Ev_Next
        dec hl
        ld a,(hl)
        cp '.'
        jr nz,Ev_Next
        or a                            ; NZ ('.')
        ret

; --- LIST ----------------------------------------------------------------------------------
Ev_List:
        ld hl,(ProtoBuf+2)
        ld (EvSkip),hl
        ld a,(ProtoBuf+4)
        or a
        jr z,.max
        cp EV_MAX+1
        jr c,.want
.max:
        ld a,EV_MAX
.want:
        ld (EvWant),a
        xor a
        ld (EvCount),a
        ld (EvMore),a
        ld (EvOut),a
        ld hl,EvOut+3
        ld (EvPtr),hl
        ld hl,ProtoBuf+5
        call Ev_TakePath
        jr c,.no_path
        call Ev_Walk
        jr c,.no_path
        call Ev_OpenDir
.next:
        call Ev_Next
        jr z,.send
        ld hl,(EvSkip)
        ld a,h
        or l
        jr z,.take
        dec hl
        ld (EvSkip),hl
        jr .next
.take:
        ld a,(EvWant)
        ld b,a
        ld a,(EvCount)
        cp b
        jr c,.store
        ld a,1
        ld (EvMore),a                   ; за полной пачкой есть ещё подходящая запись
        jr .send
.store:
        inc a
        ld (EvCount),a
        ld de,(EvPtr)
        ld a,(EvFlag)
        ld (de),a
        inc de
        ld hl,EvDir+EV_ENAME
        ld b,EV_NAME
.copy:
        ld a,(hl)
        or a
        jr z,.copied
        ld (de),a
        inc de
        inc hl
        djnz .copy
.copied:
        xor a
        ld (de),a
        inc de
        ld (EvPtr),de
        jr .next
.no_path:
        ld a,1
        ld (EvOut),a
.send:
        ld a,(EvMore)
        ld (EvOut+1),a
        ld a,(EvCount)
        ld (EvOut+2),a
        ld hl,(EvPtr)
        ld de,EvOut
        or a
        sbc hl,de
        ld b,h
        ld c,l
        ld hl,EvOut
        ld a,#6E
        call Proto_Send
        ld a,#EE
        ld de,NX_WAIT
        call Proto_WaitCmd
        or a
        ret

; --- READ ----------------------------------------------------------------------------------
Ev_Read:
        ld hl,(ProtoBuf+2)
        ld (EvSkip),hl                  ; номер записи
        ld hl,ProtoBuf+4
        call Ev_TakePath
        jr c,.missing
        call Ev_Walk
        jr c,.missing
        call Ev_OpenDir
.next:
        call Ev_Next
        jr z,.missing
        ld hl,(EvSkip)
        ld a,h
        or l
        jr z,.found
        dec hl
        ld (EvSkip),hl
        jr .next
.found:
        ld a,(EvFlag)
        or a
        jr nz,.missing                  ; каталог, не образ
        xor a
        ld (EvEnt),a                    ; искать файл
        ld hl,EvDir+EV_ENAME
        ld de,EvEnt+1
.name:
        ld a,(hl)
        ld (de),a
        inc hl
        inc de
        or a
        jr nz,.name
        ld hl,EvEnt
        call Ev_SendEntry
        jp c,Ev_Status
        ld hl,EvPath                    ; игра теперь с карты Evo: снимки — рядом с ней
        ld de,GamePath
        ld bc,256
        ldir
        ld hl,EvEnt+1
        ld de,GameName
        ld bc,256
        ldir
        ld a,1
        ld (GameKind),a
        or a
        ret
.missing:
        ld a,1
; Итог запроса A — #6F [A]. Выход: CF=0.
Ev_Status:
        ld (EvOut),a
        ld hl,EvOut
        ld bc,1
        ld a,#6F
        call Proto_Send
        ld a,#EF
        ld de,NX_WAIT
        call Proto_WaitCmd
        or a
        ret

; Файл записи HL ([атрибут][имя, 0]) потока 1 — ESP прежними #77/#78/#79 (Nes_Send).
; CF=1 — нет: A — код отказа (1 — нет файла, 2 — чтение или передача).
Ev_SendEntry:
        push hl
        call Ev_Select
        pop hl
        ld a,FN_FENTRY
        call WC_API
        ld a,1
        scf
        ret z
        ld (NesSize),hl
        ld (NesSize+2),de
        ld a,FN_GFILE
        call WC_API
        ld a,1
        ld (NesNoEsc),a                 ; в игре Esc принадлежит игре, не прерывает чтение
        call Nes_Send
        push af
        xor a
        ld (NesNoEsc),a
        pop af
        ld a,2
        ret                             ; CF — от Nes_Send

; --- снимки -------------------------------------------------------------------------------
; Поток 1 — в каталог игры. CF=1 — игры с карты Evo нет или каталога нет.
Ev_GameDir:
        ld a,(GameName)
        or a
        scf
        ret z
        ld a,(GameKind)
        or a
        jr nz,.path
        ld a,1
        ld (EvCloned),a                 ; клон панели заново: поток 1 — в каталог запуска
        ld d,STREAM_CLONE
        ld a,FN_STREAM
        call WC_API
        or a
        ret
.path:
        ld hl,GamePath
        ld de,EvPath
        ld bc,256
        ldir
        jp Ev_Walk

; NsEntry — [0][<игра>.sst, 0], NsTemp — [0][<игра>.sts, 0], NsNameLen — атрибут + имя + NUL.
; CF=1 — имя короче "x.nes" или без точки перед расширением.
Ev_SsName:
        ld hl,GameName
        ld de,NsEntry+1
        ld bc,0
.copy:
        ld a,(hl)
        ld (de),a
        inc hl
        inc de
        inc bc
        or a
        jr nz,.copy
        ld a,c
        cp 6                            ; "x.nes" и NUL
        ret c
        inc bc
        ld (NsNameLen),bc
        ex de,hl
        ld de,-5
        add hl,de                       ; точка расширения
        ld a,(hl)
        cp '.'
        scf
        ret nz
        inc hl
        ld (hl),'s'
        inc hl
        ld (hl),'s'
        inc hl
        ld (hl),'t'
        xor a
        ld (NsEntry),a                  ; искать файл
        ld hl,NsEntry
        ld de,NsTemp
        ld bc,(NsNameLen)
        ldir
        dec de
        dec de
        ld a,'s'
        ld (de),a                       ; .sts
        dec de
        ld a,'t'
        ld (de),a
        or a
        ret

Ev_SsOpen:
        xor a
        ld (EvSsOpen),a
        call Ev_GameDir
        ld a,1
        jp c,Ev_Status
        call Ev_SsName
        ld a,1
        jp c,Ev_Status
        call Ns_Select
        call Ns_FilexReset
        call Ns_Filex
        ld a,3
        jp nz,Ev_Status
        ld a,(NsFx+24)
        and #53                         ; READ_AT, WRITE_AT, MOVE, CURRENT_DIR
        cp #53
        ld a,3
        jp nz,Ev_Status
        ld hl,ProtoBuf+2
        ld de,NsSize
        ld bc,4
        ldir
        call Ns_Select
        ld hl,NsTemp
        ld a,FN_FENTRY
        call WC_API
        jr z,.create                    ; прежнего .sts нет
        call Ns_Select
        ld hl,NsTemp
        ld a,FN_DELETE
        call WC_API
        ld a,2
        jp z,Ev_Status
.create:
        ld hl,NsSize
        ld de,NsCreate+1
        ld bc,4
        ldir
        ld hl,NsTemp+1
        ld de,NsCreate+5
        ld bc,(NsNameLen)
        dec bc
        ldir
        call Ns_Select
        ld hl,NsCreate
        ld a,FN_MKFILE
        call WC_API
        ld a,2
        jp nz,Ev_Status
        call Ns_Select
        ld hl,NsTemp
        ld a,FN_FENTRY
        call WC_API
        ld a,2
        jp z,Ev_Status
        call Ns_OffsetZero
        ld hl,0
        ld (EvSsAt),hl
        ld (EvSsAt+2),hl
        ld a,1
        ld (EvSsOpen),a
Ev_SsOk:
        ld a,1
        ld (EvFast),a
        xor a
        jp Ev_Status

; Часть по порядку — в сектор NsSector; полный сектор — WRITE_AT и чтение обратно.
Ev_SsPut:
        ld a,(EvSsOpen)
        or a
        ld a,4
        jp z,Ev_Status
        ld a,(ProtoBuf+6)
        cp EV_SS_CHUNK
        ld a,4
        jp nz,Ev_Status
        ld hl,ProtoBuf+2
        ld bc,5+EV_SS_CHUNK
        call Nes_Crc16
        ld a,(hl)
        cp e
        ld a,4
        jp nz,Ev_Status
        inc hl
        ld a,(hl)
        cp d
        ld a,4
        jp nz,Ev_Status
        ld hl,ProtoBuf+2
        ld de,EvSsAt
        ld bc,4
        call Ns_Compare
        ld a,4
        jp c,Ev_Status                  ; не следующая часть
        ld hl,(EvSsAt)
        ld a,h
        and 1
        ld h,a
        ld de,NsSector
        add hl,de
        ex de,hl
        ld hl,ProtoBuf+7
        ld bc,EV_SS_CHUNK
        ldir
        ld hl,(EvSsAt)
        ld de,EV_SS_CHUNK
        add hl,de
        ld (EvSsAt),hl
        ld hl,(EvSsAt+2)
        ld de,0
        adc hl,de
        ld (EvSsAt+2),hl
        ld a,(EvSsAt)
        or a
        jp nz,Ev_SsOk
        ld a,(EvSsAt+1)
        and 1
        jp nz,Ev_SsOk                   ; сектор ещё не полон
        ld hl,NsSector
        ld de,NsVerify
        ld bc,512
        ldir
        call Ns_Select
        ld a,2                          ; WRITE_AT, ровно 512 байтов
        call Ns_SectorIo
        ld a,2
        jp nz,Ev_SsFail
        call Ns_Select
        ld a,1                          ; чтение обратно
        call Ns_SectorIo
        ld a,2
        jp nz,Ev_SsFail
        ld hl,NsSector
        ld de,NsVerify
        ld bc,512
        call Ns_Compare
        ld a,2
        jp c,Ev_SsFail
        call Ns_OffsetNext
        jp Ev_SsOk
Ev_SsFail:
        push af
        xor a
        ld (EvSsOpen),a
        pop af
        jp Ev_Status

; Весь файл записан — .sts заменяет .sst (FILEX MOVE | REPLACE в каталоге игры).
Ev_SsCommit:
        ld a,(EvSsOpen)
        or a
        ld a,2
        jp z,Ev_Status
        xor a
        ld (EvSsOpen),a
        ld hl,EvSsAt
        ld de,NsSize
        ld bc,4
        call Ns_Compare
        ld a,2
        jp c,Ev_Status
        call Ns_Select
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
        jr z,.done
        cp FILEX_STATUS_COMMITTED_CLEANUP
        ld a,2
        jp nz,Ev_Status
.done:
        xor a
        jp Ev_Status

Ev_SsRead:
        call Ev_GameDir
        ld a,1
        jp c,Ev_Status
        call Ev_SsName
        ld a,1
        jp c,Ev_Status
        ld hl,NsEntry
        call Ev_SendEntry
        jp c,Ev_Status
        or a
        ret

EvCloned:       db 0
EvFast:         db 0
EvSsOpen:       db 0
EvSsAt:         ds 4
GameKind:       db 0
EvSkip:         dw 0
EvWant:         db 0
EvCount:        db 0
EvMore:         db 0
EvFlag:         db 0
EvPtr:          dw 0
EvPath:         ds 256
EvEnt:          ds 1+256
EvDir:          ds 9+256+1
EvOut:          ds 3+EV_MAX*(1+EV_NAME+1)
GamePath:       ds 256
GameName:       ds 256
