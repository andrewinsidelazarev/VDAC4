; Экран плагина на HDMI Tang (0.16): COREMENU.ELF — приложение ESP с microSD Tang (\CORES\, рядом с
; ядрами) — рисует меню, загрузку и ошибки оверлеем Tang (cores_ui, дизайн Codex). Плагин —
; диспетчер, как прежде: список (CORE_LIST), выбор, запуск слоя и ядра; экрану он шлёт только
; что показать. Карточки ядер (значки, картинки, тексты, клавишу Manage) экран берёт из ELF самих
; ядер. Экрана нет (нет COREMENU.ELF, Tang без оверлеев, ESP не дала памяти) — окно WC, как было.
;
; Порядок: CORE_APP COREMENU (общий слой «core» вместо него — экрана нет, APP_STOP); LIST — тело
; CORE_LIST; раз в кадр STATE — что показать, заодно знак жизни (молчание 3 с — экран уходит сам).
; Esc — LEAVE 0 (уход плавно) до флага «экрана нет», затем LEAVE 2 (приложение выгружается).
; Запуск ядра: STATE «Loading», LEAVE 1 до флага «кадр на экране» — и CORE_APP ядра: ESP выгружает
; экран, картинка остаётся на HDMI, пока Tang не сменит ядро. Ошибка после этого — экран заново
; (CORE_APP COREMENU), сразу непрозрачным (#80 в STATE), с текстом ошибки, до Esc.

CMD_SCR_LIST    equ #47                 ; [число][записи CrList] -> C7 [1 — принято]
CMD_SCR_STATE   equ #48                 ; [состояние | #80][выбранный][проценты][ошибка][текст, 0] -> C8 [флаги]
CMD_SCR_LEAVE   equ #49                 ; [0 — уйти / 1 — оставить картинку / 2 — конец] -> C9 [флаги]
RESP_SCR_LIST   equ #C7
RESP_SCR_STATE  equ #C8
RESP_SCR_LEAVE  equ #C9

SCR_F_TANG      equ 1                   ; Tang с оверлеями есть, холст выделен
SCR_F_SHOWN     equ 2                   ; экран виден целиком
SCR_F_DRAWN     equ 4                   ; последнее STATE на экране
SCR_F_GONE      equ 8                   ; экрана нет

SCR_MENU        equ 2                   ; состояния cui_state_t (cores_ui.h)
SCR_LOADING     equ 5
SCR_ERROR       equ 7
SCR_RECEIVING   equ 8                   ; 0.19: образ с карты Evo идёт в экран (COREMENU 0.2), полоса хода
CMD_FILE_BEGIN  equ #4A                 ; [длина LE32][кому, 0] -> CA [1 — принимаю][1 — порции DMA можно]
RESP_FILE_BEGIN equ #CA                 ; #4B пакеты, #4C конец; 0.20 — #4D порции DMA (COREMENU 0.3)
CMD_FILE_RAW    equ #4D                 ; raw media: [смещение LE32][XOR][данные] -> CD [1][принято LE32]
RAW_BUF         equ #E000               ; 0.20: порция DMA — в странице 1 плагина (Page1_Map)
RAW_MAX         equ 8192                ; 16 секторов: предел raw media прошивки (kMediaMax)
LINK_F_MEDIA    equ #10                 ; запрос raw media (spi_link_core.hpp, kFlagMedia)
SCR_DMASADDRL   equ #1AAF               ; DMA TS-Config (как в zifi_spi.asm, там — только с ZIFI_SPI_DMA):
SCR_DMALEN      equ #26AF               ; источник — младший байт, старший, страница; слов в блоке − 1;
SCR_DMACTR      equ #27AF               ; запуск (чтение: бит 7 — занят); блоков − 1
SCR_DMANUM      equ #28AF
SCR_DMA_RAM_SPI equ #82
SCR_SPI_WRDMA   equ #03                 ; команды SPI ESP: запись DMA и её конец
SCR_SPI_WR_DONE equ #07
SCR_ERR_TEXT    equ 6                   ; cui_error_t: текст ошибки ESP
SCR_INSTANT     equ #80                 ; картинка уже на экране: сразу непрозрачный
SCR_WAIT        equ 50                  ; кадров на появление и уход

; CORE_APP COREMENU. Выход: CF=0 — экран запущен (ScrOn = 1); CF=1 — нет.
Scr_Start:
        ld hl,ScrAppName
        ld bc,9
        ld a,CMD_CORE_APP
        call Proto_Send
        ld a,RESP_CORE_APP
        ld b,32                          ; ELF с SD Tang в кэш ESP — до секунд
        call Nx_WaitLong
        ret c
        ld a,(ProtoBuf)                  ; итог int8: 0 — работает
        or a
        scf
        ret nz
        ; Приложение — «coremenu»; «core» — общий слой прошивки: COREMENU.ELF на карте нет
        ld hl,ProtoBuf+1
        ld de,ScrAppTag
        ld b,9
.cmp:
        ld a,(de)
        cp (hl)
        jr nz,.not_menu
        inc hl
        inc de
        djnz .cmp
        ld a,1
        ld (ScrOn),a
        or a
        ret
.not_menu:
        ld a,CMD_APP_STOP
        call Proto_SendEmpty
        ld a,RESP_APP_STOP
        ld b,4
        call Nx_WaitLong
        scf
        ret

; LIST: число и записи CrList (тело ответа CORE_LIST). Выход: CF=1 — ESP не ответила.
Scr_List:
        ld a,(CrCount)
        call Cr_Entry                    ; HL — за последней записью
        ld de,CrCount
        or a
        sbc hl,de
        ld b,h
        ld c,l
        ex de,hl
        ld a,CMD_SCR_LIST
        ld e,RESP_SCR_LIST
        jp Scr_Ask                       ; 0.21: с повтором (см. Scr_Ask)

; Меню экраном: список, затем STATE, пока экран не скажет, что Tang с оверлеями есть. Не сказал
; за SCR_WAIT кадров — экран прочь, окно WC.
Scr_Menu:
        ld a,(ScrOn)
        or a
        ret z
        call Scr_List
        jr c,.none
        ld a,SCR_MENU
        ld hl,0
        ld c,l
        call Scr_Set
        call Scr_Visible
        ret nc
.none:
        call Scr_Close
        jp Ui_Open

; STATE, пока экран не скажет, что Tang с оверлеями есть. Выход: CF=0 — есть; CF=1 — нет за
; SCR_WAIT кадров или ESP не ответила.
Scr_Visible:
        ld b,SCR_WAIT
.wait:
        push bc
        call Scr_State
        pop bc
        ret c
        and SCR_F_TANG
        ret nz                           ; CF=0
        ei
        halt
        djnz .wait
        scf
        ret

; STATE, пока экран не покажет его целиком (флаги «виден» и «кадр на экране»). Нужно перед LEAVE 1,
; если экран ещё не появлялся (файл с панели WC, 0.18): COREMENU.ELF, пока экран выключен (читает
; карточки ядер с карты Tang), на LEAVE 1 его уже не включает — картинки «Loading» не было бы вовсе.
; Выход: CF=0 — виден; CF=1 — нет за 5 с (SCR_WAIT × 5 кадров) или ESP не ответила.
Scr_Shown:
        ld c,5
.outer:
        ld b,SCR_WAIT
.wait:
        push bc
        call Scr_State
        pop bc
        ret c
        and SCR_F_SHOWN|SCR_F_DRAWN
        cp SCR_F_SHOWN|SCR_F_DRAWN
        ret z                            ; CF=0
        ei
        halt
        djnz .wait
        dec c
        jr nz,.outer
        scf
        ret

; 0.21 (09.11.2026, плата: первый запуск MD после записи нового MD.ELF на карту Tang): сразу после ответа
; на LIST задача обмена ESP молчала дольше 16 попыток обмена (~0,4 с), и кадр BEGIN был выброшен
; (SpiLink_Push) — до COREMENU он не дошёл (ни «raw media DMA», ни «receiving» в журнале ESP). 0.20
; ошибку отправки не смотрела, ответа ждала ~0,1 с (NX_WAIT) и молча отдавала образ слою пакетами по 512
; байт: 2 МБ — 70 с, полосы хода нет. Так же мог пропасть и LIST (тогда — окно WC и те же пакеты).
; Команда экрану с повтором: кадр не ушёл — ещё раз (до SCR_ASK_TRIES); ответа — до SCR_ASK_WAIT × NX_WAIT
; опросов (секунды); доклад ESP об ошибке (#EE: у старого COREMENU такой команды нет) — сразу «нет».
; Вход: A — команда, E — код ответа, HL — данные, BC — длина. Выход: CF=0 — ответ в ProtoBuf; CF=1 — нет.
SCR_ASK_TRIES   equ 3
SCR_ASK_WAIT    equ 32
Scr_Ask:
        ld (ScrAskCmd),a
        ld a,e
        ld (ScrAskResp),a
        ld (ScrAskData),hl
        ld a,SCR_ASK_TRIES
.try:
        ld (ScrTries),a
        xor a
        ld (ProtoErr),a
        push bc
        ld hl,(ScrAskData)
        ld a,(ScrAskCmd)
        call Proto_Send
        pop bc
        jr c,.again                      ; кадр не ушёл: ESP молчала дольше 16 попыток обмена
        push bc
        ld b,SCR_ASK_WAIT
.wait:
        push bc
        ld a,(ScrAskResp)
        ld de,NX_WAIT
        call Proto_WaitCmd
        pop bc
        jr nc,.got
        ld a,(ProtoErr)
        or a
        jr nz,.refused                   ; ESP ответила ошибкой: экран такой команды не знает
        djnz .wait
        pop bc
.again:
        ld a,(ScrTries)
        dec a
        jr nz,.try
        scf
        ret
.refused:
        pop bc
        scf
        ret
.got:
        pop bc
        or a
        ret

; 0.19: образ с карты Evo (файл с панели WC) — экрану: COREMENU принимает его, рисуя ход полосой,
; и оставляет слою ядра (прошивка 0.1.123+, ABI 1.14); слой берёт его (#7A [1], Nes_Take), а не
; принимает второй раз. 0.20 — всем ядрам: MD.ELF 1.5, NES.ELF 0.10, AMIGA.ELF 0.27 умеют брать.
; BEGIN [длина LE32][CrName, 0]. Ответ COREMENU 0.3 — [1][1 — порции DMA можно]; порции — ещё и если
; прошивка принимает блоки от Z80 (LINK_S_PUT_CAPABLE в статусе обмена): ScrRaw = 1.
; Выход: CF=0 — экран принимает (ScrFile = 1); CF=1 — нет: экрана нет, он старый (0.1), нет памяти —
; образ пойдёт слою, как прежде.
; 0.21: BEGIN — через Scr_Ask (повтор безопасен: COREMENU на новый BEGIN начинает приём заново).
Scr_FileBegin:
        xor a
        ld (ScrFile),a
        ld (ScrRaw),a
        ld a,(ScrOn)
        or a
        scf
        ret z
        ld hl,NesSize
        ld de,ScrBuf
        ld bc,4
        ldir
        ld hl,CrName
.name:
        ld a,(hl)
        ld (de),a
        inc hl
        inc de
        or a
        jr nz,.name
        ex de,hl
        ld de,ScrBuf
        sbc hl,de                        ; CF=0 после OR
        ld b,h
        ld c,l
        ld hl,ScrBuf
        ld a,CMD_FILE_BEGIN
        ld e,RESP_FILE_BEGIN
        call Scr_Ask
        ret c
        ld a,(ProtoRxLen)
        cp 2
        jr c,.packets                    ; COREMENU 0.2: байта DMA нет
        ld a,(SpiRsp)                    ; статус последнего обмена: бит 7 — блоки от Z80 принимает
        rlca
        ld hl,ProtoBuf+1
        and (hl)
        and 1
        ld (ScrRaw),a
.packets:
        ld a,(ProtoBuf)
        cp 1
        scf
        ret nz
        ld (ScrFile),a
        or a
        ret

; 0.19: тело образа — экрану (пакеты #4B, конец #4C — те же, что у слоя; 0.20 — порции DMA #4D, если
; ScrRaw). Выход: CF=0 — образ у экрана и оставлен слою; CF=1, HL — текст ошибки (0 — Esc).
Scr_SendFile:
        ld a,CMD_FILE_BEGIN
        ld (NesCmd),a
        call Nes_SendBody
        ld a,#77                         ; дальше (повтор с панели) — снова слою
        ld (NesCmd),a
        ret

; 0.20: порция файла (NesCount байт) — в RAW_BUF целыми секторами (страница 1), её XOR и управление
; raw media ScrRawCtl: [#4D][длина LE16][смещение NesSent LE32][XOR]. Выход: CF=1 — WC не прочитал.
Scr_RawRead:
        ld hl,(NesCount)
        dec hl
        ld a,h
        srl a
        inc a
        ld (ScrRawSecs),a
        ld b,a
        call Page1_Map
        ld hl,RAW_BUF
        ld a,FN_LOAD512
        call WC_API
        ei                               ; файловый драйвер мог вернуть DI
        ret c
        call Page1_Map
        ld hl,(NesCount)
        ld (ScrRawCtl+1),hl
        ld hl,NesSent
        ld de,ScrRawCtl+3
        ld bc,4
        ldir
        ; XOR NesCount байтов: группами по 8 (~15 тактов на байт; побайтно с CPI — 41, а это
        ; 24 мс на порцию), затем хвост 0..7
        ld bc,(NesCount)
        ld hl,RAW_BUF
        ld a,c
        and 7
        ld e,a                           ; E — хвост
        srl b
        rr c
        srl b
        rr c
        srl b
        rr c                             ; BC — групп
        ld a,b
        or c
        ld d,0                           ; D — XOR
        jr z,.tail
        ld a,c                           ; счёт DJNZ: B — младший, C — старший (+1, если младший ≠ 0)
        ld c,b
        ld b,a
        or a
        jr z,.groups
        inc c
.groups:
        xor a
.group:
        xor (hl)
        inc hl
        xor (hl)
        inc hl
        xor (hl)
        inc hl
        xor (hl)
        inc hl
        xor (hl)
        inc hl
        xor (hl)
        inc hl
        xor (hl)
        inc hl
        xor (hl)
        inc hl
        djnz .group
        dec c
        jr nz,.group
        ld d,a
.tail:
        ld a,d
        inc e
        jr .tail_next
.byte:
        xor (hl)
        inc hl
.tail_next:
        dec e
        jr nz,.byte
        ld (ScrRawCtl+7),a
        or a
        ret

; 0.20: порция RAW_BUF (ScrRawSecs секторов) — ESP: запрос raw media (флаг #10, управление ScrRawCtl)
; и, когда ESP откроет приём, DMA «ОЗУ → SPI» целыми секторами (лишнее за длиной ESP отбросит), затем
; «записано» (LINK_F_PUT_DONE) — как Fast_Send плеера VDAC4MP3. Очередь ESP занята — ждать её по кадру,
; не дольше SCR_WAIT×2 кадров. Выход: CF=0 — ESP приняла порцию (ответ CD придёт обычным путём); CF=1 — нет.
Scr_RawPut:
        call Page1_Map                   ; страницу для DMA берём из тени WC_PAGE3
        ld a,SCR_WAIT*2
        ld (ScrRawWait),a
.ask:
        ld hl,ScrRawCtl
        ld de,SpiTxBuf
        ld bc,8
        ldir
        ld a,8
        ld (SpiTxLen),a                  ; обмен уносит только управление
        ld hl,SpiRsp+3
        ld (SpiDest),hl                  ; данных в ответ не будет
        xor a
        ld (SpiRoom),a
        ld a,LINK_F_MEDIA
        call SpiLink_Turn
        ret c
        ld a,(SpiRsp)
        and LINK_S_PUT_READY
        jr nz,.ready
        ld hl,ScrRawWait
        dec (hl)
        scf
        ret z
        ei
        halt                             ; ESP разбирает очередь
        jr .ask
.ready:
        ld hl,RAW_BUF
        ld bc,SCR_DMASADDRL
        out (c),l
        inc b
        ld a,h
        and #3F
        out (c),a
        inc b
        ld a,(WC_PAGE3)
        out (c),a
        ld b,high SCR_DMALEN
        ld a,255                         ; блок — сектор: 256 слов
        out (c),a
        ld b,high SCR_DMANUM
        ld a,(ScrRawSecs)
        dec a
        out (c),a
        ld a,SPI_SELECT_ESP
        out (ZIFI_SPI_CFG),a
        ld a,SCR_SPI_WRDMA
        out (ZIFI_SPI_DATA),a
        in a,(ZIFI_SPI_DATA)             ; пошёл байт адреса
        in a,(ZIFI_SPI_DATA)             ; пошёл пустой байт
        ld b,high SCR_DMACTR
        ld a,SCR_DMA_RAM_SPI
        out (c),a
.dma:
        in a,(c)
        rla
        jr c,.dma
        ld a,SPI_SELECT_NONE
        out (ZIFI_SPI_CFG),a
        ld a,SPI_SELECT_ESP              ; конец записи — отдельная транзакция из одной команды
        out (ZIFI_SPI_CFG),a
        ld a,SCR_SPI_WR_DONE
        out (ZIFI_SPI_DATA),a
        in a,(ZIFI_SPI_DATA)
        ld a,SPI_SELECT_NONE
        out (ZIFI_SPI_CFG),a
        ld a,LINK_F_PUT_DONE
        call SpiLink_Send
        ret c
        ld a,(SpiRsp)
        and LINK_S_PUT_OK
        ret nz                           ; CF=0
        scf
        ret

; Что показывать: A — состояние, C — код ошибки, HL — текст ошибки (0 — нет).
Scr_Set:
        ld (ScrSt),a
        ld a,c
        ld (ScrCode),a
        ld (ScrText),hl
        ret

; STATE по ScrSt, MenuSel, ScrCode, ScrText. Выход: CF=0, A — флаги; CF=1 — ESP не ответила.
Scr_State:
        ld de,ScrBuf
        ld a,(ScrSt)
        ld (de),a
        inc de
        ld a,(MenuSel)
        ld (de),a
        inc de
        ld a,#FF                         ; процентов плагин не знает
        ld (de),a
        inc de
        ld a,(ScrCode)
        ld (de),a
        inc de
        ld hl,(ScrText)
        ld a,h
        or l
        jr z,.end
        ld b,60
.copy:
        ld a,(hl)
        or a
        jr z,.end
        ld (de),a
        inc hl
        inc de
        djnz .copy
.end:
        xor a
        ld (de),a
        inc de
        ex de,hl
        ld de,ScrBuf
        or a
        sbc hl,de
        ld b,h
        ld c,l
        ld hl,ScrBuf
        ld a,CMD_SCR_STATE
Scr_Command:
        ld (ScrCmd),a
        call Proto_Send
        ld a,(ScrCmd)
        or #80                           ; ответ — код команды | #80
        ld de,NX_WAIT
        call Proto_WaitCmd
        ret c
        ld a,(ProtoBuf)
        ret

; LEAVE A. Выход: CF=0, A — флаги; CF=1 — ESP не ответила.
Scr_Leave:
        ld (ScrBuf),a
        ld hl,ScrBuf
        ld bc,1
        ld a,CMD_SCR_LEAVE
        jr Scr_Command

; Экран уходит плавно (до флага «экрана нет», не дольше SCR_WAIT кадров), затем выгружается.
Scr_Close:
        ld a,(ScrOn)
        or a
        ret z
        ld b,SCR_WAIT
.wait:
        push bc
        xor a
        call Scr_Leave
        pop bc
        jr c,.done
        and SCR_F_GONE
        jr nz,.done
        ei
        halt
        djnz .wait
.done:
        ld a,2
        call Scr_Leave
        xor a
        ld (ScrOn),a
        ret

; Перед CORE_APP ядра: «Loading» на экран и оставить его там. Дальше ESP выгрузит экран, а
; картинка останется на HDMI до смены ядра.
Scr_Keep:
        ld a,(ScrOn)
        or a
        ret z
        ld a,SCR_LOADING
        ld hl,0
        ld c,l
        call Scr_Set
        call Scr_State
        ld b,SCR_WAIT
.wait:
        push bc
        ld a,1
        call Scr_Leave
        pop bc
        jr c,.kept
        and SCR_F_SHOWN|SCR_F_DRAWN
        cp SCR_F_SHOWN|SCR_F_DRAWN
        jr z,.kept
        ei
        halt
        djnz .wait
.kept:
        xor a
        ld (ScrOn),a
        inc a
        ld (ScrKept),a
        ret

; Ошибка экраном: HL — текст плагина. Экран запущен или его картинка оставлена на HDMI — показать
; ошибку и ждать Esc. Выход: CF=0 — показана и закрыта; CF=1 — экрана нет, HL — тот же текст.
Scr_Fatal:
        ld (ScrErrSrc),hl
        ld a,(ScrOn)
        or a
        jr nz,.show
        ld a,(ScrKept)
        or a
        jr z,.none
        call Scr_Start                   ; слой ядра выгружен — экран снова
        jr c,.none
.show:
        call Scr_List                    ; ошибка CORE_LIST — список пуст, экрану он нужен всё равно
        ld hl,(ScrErrSrc)
        call Scr_ErrCode
        ld a,(ScrKept)
        or a
        ld a,SCR_ERROR
        jr z,.set
        or SCR_INSTANT                   ; «Loading» ещё на HDMI — без проявления с нуля
.set:
        call Scr_Set
        call Scr_Visible
        jr c,.no_tang
.wait_esc:
        call Scr_State                   ; заодно знак жизни экрану
        ei
        halt
        call Nx_Esc
        jr z,.wait_esc
        call Scr_Close
        xor a
        ld (ScrKept),a
        ret
.no_tang:
        call Scr_Close                   ; Tang без оверлеев — ошибка в окне WC
.none:
        xor a
        ld (ScrKept),a
        ld hl,(ScrErrSrc)
        scf
        ret

; Текст ошибки HL → C — код cui_error_t, HL — текст для экрана (0 — у экрана свой). Текст ESP и
; прочие тексты плагина — кодом «текст ESP» с самим текстом.
Scr_ErrCode:
        ex de,hl
        ld hl,ScrErrMap
.next:
        ld a,(hl)
        cp #FF
        jr z,.text
        ld c,a
        inc hl
        ld a,(hl)
        inc hl
        cp e
        jr nz,.skip
        ld a,(hl)
        cp d
        jr z,.found
.skip:
        inc hl
        jr .next
.found:
        ld hl,0
        ret
.text:
        ld c,SCR_ERR_TEXT
        ex de,hl
        ret

ScrErrMap:
        db 0
        dw UiErrNoCores
        db 2
        dw UiErrApp
        db 3
        dw UiErrTimeout
        db 4
        dw UiErrCore
        db 5
        dw UiErrLost
        db 7
        dw NesErrMenu
        db 9
        dw UiErrNoAnswer
        db #FF

ScrAppName:     db "COREMENU",0
ScrAppTag:      db "coremenu",0
ScrOn:          db 0                    ; 1 — экран (COREMENU.ELF) запущен
ScrFile:        db 0                    ; 1 — образ с карты Evo принимает экран (0.19)
ScrRaw:         db 0                    ; 1 — экрану порциями DMA (0.20); иначе пакетами по 512
ScrRawSecs:     db 0                    ; секторов в порции
ScrRawWait:     db 0
ScrRawCtl:      db CMD_FILE_RAW         ; управление raw media: [#4D][длина LE16][смещение LE32][XOR]
                ds 7
ScrTries:       db 0                    ; 0.21: Scr_Ask — попыток осталось, команда, код ответа, данные
ScrAskCmd:      db 0
ScrAskResp:     db 0
ScrAskData:     dw 0
ScrKept:        db 0                    ; 1 — экран выгружен, его «Loading» оставлен на HDMI
ScrCmd:         db 0
ScrSt:          db 0
ScrCode:        db 0
ScrText:        dw 0
ScrErrSrc:      dw 0
ScrBuf:         ds 5+60
