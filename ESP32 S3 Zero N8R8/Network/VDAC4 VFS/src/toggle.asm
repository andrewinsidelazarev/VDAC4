; VDAC4VFS.WMF — переключатель VDAC4 VFS (меню F10): SD2 Wild Commander -> диск ESP и обратно.
; Окно с переключателями (13.11.2026): «SD-Card 2» — второй SD Evo, «SD Tang» — сама карта Tang с
; записью (ESP отдаёт её WC: MODE 1 — FatFs ESP карту не монтирует, ядро Tang не меняется), «VFS» —
; том VFS с папками источников, только чтение; по умолчанию выбран «SD-Card 2». Возврат к SD-Card 2
; карту Tang отдаёт ESP (MODE 0); не вышло — её вернёт резидент при следующем старте WC.
;
; Включение: сверить WC Improved v1.11i побайтно (wc111i.inc), найти страницу резидента VFSRES.WMF
; по подписи, убедиться, что прошивка ESP умеет VDISK (INFO), сбросить её том (RESET), поставить
; переходник перед DOS_SWP и «JP WC_TRAMP» на месте «LD HL,DR2» ветки A=6. Выключение — вернуть
; эталонные байты. Панели, что стоят на SD2, перемонтируются как при выборе диска (как MEDIA_IDLE
; ядра: (IX+#1C)=0, DRACT, ORIGA, затем DRACT, XPAN, STATUS, XPAN); при возврате SD-Card 2 они
; переходят на диск 1: (13.11.2026, по слову пользователя: на них был VFS или SD Tang, а не второй
; SD — показывать его вместо них незачем). Этот код WC ждёт свою
; страницу #02 в окне #8000, а там — плагин; поэтому перемонтирование идёт «перелётом»: копия
; R_START лежит в свободной после запуска области страницы #02 (WC_FREE02), туда и обратно ведёт
; MNG8 — возврат из него приходит уже в другую страницу по тому же адресу.

        DEVICE ZXSPECTRUM128
        INCLUDE "wc_api.inc"
        INCLUDE "wc111i.inc"
        INCLUDE "vfs_res.inc"

T_POLLS         equ 30000               ; опросов ответа ESP

startCode:
        ORG #0000
        ds 16
        db "WildCommanderMDL"
        db #0A
        db 0
        db 1                            ; страниц: одна
        db 0                            ; страница 0 — в окне #8000
        db 0,(pageEnd-pageStart+511)/512
        ds 10
        ds 16
        ds 96                           ; расширений нет
        db 0
        dd 0
.name:  db "VDAC4 VFS: SD2 <-> ESP"
        ds 32-($-.name),' '
        db #03                          ; меню F10
        ds 512-$,0

pageStart:
        DISP #8000
PLUGIN:
        push ix
        ld (T_IX),ix
        ld a,(#6002)
        ld (T_Own),a
        call T_Main
        call T_Show
        pop ix
        xor a
        ret

; Всё, кроме окон: выбор (T_Select) и смена режима. Итог, если есть что сказать, — в T_Msg/T_Msg2;
; T_Err — ошибка (окно красное, с подробностями для разбора). Удачная смена окна итога не даёт:
; панели на SD2 переключатель перемонтирует сам.
T_Main:
        xor a
        ld (T_Err),a
        ld (T_Kept),a
        ld hl,0
        ld (T_Msg),hl
        ld (T_Msg2),hl
        call T_CheckWc
        ld hl,T_ErrWc
        jp c,.error
        call T_FindRes
        ld hl,T_ErrRes
        jp c,.error
        call T_Diag                     ; что было с драйвером до этого запуска
        call T_SelfTest                 ; код резидента: BEGIN и INFO, как у драйвера
        ; что сейчас на SD2: переходника нет — второй SD; есть — режим из подписи резидента
        xor a
        ld (T_Cur),a
        ld a,(T_On)
        or a
        jr z,.cur_known
        call T_GetState
        cp STATE_TANG
        jr z,.cur_set
        ld a,STATE_VFS
.cur_set:
        ld (T_Cur),a
.cur_known:
        call T_Select                   ; A — выбранный режим; CF=1 — Esc
        ret c                           ; отмена: SD2 как был, окна итога нет
        ld hl,T_Cur
        cp (hl)
        ret z                           ; выбран тот, что уже стоит
        ld (T_Mode),a
        or a
        jp z,.to_sd2
        call T_Esp                      ; у прошивки есть VDISK; её том — заново
        ld hl,T_ErrEsp
        jp c,.error
        ld a,(T_Mode)
        cp STATE_TANG
        jr z,.to_tang
        ld a,(T_EspFlags)               ; том без источников драйвер не примет (DEVINI)
        and VDISK_READY
        ld hl,T_ErrEmpty
        jp z,.error
        xor a                           ; ESP — том VFS (карту Tang, если была у WC, — себе);
        call T_EspMode                  ; прошивка до 0.1.100 MODE не знает — не важно
        ld a,STATE_VFS
        call T_Install
        jr .remount
.to_tang:
        ld a,1                          ; карту Tang — WC: ESP её не монтирует и не меняет ядро
        call T_EspMode
        ld hl,T_ErrOld
        jp c,.error                     ; ответа нет: прошивка без MODE
        ld a,(ProtoBuf)
        or a
        ld hl,T_ErrNoCard
        jp nz,.error
        ld a,STATE_TANG
        call T_Install
        jr .remount
.to_sd2:
        ld a,(T_Cur)
        cp STATE_TANG
        jr nz,.uninstall
        xor a                           ; карту Tang — снова ESP
        call T_EspMode
        jr nc,.uninstall
        ld a,1
        ld (T_Kept),a                   ; не ответила: отдаст резидент при старте WC
.uninstall:
        call T_Uninstall
.remount:
        call T_Remount
        ld a,(T_NoRemount)
        or a
        jr z,.no_panel
        ld hl,T_InfoPanel
        ld (T_Msg),hl
.no_panel:
        ld a,(T_Kept)
        or a
        ret z
        ld hl,T_InfoKept
        ld de,(T_Msg)
        ld a,d
        or e
        jr z,.kept_first
        ld (T_Msg2),hl
        ret
.kept_first:
        ld (T_Msg),hl
        ret
.error:
        ld (T_Msg),hl
        ld a,1
        ld (T_Err),a
        ret

; BC байтов по HL и DE: CF=0 — равны, CF=1 — нет. Портит A, BC, DE, HL.
T_Same:
        ld a,(de)
        cp (hl)
        scf
        ret nz
        inc hl
        inc de
        dec bc
        ld a,b
        or c
        jr nz,T_Same
        ret

; Сверка WC. CF=1 — не та сборка. (T_On) — переходник уже стоит.
T_CheckWc:
        ld hl,WC_DOS_SWP
        ld de,WC_SWP_BYTES+(WC_DOS_SWP-WC_TRAMP)
        ld bc,WC_SD2_LDHL-WC_DOS_SWP
        call T_Same
        ret c
        ld hl,WC_SD2_LDHL+3
        ld de,WC_SWP_BYTES+(WC_SD2_LDHL+3-WC_TRAMP)
        ld bc,WC_TRAMP+WC_SWP_LEN-(WC_SD2_LDHL+3)
        call T_Same
        ret c
        ld hl,WC_ENTRY_CHECKS           ; входы WC для перемонтирования
.entry:
        ld e,(hl)
        inc hl
        ld d,(hl)
        inc hl
        ld a,d
        or e
        jr z,.status
        ex de,hl                        ; HL — вход, DE — эталон
        ld bc,8
        push de
        call T_Same
        pop de
        ret c
        ld hl,8
        add hl,de
        jr .entry
.status:
        ld a,#02                        ; STATUS — в странице #02
        call T_MapC000
        ld hl,WC_STATUS+#4000
        ld de,WC_STATUS_CHECK
        ld bc,8
        call T_Same
        push af
        call T_UnmapC000
        pop af
        ret c
        ; ветка A=6: эталон — переходника нет; JP WC_TRAMP — стоит
        ld hl,WC_SD2_LDHL
        ld de,WC_SWP_BYTES+(WC_SD2_LDHL-WC_TRAMP)
        ld bc,3
        call T_Same
        jr nc,.off
        ld hl,WC_SD2_LDHL
        ld de,T_JpTramp
        ld bc,3
        call T_Same
        ret c
        ld hl,WC_TRAMP                  ; переходник наш: всё, кроме страницы и адреса
        ld de,T_Tramp
        ld bc,T_TRAMP_PAGE
        call T_Same
        ret c
        ld a,1
        ld (T_On),a
        or a
        ret
.off:
        ld hl,WC_TRAMP
        ld b,16
.zero:
        ld a,(hl)
        or a
        scf
        ret nz                          ; там уже что-то чужое
        inc hl
        djnz .zero
        xor a
        ld (T_On),a
        ret

; A — страница в окно #C000 (прежняя — в T_SaveC000)
T_MapC000:
        push af
        ld a,(#6003)
        ld (T_SaveC000),a
        pop af
        jp WC_MNGC
T_UnmapC000:
        ld a,(T_SaveC000)
        jp WC_MNGC

; Страница резидента: подпись в окне #C000. CF=1 — нет.
T_FindRes:
        ld a,(#6003)
        ld (T_SaveC000),a
        ld b,#61
.page:
        ld a,b
        call WC_MNGC                    ; BC сохраняет
        push bc
        ld hl,RES_WINDOW+RES_SIG
        ld de,T_SigText
        ld bc,8
        call T_Same
        pop bc
        jr c,.next
        ld a,(RES_WINDOW+RES_SIG+SIG_VERSION)
        cp RES_LAYOUT
        jr nz,.next
        ld a,(RES_WINDOW+RES_SIG+SIG_PAGE)
        cp b
        jr nz,.next                     ; копия заголовка, не установленный резидент
        ld (T_ResPage),a
        ld hl,(RES_WINDOW+RES_SIG+SIG_SWAP)
        ld (T_Tramp+T_TRAMP_SWAP),hl
        ld (T_Tramp+T_TRAMP_PAGE),a
        ld hl,(RES_WINDOW+RES_SIG+SIG_IO)
        ld (T_ResIo),hl
        call T_UnmapC000
        or a
        ret
.next:
        inc b
        ld a,b
        cp #A0
        jr c,.page
        call T_UnmapC000
        scf
        ret

; --- диагностика драйвера (поля SIG_SWAPS…SIG_ERR подписи резидента) ---------------------------

; Запись прошлой работы драйвера — в T_Diag3 (счётчики, операция, итог) и T_Diag4 (доклад ESP).
T_Diag:
        call T_DiagCopy
        ld de,T_Diag3
        ld hl,T_PfxDrv
        call T_FmtDiag
        ld de,T_Diag4
        ld a,(T_DiagBuf+SIG_WHY-SIG_SWAPS)
        cp DIAG_ESPERR
        jr nz,.no_err
        ld hl,T_PfxEsp
        call T_PutStr
        ld hl,T_DiagBuf+SIG_ERR-SIG_SWAPS
        call T_PutStr
.no_err:
        xor a
        ld (de),a
        ret

; Поля диагностики резидента — в T_DiagBuf.
T_DiagCopy:
        ld a,(T_ResPage)
        call T_MapC000
        ld hl,RES_WINDOW+RES_SIG+SIG_SWAPS
        ld de,T_DiagBuf
        ld bc,SIG_SIZE-SIG_SWAPS
        ldir
        jp T_UnmapC000

; Самопроверка: BEGIN и INFO кодом резидента (RES_IO через окно #C000, прерывания закрыты на
; время вызова; ответ ждём, как драйвер, порциями RES_WAIT). Итог — в T_Diag5.
T_SelfTest:
        ld a,RES_BEGIN
        call T_ResCall
        jr c,.report
        ld a,RES_INFO
        call T_ResCall
        jr nc,.report
        ld b,RES_TRIES
.wait:
        cp RES_PENDING
        jr nz,.report
        push bc
        ld a,RES_WAIT
        call T_ResCall
        pop bc
        jr nc,.report
        djnz .wait
.report:
        call T_DiagCopy
        ld de,T_Diag5
        ld hl,T_PfxSelf
        jp T_FmtDiag

; Вызвать RES_IO резидента с операцией A. Выход: CF и A — как у RES_IO.
T_ResCall:
        push af
        ld a,(T_ResPage)
        call T_MapC000
        pop af
        di
        ld hl,(T_ResIo)
        call .go
        push af
        call T_UnmapC000
        ei
        pop af
        ret
.go:
        jp (hl)

; Строка DE: префикс HL, счётчики, операция, итог, ожидания, байт итога; в конце 0.
T_FmtDiag:
        call T_PutStr
        ld hl,T_TxtSwap
        call T_PutStr
        ld hl,(T_DiagBuf+SIG_SWAPS-SIG_SWAPS)
        call T_PutHex16
        ld hl,T_TxtIo
        call T_PutStr
        ld hl,(T_DiagBuf+SIG_CALLS-SIG_SWAPS)
        call T_PutHex16
        ld a,' '
        ld (de),a
        inc de
        ld a,(T_DiagBuf+SIG_OP-SIG_SWAPS)
        ld hl,T_OpNames
        call T_PutName
        ld a,' '
        ld (de),a
        inc de
        ld a,(T_DiagBuf+SIG_WHY-SIG_SWAPS)
        ld hl,T_WhyNames
        call T_PutName
        ld hl,T_TxtWait
        call T_PutStr
        ld hl,(T_DiagBuf+SIG_WAITS-SIG_SWAPS)
        call T_PutHex16
        ld hl,T_TxtRes
        call T_PutStr
        ld a,(T_DiagBuf+SIG_RES-SIG_SWAPS)
        call T_PutHex8
        xor a
        ld (de),a
        ret

; Строку HL (до нуля, не больше остатка поля) — в DE; DE — за ней.
T_PutStr:
        ld a,(hl)
        or a
        ret z
        ld (de),a
        inc hl
        inc de
        jr T_PutStr

; Имя номер A из списка HL (строки с нулём подряд, конец — пустая); нет — «?».
T_PutName:
        or a
        jr z,T_PutStr
.skip:
        ld c,a
.char:
        ld a,(hl)
        inc hl
        or a
        jr nz,.char
        ld a,(hl)
        or a
        jr z,.unknown
        ld a,c
        dec a
        jr nz,.skip
        jr T_PutStr
.unknown:
        ld a,'?'
        ld (de),a
        inc de
        ret

T_PutHex16:
        ld a,h
        call T_PutHex8
        ld a,l
T_PutHex8:
        push af
        rrca
        rrca
        rrca
        rrca
        call .digit
        pop af
.digit:
        and #0F
        add a,'0'
        cp '9'+1
        jr c,.put
        add a,'A'-'9'-1
.put:
        ld (de),a
        inc de
        ret

T_PfxDrv:       db "DRV",0
T_PfxSelf:      db "SELF",0
T_PfxEsp:       db "ESP: ",0
T_TxtSwap:      db " SW",0
T_TxtIo:        db " IO",0
T_TxtWait:      db " W",0
T_TxtRes:       db " R",0
T_OpNames:      db "BEGIN",0,"INFO",0,"READ",0,"WAIT",0,"WRITE",0,"RELEASE",0,0
T_WhyNames:     db "OK",0,"LINK",0,"SEND",0,"WAIT",0,"ESPERR",0,"RESULT",0,"NOTREADY",0,"LENGTH",0,0

; Прошивка ESP умеет VDISK: INFO — итог 0, затем RESET (том заново). CF=1 — нет.
T_Esp:
        call ZiFi_Init
        ret c
        ld a,CMD_VDISK_INFO
        call Proto_SendEmpty
        ret c
        ld a,RESP_VDISK_INFO
        ld de,T_POLLS
        call Proto_WaitCmd
        ret c
        ld a,(ProtoBuf+1)
        ld (T_EspFlags),a
        ld a,(ProtoBuf)
        or a
        scf
        ret nz
        ld a,CMD_VDISK_RESET
        call Proto_SendEmpty
        ret c
        ld a,RESP_VDISK_RESET
        ld de,T_POLLS
        call Proto_WaitCmd
        ret c
        ld a,(ProtoBuf)
        or a
        ret z
        scf
        ret

; Режим ESP (MODE): A — 0 (том VFS) или 1 (карта Tang у WC). CF=1 — ответа нет; иначе итог —
; в (ProtoBuf).
T_EspMode:
        ld (T_ModeReq),a
        call ZiFi_Init
        ret c
        ld hl,T_ModeReq
        ld bc,1
        ld a,CMD_VDISK_MODE
        call Proto_Send
        ret c
        ld a,RESP_VDISK_MODE
        ld de,T_POLLS
        jp Proto_WaitCmd

; A — STATE_* SD2 из подписи резидента
T_GetState:
        ld a,(T_ResPage)
        call T_MapC000
        ld a,(RES_WINDOW+RES_SIG+SIG_STATE)
        push af
        call T_UnmapC000
        pop af
        ret

; Окно выбора: что будет на SD2 (13.11.2026, по слову пользователя). «SD-Card 2» — второй SD Evo,
; «SD Tang» — карта Tang через ESP с записью, «VFS» — том ESP с папками TANG и USB, только чтение.
; По умолчанию выбран «SD-Card 2». ↑/↓ — выбор, Enter — поставить, Esc — ничего не менять (CF=1).
; Выход: A — STATE_* выбранного, CF=0.
;
; Оформление (13.11.2026: «красивый текстовый консольный дизайн» вместо скобок «(*)»): выбранная
; строка — полоса чёрным по голубому со стрелкой «►», как курсор панелей WC; имена — ярко-белым,
; пояснения — голубым; что стоит сейчас — зелёная метка «◄ now»; под разделителем ╟──╢ — клавиши
; жёлтым. Всё это умеет сам WC, своего вывода на экран у плагина нет:
; - цвет меняют коды TXTPR внутри строки (WCIF.ASM, L7828_96): код 1…8 и за ним знак — цвет знаков
;   «код AND 7» на фоне окна; два одинаковых кода c,c — яркий цвет c (c+8) на фоне окна; два разных
;   p,i — фон p-1 и цвет знаков i-1; #09 — фон и знаки местами; #0D (новая строка) — снова цвет
;   окна. Знаки #10 «►», #11 «◄», #18 «↑», #19 «↓» шрифта WC (CP866) TXTPR печатает как есть;
; - разделитель ╟──╢ рисует PRWOW: байт +#0A описателя — его строка, считая от нижней рамки
;   (1 — последняя строка внутри окна).
T_Select:
        call WC_WAITUP_                 ; Enter, которым запустили плагин, — не выбор
        ld a,(T_Cur)                    ; строка того, что стоит сейчас: 0, 1 или 2
        ld c,0
        or a
        jr z,.now
        inc c
        cp STATE_TANG
        jr z,.now
        inc c
.now:
        ld a,c
        ld (T_NowRow),a
        xor a
        ld (T_Sel),a
        ld ix,T_SelWin
        ld (ix+2),#FF                   ; центр текущего режима экрана
        ld (ix+3),#FF
        call WC_PRWOW_
.draw:
        call T_SelBuild                 ; текст окна — в T_Text: строки целиком, полоса — заново
        ld ix,T_SelWin
        ld hl,T_Text
        ld de,#0101
        call WC_TXTPR_
.wait:
        ei
        halt
        call WC_ESC_
        jr nz,.cancel
        call WC_ENTER_
        jr nz,.chosen
        call WC_DOWN_
        jr nz,.down
        call WC_UP_
        jr nz,.up
        jr .wait
.down:
        ld a,(T_Sel)
        cp 2
        jr nc,.wait
        inc a
        ld (T_Sel),a
        jr .draw
.up:
        ld a,(T_Sel)
        or a
        jr z,.wait
        dec a
        ld (T_Sel),a
        jr .draw
.chosen:
        call .close
        ld a,(T_Sel)
        ld e,a
        ld d,0
        ld hl,T_SelStates
        add hl,de
        ld a,(hl)
        or a                            ; CF=0
        ret
.cancel:
        call .close
        scf
        ret
.close:
        call WC_WAITUP_                 ; та же клавиша не уйдёт в WC и не закроет окно итога
        ld ix,T_SelWin
        jp WC_RRESB_

; Текст окна выбора — в T_Text: вопрос, три строки, пустая строка, разделитель, клавиши. Каждая
; строка выбора — одной ширины (T_ROW_W знаков без кодов) в обоих видах: полоса, ушедшая со
; строки, затирается целиком.
T_SelBuild:
        ld de,T_Text
        ld hl,T_SelHead
        call T_Put
        ld hl,T_SelRows
        ld b,0                          ; номер строки
.row:
        ld a,' '                        ; поле слева — цветом окна
        ld (de),a
        inc de
        ld a,(T_Sel)
        sub b
        ld (T_InBar),a                  ; 0 — строка под полосой
        push hl
        ld hl,T_SelBar                  ; полоса: коды 6,1 (чёрный по голубому) и « ► »
        jr z,.lead
        ld hl,T_SelLead
.lead:
        call T_Put
        pop hl
        call T_PutRow                   ; имя и пояснение; HL — за нулём строки
        push hl
        ld hl,T_SelNowMark              ; «◄ now» ярко-зелёным (под полосой — её цветом)
        ld a,(T_NowRow)
        cp b
        jr z,.mark
        ld hl,T_SelNoMark
.mark:
        call T_PutRow
        ld hl,T_SelEnd                  ; пробел и новая строка
        call T_Put
        pop hl
        inc b
        ld a,b
        cp 3
        jr c,.row
        ld hl,T_SelFoot
        call T_Put
        xor a
        ld (de),a
        ret

; Строку HL до нуля — в DE; под полосой (T_InBar=0) — без кодов цвета (байты 1…9): строка
; полосы — одним её цветом. Выход: HL — за нулём, DE — за строкой.
T_PutRow:
        ld a,(hl)
        inc hl
        or a
        ret z
        cp #0A
        jr nc,.char
        ld c,a
        ld a,(T_InBar)
        or a
        jr z,T_PutRow                   ; под полосой код пропустить
        ld a,c
.char:
        ld (de),a
        inc de
        jr T_PutRow

; A — STATE_VFS или STATE_TANG
T_Install:
        push af
        di
        ld hl,T_Tramp
        ld de,WC_TRAMP
        ld bc,16
        ldir
        ld hl,T_JpTramp
        ld de,WC_SD2_LDHL
        ld bc,3
        ldir
        ei
        pop af
        jr T_SetState

T_Uninstall:
        di
        ld hl,WC_TRAMP
        ld b,16
.zero:
        ld (hl),0
        inc hl
        djnz .zero
        ld hl,WC_SWP_BYTES+(WC_SD2_LDHL-WC_TRAMP)
        ld de,WC_SD2_LDHL
        ld bc,3
        ldir
        ei
        xor a
T_SetState:
        ld (T_On),a
        push af
        ld a,(T_ResPage)
        call T_MapC000
        pop af
        ld (RES_WINDOW+RES_SIG+SIG_STATE),a
        jp T_UnmapC000

; Перемонтировать панели на SD2: копия R_START — в страницу #02, перелёт. Нет места — T_NoRemount.
; Возврат SD-Card 2 (T_Mode=0): на этих панелях был VFS или SD Tang — они переходят на диск 1:
; (13.11.2026, по слову пользователя), прочие режимы — снова SD2 с новым томом. Устройство, как и
; страница возврата, ставится в копию после сверки: от запуска к запуску оно разное.
T_Remount:
        xor a
        ld (T_NoRemount),a
        ld a,(WC_WINDOW+#1D)
        cp DEV_SD2
        jr z,.need
        ld a,(WC_WINDO2+#1D)
        cp DEV_SD2
        ret nz                          ; на SD2 ни одной панели
.need:
        ld a,#02
        call T_MapC000
        ld hl,R_START+#4000             ; наша копия уже там? (кроме байта устройства)
        ld de,R_START
        ld bc,R_DEV_ARG-R_START
        call T_Same
        jr c,.foreign
        ld hl,R_DEV_ARG+1+#4000
        ld de,R_DEV_ARG+1
        ld bc,R_BACK-R_DEV_ARG
        call T_Same
        jr nc,.copy
.foreign:
        ld hl,WC_FREE02+#4000           ; иначе область должна быть как в boot.$C
        ld bc,WC_FREE02_END-WC_FREE02
        ld de,0
.sum:
        ld a,(hl)
        add a,e
        ld e,a
        jr nc,.carry
        inc d
.carry:
        inc hl
        dec bc
        ld a,b
        or c
        jr nz,.sum
        ld hl,WC_FREE02_SUM
        or a
        sbc hl,de
        jr z,.copy
        call T_UnmapC000
        ld a,1
        ld (T_NoRemount),a
        ret
.copy:
        ld hl,R_START
        ld de,R_START+#4000
        ld bc,R_LEN
        ldir
        ld a,(T_Own)
        ld (R_BACK+1+#4000),a
        ld a,(T_Mode)
        or a
        ld a,DEV_SD2
        jr nz,.device
        ld a,DEV_SD1
.device:
        ld (R_DEV_ARG+#4000),a            ; LD (IX+#1D),n
        call T_UnmapC000
        ld ix,(T_IX)                    ; DRACT в конце — активной панели
        jp R_FLIP                       ; назад — через T_CONT, к вызвавшему

; Окно итога (T_Msg; 0 — окна нет): сообщение и T_Msg2. Ошибка (T_Err) — окно красное, как отказ
; FTP и SMB, и под сообщением — подробности для разбора: прошлая работа драйвера, доклад ESP,
; самопроверка резидента (серым). Под разделителем ╟──╢ — «Any key» жёлтым. Высота — по числу
; строк. Любая клавиша — назад в WC.
T_Show:
        ld hl,(T_Msg)
        ld a,h
        or l
        ret z
        ld de,T_Text
        ld a,#0D                        ; первая строка окна — пустая
        ld (de),a
        inc de
        call T_Put
        ld hl,(T_Msg2)
        ld a,h
        or l
        jr z,.one
        ld a,#0D
        ld (de),a
        inc de
        call T_Put
.one:
        ld a,(T_Err)
        or a
        jr z,.footer
        ld hl,T_TxtDetails
        call T_Put
        ld hl,T_Diag3
        call .diag
        ld hl,T_Diag4
        call .diag
        ld hl,T_Diag5
        call .diag
.footer:
        ld hl,T_TxtFooter
        call T_Put
        xor a
        ld (de),a
        ld hl,T_Text                    ; высота: строк текста + 2 (рамка)
        ld b,3
.count:
        ld a,(hl)
        inc hl
        or a
        jr z,.counted
        cp #0D
        jr nz,.count
        inc b
        jr .count
.counted:
        ld ix,T_ResWin
        ld (ix+2),#FF                   ; центр текущего режима экрана
        ld (ix+3),#FF
        ld (ix+5),b
        ld a,(T_Err)
        or a
        ld a,#17                        ; сообщение — цветом окна выбора
        jr z,.colour
        ld a,#2F                        ; ошибка: ярко-белые по красному, как у FTP и SMB
.colour:
        ld (ix+6),a
        call WC_PRWOW_
        ld ix,T_ResWin
        ld hl,T_Text
        ld de,#0101
        call WC_TXTPR_
        call WC_WAITUP_
.wait:
        ei
        halt
        call WC_ESC_
        jr nz,.close
        call WC_ANYK_                   ; любая клавиша (KBSCN с обычной таблицей её не видит)
        jr z,.wait
.close:
        call WC_WAITUP_                 ; отпущена — WC её не примет за свою команду
        ld ix,T_ResWin
        jp WC_RRESB_
.diag:                                  ; строка подробностей — если не пустая, серым
        ld a,(hl)
        or a
        ret z
        ld a,#0D
        ld (de),a
        inc de
        ld a,7                          ; код TXTPR: знаки цвета 7 на фоне окна
        ld (de),a
        inc de
        ld a,' '                        ; отступ — как у «Details:»
        ld (de),a
        inc de
        ld (de),a
        inc de
        jp T_Put

; Строку HL до нуля — в DE (без нуля); DE — за ней.
T_Put:
        ld a,(hl)
        or a
        ret z
        ld (de),a
        inc hl
        inc de
        jr T_Put

WC_PRWOW_:
        ld a,FN_PRWOW
        jp WC_API
WC_RRESB_:
        ld a,FN_RRESB
        jp WC_API
WC_TXTPR_:
        ld a,FN_TXTPR
        jp WC_API
WC_ESC_:
        ld a,FN_ESC
        jp WC_API
WC_ENTER_:
        ld a,FN_ENTER
        jp WC_API
WC_UP_:
        ld a,FN_UP
        jp WC_API
WC_DOWN_:
        ld a,FN_DOWN
        jp WC_API
WC_ANYK_:                               ; NZ — нажата любая клавиша
        ld a,FN_ANYK
        jp WC_API
WC_WAITUP_:
        ld a,FN_WAITUP
        jp WC_API

T_WIDTH         equ 44                  ; строка подробностей (T_Diag3…T_Diag5)
T_WINW          equ 56                  ; ширина окон: 54 знака внутри рамки
T_COLOUR        equ #17                 ; окна: белые знаки по синему фону, как у WC
T_INK           equ T_COLOUR&7          ; код TXTPR «снова цвет окна» (после жёлтого)
T_NAME_W        equ 12                  ; поле имени в строке выбора
T_DESC_W        equ 31                  ; поле пояснения
T_ROW_W         equ 3+T_NAME_W+T_DESC_W+5+1   ; строка полосы: « ► », имя, пояснение, метка, пробел
        ASSERT 1+T_ROW_W+1 == T_WINW-2, полоса — во всю ширину окна, по знаку поля с боков
T_Header:       db #0E,9," VDAC4 VFS ",0
T_SelWin:
        db #81,0                        ; тень и рамка стиля 1 (двойная)
        db #FF,#FF                      ; X,Y: центр
        db T_WINW,11                    ; 9 строк внутри
        db T_COLOUR,0
        dw 0
        db 2,0                          ; разделитель ╟──╢ — над строкой клавиш
        dw T_Header
        dw 0
        dw 0
T_SelHead:
        db #0D
        db #0E,7,7,"Choose what drive SD2 shows",#0D
        db #0D,0
T_SelBar:       db 6,1," ",#10," ",0    ; полоса: чёрный (1-1) по голубому (6-1), « ► »
T_SelLead:      db "   ",0
; Строки выбора: имя ярко-белым (7,7) в T_NAME_W знаков, пояснение голубым (5) в T_DESC_W
T_SelRows:
.r0:    db 7,7,"SD-Card 2"
        ds T_NAME_W-($-.r0-2),' '
.d0:    db 5,"the second SD card of the Evo"
        ds T_DESC_W-($-.d0-1),' '
        db 0
.r1:    db 7,7,"SD Tang"
        ds T_NAME_W-($-.r1-2),' '
.d1:    db 5,"Tang microSD, read and write"
        ds T_DESC_W-($-.d1-1),' '
        db 0
.r2:    db 7,7,"VFS"
        ds T_NAME_W-($-.r2-2),' '
.d2:    db 5,"TANG + USB folders, read only"
        ds T_DESC_W-($-.d2-1),' '
        db 0
T_SelNowMark:   db 4,4,#11," now",0     ; «◄ now» ярко-зелёным
T_SelNoMark:    db "     ",0
T_SelEnd:       db " ",#0D,0
T_SelFoot:
        db #0D                          ; пустая строка
        db #0D                          ; строка разделителя (его рисует PRWOW)
        db #0E,6,6,#18,#19,T_INK," choose      "
        db 6,6,"Enter",T_INK," set      "
        db 6,6,"Esc",T_INK," cancel",0
T_SelStates:    db 0,STATE_TANG,STATE_VFS
T_ResWin:
        db #81,0
        db #FF,#FF
        db T_WINW,4                     ; высоту и цвет ставит T_Show
        db T_COLOUR,0
        dw 0
        db 2,0                          ; разделитель ╟──╢ — над строкой «Any key»
        dw T_Header
        dw 0
        dw 0
T_TxtDetails:   db #0D,#0D,7,"  Details:",0
T_TxtFooter:
        db #0D                          ; конец последней строки сообщения
        db #0D                          ; пустая строка
        db #0D                          ; строка разделителя
        db #0E,6,6,"Any key",7,7," - back to Wild Commander",0

; Сообщения итога: строки по центру (#0E), ярко-белым (7,7), между ними #0D
T_InfoPanel:
        db #0E,7,7,"SD2 has changed. Choose the drive",#0D
        db #0E,7,7,"again in the drive menu.",0
T_InfoKept:
        db #0E,7,7,"SD2 is the second SD card again.",#0D
        db #0E,7,7,"The ESP did not answer: it takes the",#0D
        db #0E,7,7,"Tang card back at the next WC start.",0
T_ErrWc:
        db #0E,7,7,"VDAC4 VFS needs Wild Commander",#0D
        db #0E,7,7,"Improved v1.11i.",0
T_ErrRes:
        db #0E,7,7,"VFSRES.WMF is not loaded.",#0D
        db #0E,7,7,"Add it to [PLUGINS] in /WC/wc.ini",#0D
        db #0E,7,7,"and restart Wild Commander.",0
T_ErrEsp:
        db #0E,7,7,"The ESP firmware has no VDAC4 VFS.",#0D
        db #0E,7,7,"Update the ESP: vdac4-0.1.99 or newer.",0
T_ErrEmpty:
        db #0E,7,7,"The VFS volume has no sources.",#0D
        db #0E,7,7,"Update the ESP: vdac4-0.1.99 or newer.",0
T_ErrOld:
        db #0E,7,7,"SD Tang needs ESP firmware",#0D
        db #0E,7,7,"vdac4-0.1.100 or newer.",0
T_ErrNoCard:
        db #0E,7,7,"No Tang microSD card.",#0D
        db #0E,7,7,"Insert it into the Tang Nano",#0D
        db #0E,7,7,"(Tang core TS-Config 1.30 or newer).",0
T_SigText:      db "VDAC4VFS"
        INCLUDE "wc111i_data.inc"

; Переходник (vfs_res.inc): страница и адрес RES_SWAP — из подписи резидента
T_TRAMP_PAGE    equ 10
T_TRAMP_SWAP    equ 14
T_Tramp:
        db #ED,#57                      ; LD A,I
        db #F3                          ; DI
        db #F5                          ; PUSH AF
        db #01,#AF,#13                  ; LD BC,#13AF
        db #ED,#58                      ; IN E,(C)
        db #3E,0                        ; LD A,страница
        db #ED,#79                      ; OUT (C),A
        db #C3                          ; JP RES_SWAP
        dw 0
        ASSERT $-T_Tramp == 16
T_JpTramp:      db #C3
                dw WC_TRAMP

T_IX:           dw 0
T_Own:          db 0
T_On:           db 0                    ; переходник стоит; T_SetState — STATE_* (не 0 — включено)
T_Mode:         db 0                    ; режим, выбранный в T_Select (STATE_*, 0 — второй SD)
T_Cur:          db 0                    ; режим, что стоял при запуске
T_Sel:          db 0                    ; строка выбора 0…2
T_NowRow:       db 0                    ; строка того, что стоит сейчас (метка «◄ now»)
T_InBar:        db 0                    ; T_SelBuild: 0 — строка под полосой
T_Err:          db 0                    ; итог — ошибка
T_Kept:         db 0                    ; ESP не ответила на возврат карты Tang
T_Text:         ds 640                  ; текст окна итога (T_Show)
T_ModeReq:      db 0                    ; payload MODE
T_ResPage:      db 0
T_SaveC000:     db 0
T_NoRemount:    db 0
T_Msg:          dw 0
T_Msg2:         dw 0
T_ResIo:        dw 0                    ; RES_IO резидента (SIG_IO)
T_EspFlags:     db 0                    ; флаги ответа INFO (VDISK_READY)
T_DiagBuf:      ds SIG_SIZE-SIG_SWAPS   ; копия полей диагностики резидента
T_Diag3:        ds T_WIDTH+1,0
T_Diag4:        ds T_WIDTH+1,0
T_Diag5:        ds T_WIDTH+1,0

        INCLUDE "zifi_link.asm"
        INCLUDE "proto.asm"
T_CODE_END:
        ASSERT T_CODE_END <= WC_FREE02-5, код переключателя налез на место перелёта
        ENT

; --- перелёт: те же адреса в странице плагина и в странице WC #02 ---------------------------
        ORG pageStart+(WC_FREE02-5-#8000)
        DISP WC_FREE02-5
R_FLIP:                                 ; страница плагина
        ld a,#02
        call WC_MNG8                    ; окно #8000 — страница WC: возврат из MNG8 — уже в неё
        ASSERT $ == WC_FREE02
R_START:                                ; копия — в странице #02
        jr .main
.panel:
        ld a,(ix+#1D)                   ; устройство панели
        cp DEV_SD2
        ret nz
.dev:   ld (ix+#1D),DEV_SD2             ; DEV_SD2 — снова SD2, DEV_SD1 — на 1: (ставит T_Remount в копии)
        ld (ix+#1C),0                   ; драйвер не выбран: DRACT позовёт DOS_SWP
        call WC_DRACT
        jp WC_ORIGA                     ; DEVINI, HDD, корень, список
.main:
        push ix
        ld ix,WC_WINDOW
        call .panel
        ld ix,WC_WINDO2
        call .panel
        pop ix
        call WC_DRACT
        call WC_XPAN                    ; строка состояния второй панели
        call WC_STATUS
        call WC_XPAN
R_BACK:
        ld a,0                          ; страница плагина (ставит T_Remount в копии)
        call WC_MNG8                    ; окно #8000 — плагин: возврат — на T_CONT
T_CONT:                                 ; только в странице плагина
        ret
R_LEN           equ T_CONT-R_START
R_DEV_ARG       equ R_START.dev+3       ; байт n в LD (IX+#1D),n: устройство панели после перелёта
        ASSERT WC_FREE02+R_LEN <= WC_FREE02_END
        ENT
pageEnd:
        ORG pageEnd
        ALIGN 512
endCode:
        SAVEBIN "../build/VDAC4VFS.WMF",startCode,endCode-startCode
