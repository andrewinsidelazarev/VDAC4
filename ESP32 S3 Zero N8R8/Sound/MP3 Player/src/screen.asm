; Окно плагина и экран проигрывателя на HDMI Tang (плагин 1.21, PLAYER 1.26).
;
; Есть Tang с оверлеями — интерфейс проигрывателя рисует PLAYER на ESP оверлеем поверх
; картинки Evo, а окна плагина на экране Evo нет: под экраном Tang видны панели WC. Тогда
; плагин и по SPI спрашивает только нужное звуку и экрану (AUDIO_STATUS с телом, данные файла,
; обложку файлом): спектра (AUDIO_VIEW), меток (AUDIO_TAGS, AUDIO_INFO) и миниатюры обложки
; (AUDIO_THUMB) окну не нужно.
;
; Окно скрыто (UiHidden) — переходники WC_PRWOW, WC_RRESB и WC_TXTPR (main.asm) ничего не
; делают, Ui_View, Ui_FetchTags и Cover_Poll ничего не спрашивают. С начала вызова окно скрыто:
; - после PING — VERSION Tang (Ui_Probe): ядра TS-Config 1.10+ нет — окно сразу, как прежде;
; - ответ AUDIO_OPEN (Ui_Screen): флаг SCREEN_F_TANG — окна нет, иначе (и у прежнего PLAYER) —
;   окно;
; - каждый AUDIO_STATUS (Ui_Track): экран показан (SCREEN_F_SHOWN) — окна нет; окна нет, а
;   экрана нет UI_TANG_WAIT кадров (PLAYER не нашёл памяти, Tang ушёл) — окно;
; - ошибка (Mp3_Fatal) — окно. Передача модуля в ESP (Esp_Prepare) окна не открывает: без Tang
;   оно уже открыто (Ui_Probe), с Tang — его нет и тут (плагин 1.22).
; Код — во второй странице (MODULE P1): из страницы 0 — через Page1_Call.

CMD_TANG_READ   equ #45                 ; [адрес LE16][слов 1…16] -> [1/0][слова LE32] (0.1.72+)
RESP_TANG_READ  equ #C5
SCREEN_F_SHOWN  equ 1                   ; флаги экрана Tang: экран сейчас на HDMI,
SCREEN_F_TANG   equ 2                   ; ... Tang может его показать (ответы PLAYER 1.26)
UI_TANG_WAIT    equ 250                 ; кадров ждать экран Tang, потом — окно (5 с: первый показ
                                        ; с холстом в Tang на плате — 1,8 с)

; После PING: Tang с оверлеями (VERSION 0x04: [31:24] личность 01 — TS-Config, [15:0]
; версия 1.10+) — окну ждать ответа AUDIO_OPEN; иначе (нет Tang, прошивка старше 0.1.72) —
; окно сразу.
Ui_Probe:
        ld a,CMD_TANG_READ
        ld hl,UiTangQuery
        ld bc,3
        call Proto_Send
        jr c,.window
        ld a,RESP_TANG_READ
        ld de,MP3_WAIT
        call Mp3_Wait
        jr c,.window
        ld a,(ProtoBuf)
        or a
        jr z,.window
        ld a,(ProtoBuf+4)
        cp 1
        jr nz,.window
        ld hl,(ProtoBuf+1)
        ld de,#010A
        or a
        sbc hl,de
        ret nc
.window:
        xor a
        ld (ProtoErr),a                  ; отказ старой прошивки — не причина другой ошибки
        jr Ui_Show

; Ответ AUDIO_OPEN (ProtoBuf): шестой байт — флаги экрана Tang.
Ui_Screen:
        xor a
        ld (UiTangWait),a
        ld a,(WC_FRAMES)
        ld (UiTangFrame),a
        ld hl,(ProtoRxLen)
        ld de,6
        or a
        sbc hl,de
        jr c,Ui_Show                     ; прежний PLAYER: экрана нет
        ld a,(ProtoBuf+5)
        and SCREEN_F_TANG
        jr z,Ui_Show
        jr Ui_Hide

; После AUDIO_STATUS (Mp3Status): см. начало файла.
Ui_Track:
        ld a,(WC_FRAMES)
        ld hl,UiTangFrame
        ld c,(hl)
        ld (hl),a
        sub c
        ld c,a                           ; кадров с прошлого раза
        ld a,(Mp3Status+ST_SCREEN)
        and SCREEN_F_SHOWN
        jr z,.absent
        xor a
        ld (UiTangWait),a
        jr Ui_Hide
.absent:
        ld a,(UiHidden)
        or a
        ret z
        ld a,(UiTangWait)
        add a,c
        jr nc,.sum
        ld a,255
.sum:
        ld (UiTangWait),a
        cp UI_TANG_WAIT
        ret c
        call Ui_Show
        jp Ui_FetchTags                  ; без окна их не спрашивали

; Окно на Evo — показать. Окна поверх без окна не открывались — закрыты; миниатюра обложки,
; если ждёт, придёт сама (Cover_Poll).
Ui_Show:
        ld a,(UiHidden)
        or a
        ret z
        xor a
        ld (UiHidden),a
        ld (UiDialog),a
        ld ix,Mp3Window
        ld (ix+2),#FF
        ld (ix+3),#FF
        ld (ix+8),0
        ld (ix+9),0
        call WC_PRWOW
        jp Ui_Flush

; Окно на Evo — убрать: экран Tang показан. Спрайт обложки — прочь, её миниатюра — снова
; к забору, если окно вернётся.
Ui_Hide:
        ld a,(UiHidden)
        or a
        ret nz
        call Ui_Close
        call Ui_SpritesOff
        ld a,(CoverState)
        cp COVER_SHOWN
        jr nz,.window
        ld a,COVER_WAIT
        ld (CoverState),a
.window:
        ld ix,Mp3Window
        call WC_RRESB
        ld a,1
        ld (UiHidden),a
        ret

; Тело AUDIO_STATUS (Ui_PackBlock; формат и постоянные UI_BLOCK_* — в ui.asm) и опрос
; экрана во время подготовки файла (Mp3_Alive): здесь, во второй странице, — в первой
; места нет (плагин 1.23); входы из страницы 0 — в esp_data.asm.
Ui_PackBlock:
        ld de,UiBlock
        ld a,UI_BLOCK_VERSION
        ld (de),a
        inc de
        ld a,(UiBlockFlags)
        ld (de),a
        inc de
        ld a,(UiDialog)
        ld (de),a
        inc de
        ld hl,(Mp3Count)
        ld a,h
        or l
        ld hl,0
        ld bc,0
        jr z,.track                      ; вызван не на файле панели: номера нет
        ld hl,(TrackNo)
        ld bc,(TrackCount)
.track:
        ex de,hl                         ; HL — куда, DE — номер
        ld (hl),e
        inc hl
        ld (hl),d
        inc hl
        ld (hl),c
        inc hl
        ld (hl),b
        inc hl
        ex de,hl
        ld hl,Mp3Sent
        ld bc,4
        ldir
        ld hl,UiName
        call .text
        ld hl,(UiState)
        call .text
        ld a,(EspPreparing)
        ld (de),a
        inc de
        ld hl,(UiLoadSent)
        ld bc,4
        ldir
        ld hl,(UiLoadTotal)
        ld bc,4
        ldir
        ex de,hl
        ld de,UiBlock
        or a
        sbc hl,de
        ld b,h
        ld c,l
        ld hl,UiBlock
        ret
; Строка HL — по DE, не длиннее UI_BLOCK_TEXT знаков, с нулём; DE — за нулём.
.text:
        ld b,UI_BLOCK_TEXT
.copy:
        ld a,(hl)
        or a
        jr z,.end
        ld (de),a
        inc de
        inc hl
        djnz .copy
.end:
        xor a
        ld (de),a
        inc de
        ret

; Экран Tang, пока файл готовится и идёт в ESP (плагин 1.23): PLAYER уже запущен — не реже
; раза в MP3_ALIVE_FRAMES кадров AUDIO_STATUS с телом. По нему PLAYER знает, что окно открыто, и
; рисует строку состояния и ход передачи (модуль декодера, банк MIDI, файл целиком); экрана нет
; UI_TANG_WAIT кадров — окно на Evo (Ui_Track). Запись состояния не меняется: файл ещё не играет.
; Звать только между командами, не в ожидании другого ответа (Esp_WaitLong): ответ ESP на
; AUDIO_STATUS пришёл бы после того, и Mp3_Wait его пропустил бы.
Mp3_Alive:
        ld a,(EspAppRunning)
        or a
        ret z
        ld a,(WC_FRAMES)
        ld hl,Mp3AliveFrame
        sub (hl)
        cp MP3_ALIVE_FRAMES
        ret c
        ld a,(WC_FRAMES)
        ld (hl),a
        call Ui_PackBlock
        ld a,CMD_AUDIO_STATUS
        call Proto_Send
        jr c,.quiet
        ld a,RESP_AUDIO_STATUS
        ld de,MP3_WAIT
        call Mp3_Wait
        jr c,.quiet
        ld hl,(ProtoRxLen)
        ld de,ST_SCREEN+1
        or a
        sbc hl,de
        ret c                            ; прежний PLAYER: байта экрана нет
        ld a,(ProtoBuf+ST_SCREEN)
        ld (Mp3Status+ST_SCREEN),a
        jp Ui_Track
.quiet:
        xor a
        ld (ProtoErr),a                  ; молчание тут — не ошибка: ответит следующая команда
        ret

UiTangQuery:    db #04,#00,1            ; VERSION, одно слово
