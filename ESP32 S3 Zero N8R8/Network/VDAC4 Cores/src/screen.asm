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
        call Proto_Send
        ld a,RESP_SCR_LIST
        ld de,NX_WAIT
        jp Proto_WaitCmd

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
ScrKept:        db 0                    ; 1 — экран выгружен, его «Loading» оставлен на HDMI
ScrCmd:         db 0
ScrSt:          db 0
ScrCode:        db 0
ScrText:        dw 0
ScrErrSrc:      dw 0
ScrBuf:         ds 5+60
