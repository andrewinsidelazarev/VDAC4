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

UiTangQuery:    db #04,#00,1            ; VERSION, одно слово
