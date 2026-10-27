; Точка входа плагина. PLUGIN обязан быть первым байтом по адресу #8000.
;
; Порядок:
;   1. связь с ESP (ZiFi_Init);
;   2. CORE_LIST (#2C): ядра на microSD Tang (\CORES\ИМЯ.BIN) и есть ли у ядра свой слой ESP
;      (ИМЯ.ELF); меню: ↑/↓, Enter — выбрать, Esc — назад в WC;
;   3. CORE_APP (#2D) [имя, 0]: ESP ставит ИМЯ.ELF с SD Tang в кэш модулей и запускает его (нет
;      своего — общий слой прошивки «CORE»);
;   4. START (#70) [имя, 0]: слой грузит ядро из кэша BL616 (с SD Tang — сначала в кэш). Плагин
;      ждёт по STATUS (#72), пока ядро не заработает, — до 60 с. Esc с этого шага не выход:
;      картинка ядра бывает на HDMI раньше, чем плагин увидит «работает», а Esc нужен ядру
;      (у ZX Next — BREAK); загрузку кончает само ядро, ошибка слоя или срок;
;   5. окно WC закрыто (картинка ядра — на HDMI), раз в кадр INPUT (#71): джойстик Kempston,
;      кнопки и счётчики мыши Kempston, байты очереди PS/2 AVR (как загрузчик DOOM VDAC4).
;      INPUT — заодно знак жизни: молчание плагина дольше 1,5 с (сброс Evo) слой сам считает
;      выходом;
;      Сразу после старта ядра ZX Next (VERSION: личность 03) — TIME (#74, ядро 0.38+): часы Gluk
;      Evo (shared/weather/rtc.asm) для часов DS1307 ядра — NextZXOS берёт из них время; затем
;      SD_ON (#75, ядро 0.39+): карта Evo — вторая SD Next (evo_sd.asm), сектора просит ESP в
;      ответе на INPUT. Другим ядрам (NES: у NES.ELF #74…#79 — свои команды) их не шлём;
;   6. F12 (код набора 2 #07 без префикса) — EXIT (#73): ESP возвращает Tang в слот 0
;      (TS-Config); плагин ждёт этого по STATUS, затем APP_STOP и выход в WC.
; Пока ядро работает, Evo — в режиме FT812 (VCONFIG #04, M_SEL = 1): видеошина Evo на IDE-40
; молчит, а сброс Evo снимает M_SEL — по этому ядро само просит возврат в слот 0. Режим держит
; WC (функция GVmod), выход возвращает текст WC (MNGV_PL 0).
; Очередь PS/2 AVR, пока ядро работает, читает только плагин: обработчик прерывания WC (I_N_T,
; PSD) иначе забирает из неё по событию за кадр, и часть нажатий и отпусканий не доходила до
; ядра — клавиша без отпускания залипала, и NextZXOS повторял её без конца (0.2). Разбор PS/2 в
; WC выключает функция INT_PL (86) с A' = 2; WC включает его сам, когда плагин вернётся.

CMD_CORE_LIST   equ #2C                 ; -> AC [1][число][записи] / [0][текст, 0]
CMD_CORE_APP    equ #2D                 ; [имя, 0] -> AD [итог int8][приложение, 0][версия/текст, 0]
RESP_CORE_LIST  equ #AC
RESP_CORE_APP   equ #AD
CMD_NX_START    equ #70                 ; [имя ядра, 0] -> F0 [1 — принято]
CMD_NX_INPUT    equ #71                 ; [джойстик, кнопки, X, Y, байты PS/2…] -> F1 [1]
CMD_NX_STATUS   equ #72                 ; -> F2 [состояние][VERSION LE32][текст ошибки, 0]
CMD_NX_EXIT     equ #73                 ; -> F3 [1]
CMD_NX_TIME     equ #74                 ; [сек, мин, час, число, месяц, год 0…99] -> F4 [1]
RTC_CORE        equ #0026               ; VERSION ядра Next с часами DS1307
RESP_NX_START   equ #F0
RESP_NX_INPUT   equ #F1
RESP_NX_STATUS  equ #F2
RESP_NX_EXIT    equ #F3
RESP_NX_TIME    equ #F4

NX_RUNNING      equ 2
NX_BACK         equ 3
NX_ERROR        equ 255

ENTRY_SIZE      equ 21                  ; запись списка: имя 16, длина LE32, флаги
ENTRY_FLAGS     equ 20
MAX_CORES       equ 16

NX_WAIT         equ 6000                ; опросов ответа на быстрые команды
LOAD_FRAMES     equ 9000                ; ядро: до 60 с (с SD Tang в кэш BL616 и JTAG)
BACK_FRAMES     equ 750                 ; назад в слот 0: до 15 с
STATUS_EVERY    equ 25                  ; кадров между STATUS, пока ядро работает
POLL_EVERY      equ 10                  ; кадров между STATUS при загрузке и выходе
PS2_DRAIN       equ 24                  ; байтов очереди PS/2 за опрос (очередь AVR — 16)
PS2_F12         equ #07
VID_FT812       equ #04                 ; VCONFIG: изображение FT812, M_SEL = 1

PLUGIN:
        push ix
        ld (NesCall),a
        ld (NesSize),hl
        ld (NesSize+2),de
        ld (NesNamePtr),bc
        ; Страница плагина живёт в памяти между запусками: состояние — заново.
        xor a
        ld (ProtoErr),a
        ld (ProtoBurstLeft),a
        ld (NxWindowOpen),a
        ld (Ps2Ready),a
        ld (Ps2Prefix),a
        ld (NxExitKey),a
        ld (NxAppStarted),a
        ld (NxFtMode),a
        ld (NsTempOwned),a
        ld (NsStarted),a
        ld (NsSize),a
        ld (NsSize+1),a
        ld (NsSize+2),a
        ld (NsSize+3),a
        ld (NxKbdOwn),a
        ld (CrCount),a
        ld (MenuSel),a
        ld (MenuTop),a
        ld (NxVersion+3),a
        ld (SdOn),a
        ld (SdWrote),a
        ld (SdBurst),a
        call Proto_ResetRx
        ld a,1
        call WC_INT_PL                   ; WC не перерисовывает своё под окном
        call WC_GEDPL
        ld hl,UiCoresTitle
        ld de,UiTitle
        ld b,44
        call Ui_CopyField
        call Ui_MakeList
        ld hl,UiEmpty
        call Ui_SetHint
        call Ui_Open
        ld hl,UiStConnect
        call Ui_Status

        call ZiFi_Init                   ; ESP на SPI и чистый сеанс канала
        ld hl,UiErrNoEsp
        jp c,.fatal
        ld a,(NesCall)
        cp 3
        jr nz,.nes_file
        ld hl,UiStList
        call Ui_Status
        call Cr_List
        jp c,.fatal
        ld hl,UiErrNoCores
        ld a,(CrCount)
        or a
        jp z,.fatal
        ld hl,UiStChoose
        call Ui_SetStatus
        ld hl,UiHintMenu
        call Ui_SetHint
        call Cr_Menu
        jp c,.done                       ; Esc — назад в WC
        jr .start_app
.nes_file:
        ld hl,NesName
        ld de,CrName
        ld bc,4
        ldir
.start_app:
        call Cr_LoadText
        ld hl,UiStStart
        call Ui_Status
        call Cr_StartApp
        jp c,.fatal
        ld a,1
        ld (NxAppStarted),a
        ld a,(NesCall)
        cp 3
        jr nz,.evo_file
        ld hl,CrName
        ld de,NesName
        ld b,4
.is_nes:
        ld a,(de)
        cp (hl)
        jr nz,.start_core
        inc de
        inc hl
        djnz .is_nes
        call Nes_Tang
        jr .prepared
.evo_file:
        call Nes_File
.prepared:
        jr nc,.start_core
        ld a,h
        or l
        jp nz,.fatal_exit
        call Nx_Exit
        jp .done
.start_core:
        ld hl,UiStLoad
        call Ui_SetStatus
        ld hl,UiHintLoad
        call Ui_Hint
        call Nx_StartCore
        jp c,.fatal_exit                 ; ошибка — с текстом
        call Nes_SaveTrack
        call Ui_Close                    ; дальше — картинка ядра на HDMI
        call Nx_VideoOff
        call Nx_KeysOwn
        call Nx_SendTime                 ; часы Evo — в часы ядра (ответ не важен)
        call Nx_SdOn                     ; карта Evo — SD1 Next (evo_sd.asm)
        call Nx_Run
        jp c,.fatal_exit
        call Nx_Exit
.done:
        call Ui_Close
        ld a,(SdWrote)                   ; Next писал на карту Evo — WC перечитает обе панели
        or a
        jr z,.code
        ld a,3
.code:
        pop ix
        ret

.fatal_exit:
        push hl
        call Nx_Exit
        pop hl
.fatal:
        push hl
        ld a,(NxWindowOpen)
        or a
        call z,Ui_Open
        pop hl
        call Ui_SetStatus
        ld hl,UiStEsc
        call Ui_Hint
.wait_esc:
        ei
        halt
        call Nx_Esc
        jr z,.wait_esc
        jr .done

; --- список ядер ------------------------------------------------------------------------
; CORE_LIST → CrList, CrCount. Выход: CF=1, HL — текст ошибки.
Cr_List:
        ld a,CMD_CORE_LIST
        call Proto_SendEmpty
        ld a,RESP_CORE_LIST
        ld b,16                          ; SD Tang монтируется — до секунд
        call Nx_WaitLong
        ld hl,UiErrNoAnswer
        ret c
        ld a,(ProtoBuf)
        or a
        jr nz,.ok
        ld hl,ProtoBuf+1                 ; текст ESP: SD нет, ядро Tang без моста…
        jp Nx_ErrorFrom
.ok:
        ld a,(ProtoBuf+1)
        cp MAX_CORES+1
        jr c,.count
        ld a,MAX_CORES
.count:
        ld (CrCount),a
        or a
        ret z
        ld b,a
        ld hl,0
        ld de,ENTRY_SIZE
.mul:
        add hl,de
        djnz .mul
        ld b,h
        ld c,l
        ld hl,ProtoBuf+2
        ld de,CrList
        ldir
        or a
        ret

; Запись A списка: HL — её начало.
Cr_Entry:
        ld l,a
        ld h,0
        ld d,h
        ld e,l
        add hl,hl                        ; ×2
        add hl,hl                        ; ×4
        add hl,de                        ; ×5
        add hl,hl                        ; ×10
        add hl,hl                        ; ×20
        add hl,de                        ; ×21
        ld de,CrList
        add hl,de
        ret

; Меню: ↑/↓ — выбор, Enter — ядро (CrName), Esc — CF=1.
Cr_Menu:
        call WC_WAITUP                   ; Enter запуска плагина ещё держат
        call Ui_MakeList
        call Ui_Draw
.loop:
        ei
        halt
        call WC_ESC
        scf
        ret nz
        call WC_KEY_ENTER
        jr nz,.enter
        call WC_KEY_DOWN
        jr nz,.down
        call WC_KEY_UP
        jr nz,.up
        jr .loop
.down:
        ld a,(CrCount)
        ld b,a
        ld a,(MenuSel)
        inc a
        cp b
        jr nc,.loop
        ld (MenuSel),a
        jr .moved
.up:
        ld a,(MenuSel)
        or a
        jr z,.loop
        dec a
        ld (MenuSel),a
.moved:
        ; MenuTop ≤ MenuSel < MenuTop + MENU_LINES
        ld hl,MenuTop
        ld a,(MenuSel)
        cp (hl)
        jr nc,.not_above
        ld (hl),a
.not_above:
        ld a,(hl)
        add a,MENU_LINES
        ld b,a
        ld a,(MenuSel)
        cp b
        jr c,.shown
        sub MENU_LINES-1
        ld (hl),a
.shown:
        call Ui_MakeList
        call Ui_Draw
        jr .loop
.enter:
        ld a,(MenuSel)
        call Cr_Entry
        ld de,CrName
        ld bc,16
        ldir
        xor a
        ld (CrName+15),a                 ; имя — до 15 знаков с нулём
Cr_LoadText:
        ld de,UiStLoad                   ; «Loading ИМЯ into the Tang...»
        ld hl,UiLoadPre
        call Ui_Append
        ld hl,CrName
        call Ui_Append
        ld hl,UiLoadPost
        call Ui_Append
        xor a
        ld (de),a                        ; CF=0
        ret

; Длина CrName с нулём → BC.
Cr_NameLen:
        ld hl,CrName
        ld bc,0
.scan:
        inc bc
        ld a,(hl)
        inc hl
        or a
        jr nz,.scan
        ret

; CORE_APP [имя, 0]: слой ESP ядра. Выход: CF=1, HL — текст ошибки.
Cr_StartApp:
        call Cr_NameLen
        ld hl,CrName
        ld a,CMD_CORE_APP
        call Proto_Send
        ld a,RESP_CORE_APP
        ld b,32                          ; ELF с SD Tang в кэш ESP — до секунд
        call Nx_WaitLong
        ld hl,UiErrNoAnswer
        ret c
        ld a,(ProtoBuf)                  ; итог int8: 0 — работает
        or a
        ret z
        ; [итог][приложение, 0][текст, 0]: текст — вторая строка
        ld hl,ProtoBuf+1
.skip:
        ld a,(hl)
        inc hl
        or a
        jr nz,.skip
        ld a,(hl)
        or a
        jr nz,Nx_ErrorFrom
        ld hl,UiErrApp
        scf
        ret

; --- ядро ------------------------------------------------------------------------------
; START и ожидание работающего ядра. Выход: CF=0 — работает; CF=1, HL — текст ошибки.
; Esc не проверяется (см. шаг 4 в начале файла).
Nx_StartCore:
        call Cr_NameLen
        ld hl,CrName
        ld a,CMD_NX_START
        call Proto_Send
        ld a,RESP_NX_START
        ld de,NX_WAIT
        call Proto_WaitCmd
        ld hl,UiErrApp
        ret c
        ld a,(ProtoBuf)
        or a
        scf
        ret z
        ld hl,LOAD_FRAMES
        ld (NxTimer),hl
        ld a,POLL_EVERY
        ld (NxDivider),a
.loop:
        ei
        halt
        ld hl,(NxTimer)
        dec hl
        ld (NxTimer),hl
        ld a,h
        or l
        ld hl,UiErrTimeout
        scf
        ret z
        ld hl,NxDivider
        dec (hl)
        jr nz,.loop
        ld (hl),POLL_EVERY
        call Nx_Status
        jr c,.loop                       ; ESP занята загрузкой — спросить позже
        cp NX_RUNNING
        ret z                            ; CF=0
        cp NX_ERROR
        jr z,Nx_ErrorText
        cp NX_BACK
        jr nz,.loop
        ld hl,UiErrCore
        scf
        ret

; Текст ошибки слоя (ProtoBuf+5) — в NxErrField. Выход: CF=1, HL — UiErrEsp.
Nx_ErrorText:
        ld hl,ProtoBuf+5
; Текст HL — в NxErrField. Выход: CF=1, HL — UiErrEsp.
Nx_ErrorFrom:
        ld de,NxErrField
        ld b,UI_STATUS_WIDTH-5
        call Ui_CopyField
        ld hl,UiErrEsp
        scf
        ret

; STATUS. Выход: CF=0, A — состояние слоя, NxVersion — VERSION ядра; CF=1 — нет ответа.
Nx_Status:
        ld a,CMD_NX_STATUS
        call Proto_SendEmpty
        ld a,RESP_NX_STATUS
        ld de,NX_WAIT
        call Proto_WaitCmd
        ret c
        ld hl,ProtoBuf+1
        ld de,NxVersion
        ld bc,4
        ldir
        ld a,(ProtoBuf)
        or a
        ret

; Ядро — ZX Next (VERSION: личность 03), версия не ниже DE. Выход: CF=0 — да.
Nx_IsNext:
        ld a,(NxVersion+3)
        cp 3
        scf
        ret nz
        ld hl,(NxVersion)
        or a
        sbc hl,de
        ret

; Ядро работает: раз в кадр INPUT, раз в STATUS_EVERY кадров — STATUS. Выход: CF=0 — F12 или
; ESP сама вернула Tang в слот 0; CF=1, HL — текст ошибки.
Nx_Run:
        call Ev_Reset                    ; меню F1: поток 1 — при первом запросе (evo_menu.asm)
        ld a,STATUS_EVERY
        ld (NxDivider),a
.loop:
        ld a,(EvFast)
        or a
        jr nz,.fast                      ; снимок на карту Evo: следующий ВВОД — сразу
        ei
        halt
.fast:
        call Nx_Input
        ld hl,UiErrLost
        ret c
        ld a,(NxExitKey)
        or a
        ret nz                           ; F12 (CF=0)
        ld hl,NxDivider
        dec (hl)
        jr nz,.loop
        ld (hl),STATUS_EVERY
        call Nx_Status
        ld hl,UiErrLost
        ret c
        cp NX_RUNNING
        jr z,.loop
        cp NX_ERROR
        jr z,Nx_ErrorText
        or a                             ; ESP уже вернула слот 0 сама
        ret

; Выход: EXIT, сохранение и ожидание слота 0; APP_STOP только после результата.
Nx_Exit:
        ld a,(NxAppStarted)
        or a
        ret z
.request:
        ld a,CMD_NX_EXIT
        call Proto_SendEmpty
        ld a,RESP_NX_EXIT
        ld de,NX_WAIT
        call Proto_WaitCmd
        ld hl,3000
        ld (NxTimer),hl
        ld a,POLL_EVERY
        ld (NxDivider),a
.loop:
        ei
        halt
        ld hl,(NxTimer)
        dec hl
        ld (NxTimer),hl
        ld a,h
        or l
        jr z,.timeout
        ld hl,NxDivider
        dec (hl)
        jr nz,.loop
        ld (hl),POLL_EVERY
        call Nx_Status
        jr c,.loop
        cp 6
        jr z,.save_evo
        cp 7
        jr z,.save_error
        cp NX_BACK
        jr z,.stop
        cp NX_ERROR
        jr nz,.not_error
        ld a,(NsStarted)
        or a
        jr z,.stop
        ld hl,NsSize
        call Ns_IsZero
        jr nz,.save_error
        jr .stop
.not_error:
        or a
        jr z,.stop                       ; ядро так и не грузилось
        jr .loop
.save_evo:
        call Nes_SaveWrite
        jr .continue
.save_error:
        call Core_SaveFailure
.continue:
        ld hl,3000
        ld (NxTimer),hl
        jp .loop
.timeout:
        call Nx_VideoBack
        call Ui_Open
        ld hl,NsErrTimeout
        call Ui_Status
        call Ns_AskRetry
        cp 1
        jp z,.request
        ; Esc — явный отказ; тайм-аут сам по себе сохранение не уничтожает.
.stop:
        ld a,CMD_APP_STOP
        call Proto_SendEmpty
        ld a,RESP_APP_STOP
        ld b,4
        call Nx_WaitLong
        xor a
        ld (NxAppStarted),a
        jp Nx_VideoBack                  ; Tang уже в TS-Config — снова текст WC

; Ядро работает: Evo — в режим FT812 (видеошина Evo молчит, M_SEL = 1 — знак для ядра, что
; Evo не сброшена). Режим кладётся в тень WC: его обработчик кадра сам выводит VCONFIG.
Nx_VideoOff:
        ld a,VID_FT812
        call WC_GVMOD
        ld a,1
        ld (NxFtMode),a
        ret

; Вернуть текст WC (режим, смещения, палитру), если плагин его менял.
Nx_VideoBack:
        ld a,(NxFtMode)
        or a
        ret z
        xor a
        ld (NxFtMode),a
        jp WC_MNGV_PL

; Ждать ответ A не дольше B × 65535 опросов. Выход: CF=0 — пришёл (ProtoBuf).
Nx_WaitLong:
        ld (NxWant),a
.loop:
        push bc
        ld a,(NxWant)
        ld de,#FFFF
        call Proto_WaitCmd
        pop bc
        ret nc
        djnz .loop
        scf
        ret

; --- клавиатура, мышь, джойстик ---------------------------------------------------------
; Как загрузчик DOOM VDAC4: клавиатура — только очередь PS/2 (набор 2) AVR Evo, матрица ZX не
; читается. Регистры Gluk: #EFF7 = #80 открывает их, #DFF7 — номер, #BFF7 — данные. Первый
; опрос включает очередь: #0C = 1 (сброс буфера), #F0 = 2 (приём с клавиатуры). Дальше #F0 —
; очередной байт: 0 — пусто, #FF — переполнение (его разбирает слой ESP). Мышь Kempston: #FADF
; — кнопки (бит 0 левая, 1 правая, 2 средняя, нажатая — 0), #FBDF — X, #FFDF — Y, счётчики по
; модулю 256; приращение считает слой ESP. F12 (#07 без префиксов E0/F0) — выход.
; Джойстик — #1F целиком: 0 вправо, 1 влево, 2 вниз, 3 вверх, 4 огонь (B Sega), 5 C, 6 A, 7 START
; (как TSLib Input.Kempston в R-Type); нужные биты выбирает слой ESP (Next — 0…4).
; INPUT [джойстик, кнопки, X, Y, байты PS/2…]. Выход: CF=1 — ESP не ответила.
Nx_Input:
        ld hl,KeyBuf
        in a,(#1F)
        cp #FF
        jr nz,.kempston
        xor a
.kempston:
        ld (hl),a
        inc hl
        ld bc,#FADF
        in a,(c)
        ld (hl),a
        inc hl
        inc b                            ; #FBDF
        in a,(c)
        ld (hl),a
        inc hl
        ld b,#FF                         ; #FFDF
        in a,(c)
        ld (hl),a
        inc hl
        ld bc,#EFF7
        ld a,#80
        out (c),a
        ld a,(Ps2Ready)
        or a
        jr nz,.queue
        inc a
        ld (Ps2Ready),a
        ld b,#DF
        ld a,#0C
        out (c),a
        ld b,#BF
        ld a,1                           ; сброс буфера
        out (c),a
        ld b,#DF
        ld a,#F0
        out (c),a
        ld b,#BF
        ld a,2                           ; приём с клавиатуры
        out (c),a
.queue:
        ld b,#DF
        ld a,#F0
        out (c),a
        ld b,#BF
        ld e,PS2_DRAIN
.drain:
        in a,(c)
        or a
        jr z,.send                       ; очередь пуста
        ld (hl),a
        inc hl
        call Nx_Key
        dec e
        jr nz,.drain
.send:
        ld de,KeyBuf
        or a
        sbc hl,de
        ld b,h
        ld c,l
        ld hl,KeyBuf
        ld a,CMD_NX_INPUT
        call Proto_Send
        ld a,RESP_NX_INPUT
        ld de,NX_WAIT
        call Proto_WaitCmd
        ret c
        jp Ev_Service                    ; в ответе может быть запрос меню F1 (evo_menu.asm)

; Время Evo для часов DS1307 ядра (NEXT.ELF 0.8+, ядро Next 0.38+): часы Gluk -> TIME (#74)
; [секунды, минуты, часы, число, месяц, год 0…99], двоичные; день недели слой считает по дате.
; Только ядру ZX Next с часами (0.38+): у других слоёв #74 — своя команда. Выход: CF=1 — нет ответа.
Nx_SendTime:
        ld de,RTC_CORE
        call Nx_IsNext
        ret c
        call Rtc_Open
        call Rtc_Read
        ld hl,TimeBuf
        ld a,(RtcSec)
        ld (hl),a
        inc hl
        ld a,(RtcMin)
        ld (hl),a
        inc hl
        ld a,(RtcHour)
        ld (hl),a
        inc hl
        ld a,(RtcDay)
        ld (hl),a
        inc hl
        ld a,(RtcMonth)
        ld (hl),a
        inc hl
        push hl
        ld hl,(RtcYear)
        ld de,-2000
        add hl,de                        ; год — две цифры
        ld a,l
        pop hl
        ld (hl),a
        ld hl,TimeBuf
        ld bc,6
        ld a,CMD_NX_TIME
        call Proto_Send
        ld a,RESP_NX_TIME
        ld de,NX_WAIT
        jp Proto_WaitCmd

; Очередь PS/2 AVR — только плагину: WC перестаёт разбирать её в своём прерывании (INT_PL,
; A' = 2). Включает разбор снова сам WC, когда плагин вернётся.
Nx_KeysOwn:
        ld a,2
        call WC_INT_PL
        ld a,1
        ld (NxKbdOwn),a
        ret

; Esc для окна ошибки. Пока очередь PS/2 читает плагин (Nx_KeysOwn), WC клавиш не видит и
; WC_ESC молчал бы: Esc (#76 без префиксов) ищется в самой очереди. Выход: NZ — Esc нажата.
Nx_Esc:
        ld a,(NxKbdOwn)
        or a
        jp z,WC_ESC
        ld bc,#EFF7
        ld a,#80
        out (c),a
        ld b,#DF
        ld a,#F0
        out (c),a
        ld b,#BF
        ld e,PS2_DRAIN
.drain:
        in a,(c)
        or a
        ret z                            ; очередь пуста — Esc нет (Z)
        cp #76
        jr nz,.key
        ld a,(Ps2Prefix)
        or a
        jr z,.esc
        ld a,#76
.key:
        call Nx_Key
        dec e
        jr nz,.drain
        xor a
        ret
.esc:
        inc a                            ; NZ
        ret

; Байт PS/2 A: F12 без префикса — NxExitKey. Регистры, кроме флагов, — как были.
Nx_Key:
        push af
        cp #F0
        jr z,.release
        cp #E0
        jr z,.extended
        cp PS2_F12
        jr nz,.plain
        ld a,(Ps2Prefix)
        or a
        jr nz,.plain
        ld a,1
        ld (NxExitKey),a
.plain:
        xor a
        ld (Ps2Prefix),a
        pop af
        ret
.release:
        ld a,(Ps2Prefix)
        or 1
        ld (Ps2Prefix),a
        pop af
        ret
.extended:
        ld a,(Ps2Prefix)
        or 2
        ld (Ps2Prefix),a
        pop af
        ret

WC_WAITUP:
        ld a,FN_WAITUP
        jp WC_API

; --- данные -------------------------------------------------------------------------------
CrCount:        db 0
CrName:         ds 16
CrList:         ds MAX_CORES*ENTRY_SIZE
MenuSel:        db 0
MenuTop:        db 0
NxTimer:        dw 0
NxDivider:      db 0
NxWant:         db 0
NxExitKey:      db 0
NxAppStarted:   db 0
NxFtMode:       db 0                    ; 1 — Evo в режиме FT812 (Nx_VideoOff)
NxKbdOwn:       db 0                    ; 1 — очередь PS/2 читает плагин, не WC (Nx_KeysOwn)
NxVersion:      ds 4                    ; VERSION ядра из STATUS: [3] — личность (03 — ZX Next)
Ps2Ready:       db 0
Ps2Prefix:      db 0                    ; 1 — был F0, 2 — был E0
KeyBuf:         ds 4+PS2_DRAIN
TimeBuf:        ds 6                    ; TIME: сек, мин, час, число, месяц, год 0…99
