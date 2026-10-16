; Точка входа плагина. PLUGIN обязан быть первым байтом по адресу #8000.
;
; Своего окна у плагина нет: он показывает экранное меню Tang поверх картинки HDMI и
; передаёт ему клавиши Evo — как кнопки S1/S2 Tang. Меню само показывает состояние входа
; и все настройки; изменённые настройки ESP сама сохраняет во flash Tang.
;   ↓ / ↑ — следующий / предыдущий пункт, → / ← — значение +1 / −1, Esc — скрыть меню и выйти.
; Нажатия уходят в регистр UI_KEY ядра Tang (1.3+) командой TANG_WRITE прошивки ESP
; (vdac4-0.1.73+). Окно WC — только для сообщения об ошибке.
;
; Плагин не перезапускает ESP (как FTP или Online Update): LINK к Tang у ESP уже поднят,
; перезапуск оборвал бы его на ~13 с. Достаточно сеанса канала SPI (ZiFi_Init).

CMD_TANG_READ   equ #45                 ; [адрес LE16][слов 1…16] -> [1/0][слова LE32]
CMD_TANG_WRITE  equ #46                 ; [адрес LE16][слов][слова LE32] -> [1/0]
RESP_TANG_READ  equ #C5
RESP_TANG_WRITE equ #C6

VIDEO_WAIT      equ 6000                ; попыток опроса ответа (ESP отвечает сразу)
VIDEO_KEEP      equ 250                 ; кадров между повторами «показать» (5 с; меню
                                        ; Tang само скрывается через 20 с без нажатий)

TANG_UI_KEY     equ #50
KEY_NEXT        equ 1
KEY_PREV        equ 2
KEY_INC         equ 3
KEY_DEC         equ 4
KEY_SHOW        equ 5
KEY_HIDE        equ 6

TANG_MIN_MINOR  equ 3                   ; UI_KEY — с ядра 1.3

PLUGIN:
        push ix
        ; Страница плагина живёт в памяти между запусками, поэтому состояние
        ; обнуляется здесь, а не полагается на начальные db.
        xor a
        ld (ProtoErr),a
        ld (ProtoBurstLeft),a
        ld (VideoWindowOpen),a
        ld a,VIDEO_KEEP
        ld (VideoTimer),a
        call Proto_ResetRx
        ld a,1
        call WC_INT_PL                   ; WC не должен перерисовывать часы под меню
        call WC_GEDPL

        call ZiFi_Init                   ; ESP на SPI и чистый сеанс канала
        ld hl,UiErrNoEsp
        jp c,.fatal

        ; ID и версия ядра Tang
        ld hl,#0000
        ld b,2
        call Tang_Read
        ld hl,UiErrNoLink
        jp c,.fatal
        ld hl,ProtoBuf+1
        ld de,TangId
        ld b,4
.id:
        ld a,(de)
        cp (hl)
        jr nz,.not_vdc4
        inc hl
        inc de
        djnz .id
        ld a,(ProtoBuf+6)                ; VERSION [15:8] — старший номер
        ld (TangMajor),a
        ld a,(ProtoBuf+5)                ; VERSION [7:0] — младший
        ld (TangMinor),a
        ld a,(TangMajor)
        cp 1
        jr nz,.version_ok                ; не 1.x — новее
        ld a,(TangMinor)
        cp TANG_MIN_MINOR
        jr c,.old_core
.version_ok:
        ld a,KEY_SHOW
        call Tang_Key
        ld hl,UiErrLost
        jp c,.fatal

.loop:
        ei
        halt
        call WC_ESC
        jr nz,.leave
        call Video_Keys
        ld hl,UiErrLost
        jr c,.fatal
        ld hl,VideoTimer
        dec (hl)
        jr nz,.loop
        ld (hl),VIDEO_KEEP
        ld a,KEY_SHOW                    ; меню не должно скрыться, пока плагин работает
        call Tang_Key
        ld hl,UiErrLost
        jr c,.fatal
        jr .loop

.leave:
        ld a,KEY_HIDE
        call Tang_Key
        jr .exit

.not_vdc4:
        ld hl,UiErrNotVdc4
        jr .fatal
.old_core:
        call Ui_SetCore
        ld hl,UiErrOldCore
.fatal:
        push hl
        call Ui_Open
        pop hl
        call Ui_SetStatus
        call Ui_Draw
.wait_exit:
        ei
        halt
        call WC_ESC
        jr z,.wait_exit
.exit:
        ld a,(VideoWindowOpen)
        or a
        jr z,.done
        ld ix,VideoWindow
        call WC_RRESB
.done:
        xor a
        pop ix
        ret

; Клавиши Evo -> UI_KEY. Опрос раз за кадр (основной цикл ждёт HALT), как требует
; автоповтор WC. Выход: CF=1 — нажатие не дошло до Tang.
Video_Keys:
        call WC_KEY_DOWN
        ld a,KEY_NEXT
        jr nz,.send
        call WC_KEY_UP
        ld a,KEY_PREV
        jr nz,.send
        call WC_KEY_RIGHT
        ld a,KEY_INC
        jr nz,.send
        call WC_KEY_LEFT
        ld a,KEY_DEC
        jr nz,.send
        or a
        ret
.send:
        push af
        ld a,VIDEO_KEEP                  ; нажатие и так держит меню показанным
        ld (VideoTimer),a
        pop af
        jp Tang_Key

; --- регистры Tang -----------------------------------------------------------

; Нажатие A в UI_KEY. Выход: CF=1 — не записано.
Tang_Key:
        ld (TangWord),a
        xor a
        ld (TangWord+1),a
        ld (TangWord+2),a
        ld (TangWord+3),a
        ld hl,TANG_UI_KEY
        ld de,TangWord
        jp Tang_Write

; Прочитать B слов (1…16) с адреса HL. Выход: CF=0 — слова в ProtoBuf+1 (LE32),
; CF=1 — нет ответа или LINK у ESP не работает.
Tang_Read:
        ld (TangReq),hl
        ld a,b
        ld (TangReq+2),a
        xor a
        ld (ProtoErr),a
        ld a,CMD_TANG_READ
        ld hl,TangReq
        ld bc,3
        call Proto_Send
        ret c
        ld a,RESP_TANG_READ
        ld de,VIDEO_WAIT
        call Proto_WaitCmd
        ret c
        ld a,(ProtoBuf)
        or a
        scf
        ret z
        or a
        ret

; Записать слово DE (4 байта LE) по адресу HL. Выход: CF=1 — не записано.
Tang_Write:
        ld (TangReq),hl
        ld a,1
        ld (TangReq+2),a
        ex de,hl
        ld de,TangReq+3
        ld bc,4
        ldir
        xor a
        ld (ProtoErr),a
        ld a,CMD_TANG_WRITE
        ld hl,TangReq
        ld bc,7
        call Proto_Send
        ret c
        ld a,RESP_TANG_WRITE
        ld de,VIDEO_WAIT
        call Proto_WaitCmd
        ret c
        ld a,(ProtoBuf)
        or a
        scf
        ret z
        or a
        ret

TangId:         db #34,#43,#44,#56      ; ID = 0x56444334 ("VDC4"), байты LE
TangReq:        ds 7                    ; [адрес LE16][слов][слово LE32]
TangWord:       ds 4
TangMajor:      db 0
TangMinor:      db 0
VideoTimer:     db 0
VideoWindowOpen: db 0
