; VDAC4VFS.WMF — переключатель VDAC4 VFS (меню F10): SD2 Wild Commander -> диск ESP и обратно.
; Каждый запуск меняет состояние на противоположное. Включение спрашивает, что будет на SD2:
; Enter — сама карта Tang с записью (ESP отдаёт её WC: MODE 1 — FatFs ESP карту не монтирует, ядро
; Tang не меняется); Space — том VFS с папками источников, только чтение. Выключение карту Tang
; возвращает ESP (MODE 0); не вышло — её вернёт резидент при следующем старте WC.
;
; Включение: сверить WC Improved v1.11i побайтно (wc111i.inc), найти страницу резидента VFSRES.WMF
; по подписи, убедиться, что прошивка ESP умеет VDISK (INFO), сбросить её том (RESET), поставить
; переходник перед DOS_SWP и «JP WC_TRAMP» на месте «LD HL,DR2» ветки A=6. Выключение — вернуть
; эталонные байты. Панели, что стоят на SD2, перемонтируются как при выборе диска (как MEDIA_IDLE
; ядра: (IX+#1C)=0, DRACT, ORIGA, затем DRACT, XPAN, STATUS, XPAN). Этот код WC ждёт свою
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

; Всё, кроме окна. В T_Msg — текст итога.
T_Main:
        call T_CheckWc
        ld hl,T_MsgWc
        jp c,.done
        call T_FindRes
        ld hl,T_MsgRes
        jp c,.done
        call T_Diag                     ; что было с драйвером до этого запуска
        call T_SelfTest                 ; код резидента: BEGIN и INFO, как у драйвера
        ld a,(T_On)
        or a
        jr nz,.off
        call T_Choose                   ; (T_Mode): STATE_TANG или STATE_VFS; CF=1 — Esc
        ld hl,T_MsgCancel
        jr c,.done
        call T_Esp
        ld hl,T_MsgEsp
        jr c,.done
        ld a,(T_Mode)
        cp STATE_TANG
        jr z,.tang
        ld a,(T_EspFlags)               ; том без источников драйвер не примет (DEVINI)
        and VDISK_READY
        ld hl,T_MsgEmpty
        jr z,.done
        xor a                           ; ESP — том VFS (карту Tang, если была у WC, — себе);
        call T_EspMode                  ; прошивка до 0.1.100 MODE не знает — не важно
        ld a,STATE_VFS
        call T_Install
        ld hl,T_MsgOn
        jr .remount
.tang:
        ld a,1                          ; карту Tang — WC: ESP её не монтирует и не меняет ядро
        call T_EspMode
        ld hl,T_MsgOld
        jr c,.done                      ; ответа нет: прошивка без MODE
        ld a,(ProtoBuf)
        or a
        ld hl,T_MsgNoCard
        jr nz,.done
        ld a,STATE_TANG
        call T_Install
        ld hl,T_MsgOnTang
        jr .remount
.off:
        call T_GetState
        cp STATE_TANG
        ld hl,T_MsgOff
        jr nz,.uninstall
        xor a                           ; карту Tang — снова ESP
        call T_EspMode
        ld hl,T_MsgKept
        jr c,.uninstall                 ; не ответила: отдаст при старте WC (резидент)
        ld hl,T_MsgOff
.uninstall:
        push hl
        call T_Uninstall
        pop hl
.remount:
        ld (T_Msg),hl
        call T_Remount
        ld a,(T_NoRemount)
        or a
        ret z
        ld hl,T_MsgPanel
        ld (T_Msg2),hl
        ret
.done:
        ld (T_Msg),hl
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

; Окно выбора: Enter — карта Tang с записью, Space — том VFS, Esc — отмена (CF=1).
T_Choose:
        call WC_WAITUP_                 ; Enter, которым запустили плагин, — не выбор
        ld ix,T_ChoiceWin
        call WC_PRWOW_
        ld ix,T_ChoiceWin
        ld hl,T_ChoiceText
        ld de,#0101
        call WC_TXTPR_
.wait:
        ei
        halt
        call WC_ESC_
        jr nz,.cancel
        call WC_ENTER_
        ld a,STATE_TANG
        jr nz,.chosen
        call WC_SPACE_
        ld a,STATE_VFS
        jr nz,.chosen
        jr .wait
.chosen:
        ld (T_Mode),a
        call .close
        or a
        ret
.cancel:
        call .close
        scf
        ret
.close:
        call WC_WAITUP_                 ; та же клавиша не закроет окно итога
        ld ix,T_ChoiceWin
        jp WC_RRESB_

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
        ld hl,R_START+#4000             ; наша копия уже там?
        ld de,R_START
        ld bc,R_BACK+1-R_START
        call T_Same
        jr nc,.copy
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
        call T_UnmapC000
        ld ix,(T_IX)                    ; DRACT в конце — активной панели
        jp R_FLIP                       ; назад — через T_CONT, к вызвавшему

; Окно итога: T_Msg, T_Msg2 (если есть); ждать Enter или Esc.
T_Show:
        ld hl,(T_Msg)
        ld de,T_Line1
        call T_CopyLine
        ld hl,(T_Msg2)
        ld de,T_Line2
        call T_CopyLine
        ld hl,T_Diag3
        ld de,T_Line3
        call T_CopyLine
        ld hl,T_Diag4
        ld de,T_Line4
        call T_CopyLine
        ld hl,T_Diag5
        ld de,T_Line5
        call T_CopyLine
        ld ix,T_Window
        call WC_PRWOW_
        ld ix,T_Window
        ld hl,T_Text
        ld de,#0101
        call WC_TXTPR_
.wait:
        ei
        halt
        call WC_ESC_
        jr nz,.close
        call WC_ENTER_
        jr z,.wait
.close:
        ld ix,T_Window
        jp WC_RRESB_

; Строка HL (0 — пусто) в поле DE шириной T_WIDTH, остаток — пробелы.
T_CopyLine:
        ld b,T_WIDTH
.copy:
        ld a,h
        or l
        jr z,.space
        ld a,(hl)
        or a
        jr z,.space
        ld (de),a
        inc hl
        inc de
        djnz .copy
        ret
.space:
        ld hl,0
        ld a,' '
        ld (de),a
        inc de
        djnz .copy
        ret

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
WC_SPACE_:
        ld a,FN_SPACE
        jp WC_API
WC_WAITUP_:
        ld a,FN_WAITUP
        jp WC_API

T_WIDTH         equ 44
T_Window:
        db #81,0
        db 13,7                         ; X,Y
        db T_WIDTH+4,10                 ; W,H
        db #17,0
        dw 0
        db 0,0
        dw T_Header
        dw 0
        dw 0
T_Header:       db #0E,9," VDAC4 VFS ",0
T_ChoiceWin:
        db #81,0
        db 13,9                         ; X,Y
        db T_WIDTH+4,7                  ; W,H
        db #17,0
        dw 0
        db 0,0
        dw T_Header
        dw 0
        dw 0
T_ChoiceText:
        db #0D
        db " Enter - SD2 = Tang microSD (read/write)",#0D
        db " Space - SD2 = VDAC4 VFS (read only)",#0D
        db " Esc   - leave SD2 as it is",0
T_Text:
        db #0D
T_Line1:        ds T_WIDTH,' '
        db #0D
T_Line2:        ds T_WIDTH,' '
        db #0D
T_Line3:        ds T_WIDTH,' '          ; диагностика: прошлая работа драйвера
        db #0D
T_Line4:        ds T_WIDTH,' '          ;   доклад ESP об ошибке
        db #0D
T_Line5:        ds T_WIDTH,' '          ;   самопроверка кода резидента
        db #0D,#0D
        db "      Enter / Esc - back to Wild Commander",0

T_MsgOn:        db "SD2 = VDAC4 VFS (ESP: TANG = Tang microSD)",0
T_MsgOnTang:    db "SD2 = Tang microSD card, read/write",0
T_MsgOff:       db "SD2 = second SD card again",0
T_MsgKept:      db "SD2 normal; ESP did not answer the release",0
T_MsgCancel:    db "SD2 left as it was",0
T_MsgOld:       db "SD2 = Tang card needs ESP firmware 0.1.100",0
T_MsgNoCard:    db "No Tang card: needs TS-Config 1.30+ and SD",0
T_MsgWc:        db "Needs Wild Commander Improved v1.11i",0
T_MsgRes:       db "VFSRES.WMF not loaded: add it to [PLUGINS]",0
T_MsgEsp:       db "ESP has no VFS: needs firmware vdac4-0.1.99",0
T_MsgEmpty:     db "ESP VFS is empty: needs firmware 0.1.99+",0
T_MsgPanel:     db "Choose SD2 again in the drive menu",0
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
T_Mode:         db 0                    ; выбор окна T_Choose
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
        ASSERT WC_FREE02+R_LEN <= WC_FREE02_END
        ENT
pageEnd:
        ORG pageEnd
        ALIGN 512
endCode:
        SAVEBIN "../build/VDAC4VFS.WMF",startCode,endCode-startCode
