; Точка входа плагина. PLUGIN обязан быть первым байтом по адресу #8000.
;
; Плагин читает /zifi/zifi.ini, перезапускает ESP, подключает её к Wi-Fi и
; спрашивает опубликованную версию (ONLINE_UPDATE_CHECK: ESP читает с GitHub
; только манифест firmware.sha256). По Enter ESP сама качает firmware.bin,
; сверяет SHA-256 с манифестом, пишет образ в свободный раздел OTA и
; перезапускается (ONLINE_UPDATE); сам образ через Z80 не идёт.
;
; IX при входе указывает на служебную структуру Wild Commander; поле IX+29 —
; устройство активной панели, его надо забрать до того, как IX займут окна.

PLUGIN:
        push ix
        ld a,(ix+29)
        ld (ConfigPanelDevice),a
        ; Страница плагина живёт в памяти между запусками, поэтому состояние
        ; обнуляется здесь, а не полагается на начальные db.
        xor a
        ld (UpdaterError),a
        ld (UpdaterKeepWaiting),a
        ld (UpdaterVersionValid),a
        ld (ProtoErr),a
        ld (ProtoBurstLeft),a
        ld a,ONLINE_UPDATE_DIFFERENT
        ld (UpdaterRelation),a
        call Proto_ResetRx
        ld a,1
        call WC_INT_PL                   ; WC не должен перерисовывать часы под окном
        call WC_GEDPL
        call Ui_Open
        call Ui_Draw

        call Config_Load
        jp c,.config_error

        ld hl,UiDetecting
        call Ui_SetStatus
        call Ui_Draw
        call Updater_StartLink
        jp c,.link_error

        ld hl,UiChecking
        call Ui_SetStatus
        xor a
        call Ui_SetProgress
        call Ui_Draw
        call Updater_CheckPublished
        jp c,.update_error
        call Updater_ResolveCurrent

        ; Понижение запрещено до подтверждения: при ONLINE_UPDATE_OLDER Enter
        ; не опрашивается и CMD_ONLINE_UPDATE не отправляется вовсе.
        ld a,(UpdaterRelation)
        cp ONLINE_UPDATE_OLDER
        jp z,.wait_exit

.confirm:
        ei
        halt
        call WC_ESC
        jp nz,.cancel
        ld bc,#BFFE                    ; ENTER: младший бит строки H/J/K/L/Enter
        in a,(c)
        and 1
        jr nz,.confirm
.release_enter:
        ei
        halt
        in a,(c)
        and 1
        jr z,.release_enter

        ld hl,UiManifest
        call Ui_SetStatus
        xor a
        call Ui_SetProgress
        call Ui_Draw
        xor a
        ld (ProtoErr),a
        ld a,CMD_ONLINE_UPDATE
        call Proto_SendEmpty
        jp c,.update_error
        ld a,RESP_ACK
        ld de,UPDATER_WAIT
        call Proto_WaitCmd
        jp c,.update_error
        ; Ответ: [успех][SHA-256 записанного образа, 32 байта].
        ld a,RESP_ONLINE_UPDATE
        call Updater_WaitResult
        jr c,.update_error
        ld hl,(ProtoRxLen)
        ld de,33
        or a
        sbc hl,de
        jr nz,.update_error
        ld a,(ProtoBuf)
        or a
        jr z,.update_error

        ld hl,UiRestarting
        call Ui_SetStatus
        ld a,100
        call Ui_SetProgress
        call Ui_Draw
        call Updater_WaitRestart
        jr c,.restart_unknown
        call Updater_ReadVersion
        ld hl,UiCompleted
        call Ui_SetStatus
        call Ui_Draw
        jr .wait_exit

.restart_unknown:
        ld hl,UiRestartUnknown
        call Ui_SetStatus
        call Ui_Draw
        jr .wait_exit

.config_error:
        ld a,(ConfigError)
        cp 1
        ld hl,UiErrorSd
        jr z,.fatal
        cp 2
        ld hl,UiErrorDir
        jr z,.fatal
        cp 4
        ld hl,UiErrorIniLarge
        jr z,.fatal
        ld hl,UiErrorIni
        jr .fatal

.link_error:
        ld a,(UpdaterError)
        cp 1
        ld hl,UiErrorNoEsp
        jr z,.fatal
        cp 2
        ld hl,UiErrorNoReply
        jr z,.fatal
        ld hl,UiErrorWifi
        ld a,(ProtoErr)
        or a
        jr z,.fatal
        ld hl,ProtoErrText
        jr .fatal

.update_error:
        ld a,(ProtoErr)
        or a
        ld hl,UiErrorUpdate
        jr z,.fatal
        ld hl,ProtoErrText               ; причина от ESP, например update-check:manifest http 404
.fatal:
        call Ui_SetStatus
        call Ui_Draw
.wait_exit:
        ei
        halt
        call WC_ESC
        jr z,.wait_exit
        jr .exit

.cancel:
        ld hl,UiCancelled
        call Ui_SetStatus
        call Ui_Draw
.exit:
        call Config_RestoreRoot
        ld ix,UpdateWindow
        call WC_RRESB
        xor a
        pop ix
        ret

UPDATER_WAIT        equ 12000
UPDATER_IDLE_TICKS  equ 250             ; около 5 секунд до проверки PING
UPDATER_SILENCE     equ 60              ; около 5 минут без единого ответа

; Связь с ESP, перезапуск, текущая версия и Wi-Fi по zifi.ini.
; Выход: CF=1 и UpdaterError (1 — ESP нет на SPI, 2 — нет READY, 3 — Wi-Fi).
Updater_StartLink:
        xor a
        ld (UpdaterError),a
        call ZiFi_Init                   ; ESP на SPI и чистый сеанс канала
        jr nc,.esp_ok
        ld a,1
        ld (UpdaterError),a
        scf
        ret
.esp_ok:
        ; Как FTP и SMB, начинаем с чистого состояния ESP: RESET ZX модуль не
        ; перезапускает, и без SYS_RESET остались бы прежний сервер, сетевые
        ; запросы, приложение звука и раздробленная куча, а HTTPS-загрузке нужна
        ; память. ACK не ждём: готовность — только READY от новой загрузки.
        ld hl,UiResetting
        call Ui_SetStatus
        call Ui_Draw
        ld a,CMD_SYS_RESET
        call Proto_SendEmpty
        call Updater_WaitReady
        jr nc,.ready
        ld a,2
        ld (UpdaterError),a
        scf
        ret
.ready:
        call Updater_ReadVersion

        ld hl,UiConnecting
        call Ui_SetStatus
        call Ui_Draw
        xor a
        ld (ProtoErr),a
        ld a,CMD_WIFI_INI
        ld hl,IniBuffer
        ld bc,(IniLength)
        call Proto_Send
        jr c,.wifi_fail
        ld a,RESP_ACK
        ld de,UPDATER_WAIT
        call Proto_WaitCmd
        jr c,.wifi_fail
        call Updater_WaitWifiResult
        jr c,.wifi_fail
        ld a,(ProtoBuf)
        or a
        jr z,.wifi_fail
        or a
        ret
.wifi_fail:
        ld a,3
        ld (UpdaterError),a
        scf
        ret

; WIFI_INI может прислать сначала доклад #EE (например, не сохранилась копия
; zifi.ini), а затем обязательный ответ #83 со статусом подключения. Здесь
; доклад запоминается, но ждём сам #83.
Updater_WaitWifiResult:
        ld a,1
        ld (UpdaterKeepWaiting),a
        ld a,RESP_WIFI_INI
        call Updater_WaitResult
        push af
        xor a
        ld (UpdaterKeepWaiting),a
        pop af
        ret

Updater_WaitReady:
        ld b,30
.try:
        push bc
        ld a,CMD_PING
        call Proto_SendEmpty
        ld a,RESP_READY
        ld de,UPDATER_WAIT
        call Proto_WaitCmd
        pop bc
        ret nc
        djnz .try
        scf
        ret

; Прочитать только опубликованный манифест; flash ESP на запись не открывается.
; Ответ: [1][отношение][опубликованная версия ASCII]. SAME разрешает
; переустановку, NEWER — обновление, OLDER блокируется вызывающим кодом.
Updater_CheckPublished:
        xor a
        ld (ProtoErr),a
        ld a,CMD_ONLINE_UPDATE_CHECK
        call Proto_SendEmpty
        ret c
        ld a,RESP_ACK
        ld de,UPDATER_WAIT
        call Proto_WaitCmd
        ret c
        ld a,RESP_ONLINE_UPDATE_CHECK
        call Updater_WaitResult
        ret c
        ld hl,(ProtoRxLen)
        ld de,3
        or a
        sbc hl,de
        jr c,.bad
        ld a,(ProtoBuf)
        or a
        jr z,.bad
        ld hl,(ProtoRxLen)
        ld de,ProtoBuf
        add hl,de
        ld (hl),0
        ld hl,ProtoBuf+2
        call Ui_SetAvailable

        ld a,(ProtoBuf+1)
        ld (UpdaterRelation),a
        cp ONLINE_UPDATE_SAME
        ld hl,UiSameVersion
        jr z,.show
        cp ONLINE_UPDATE_NEWER
        ld hl,UiNewerVersion
        jr z,.show
        cp ONLINE_UPDATE_OLDER
        ld hl,UiOlderVersion
        jr z,.show
        ld hl,UiDifferentVersion
.show:
        call Ui_SetStatus
        call Ui_Draw
        or a
        ret
.bad:
        scf
        ret

; SYS_INFO сразу после перезапуска может потеряться на границе новой загрузки,
; поэтому до трёх самостоятельных запросов. Неизвестная версия не запрещает
; переустановку.
Updater_ReadVersion:
        xor a
        ld (UpdaterVersionValid),a
        ld b,3
.try:
        push bc
        xor a
        ld (ProtoErr),a
        ld a,CMD_SYS_INFO
        call Proto_SendEmpty
        jr c,.retry
        ld a,RESP_ACK
        ld de,UPDATER_WAIT
        call Proto_WaitCmd
        jr c,.retry
        ld a,RESP_SYS_INFO
        ld de,UPDATER_WAIT
        call Proto_WaitCmd
        jr nc,.got
.retry:
        pop bc
        djnz .try
        scf
        ret
.got:
        pop bc
        ld hl,ProtoBuf
        ld bc,(ProtoRxLen)
        add hl,bc
        ld (hl),0
        call Updater_FindVersion
        ret c
        call Ui_SetVersion
        call Ui_Draw
        ld a,1
        ld (UpdaterVersionValid),a
        or a
        ret

; После удачной проверки поле Current не должно остаться «detecting...».
; Сначала ещё раз SYS_INFO уже на устоявшейся ESP. Потерян и он — отношение
; SAME доказывает, что строки версий на ESP равны, и Current берётся из
; Available.
Updater_ResolveCurrent:
        ld a,(UpdaterVersionValid)
        or a
        ret nz
        call Updater_ReadVersion
        ld a,(UpdaterVersionValid)
        or a
        ret nz
        ld a,(UpdaterRelation)
        cp ONLINE_UPDATE_SAME
        jr nz,.unavailable
        ld hl,UiAvailableField
        ld de,UiVersionField
        ld bc,UI_VERSION_WIDTH
        ldir
        ld a,1
        ld (UpdaterVersionValid),a
        jp Ui_Draw
.unavailable:
        ld hl,UiUnavailable
        call Ui_SetVersion
        jp Ui_Draw

; Поле «FW:» в ответе SYS_INFO. Выход: HL — версия; CF=1 — поля нет.
Updater_FindVersion:
        ld hl,ProtoBuf
.scan:
        ld a,(hl)
        or a
        jr z,.absent
        cp 'F'
        jr nz,.next
        inc hl
        ld a,(hl)
        cp 'W'
        jr nz,.scan
        inc hl
        ld a,(hl)
        cp ':'
        jr nz,.scan
        inc hl
        or a
        ret
.next:
        inc hl
        jr .scan
.absent:
        ld hl,UiUnknown
        scf
        ret

; Ожидание долгой сетевой операции. Попутно показывает индикатор
; [этап][проценты] (CHECK и ONLINE_UPDATE). Доклад #EE возвращается сразу
; (кроме ожидания WIFI_INI): так прошивка сообщает и об отказе, и о том, что
; команды не знает.
Updater_WaitResult:
        ld (UpdaterWanted),a
        ld a,UPDATER_SILENCE
        ld (UpdaterSilence),a
        ld hl,UPDATER_IDLE_TICKS
        ld (UpdaterTicks),hl
.loop:
        call Proto_Poll
        jr nc,.idle
        ld hl,UpdaterWanted
        cp (hl)
        jr z,.got
        cp RESP_ERROR
        jr z,.error
        cp RESP_READY
        jr z,.alive
        cp EVT_ONLINE_UPDATE_PROGRESS
        jr nz,.loop
        call Updater_ShowProgress
.alive:
        ld a,UPDATER_SILENCE
        ld (UpdaterSilence),a
        ld hl,UPDATER_IDLE_TICKS
        ld (UpdaterTicks),hl
        jr .loop
.error:
        ld a,1
        ld (ProtoErr),a
        call Proto_SaveErr
        ld a,(UpdaterKeepWaiting)
        or a
        jr nz,.alive
        scf
        ret
.idle:
        ei
        halt
        ld hl,(UpdaterTicks)
        dec hl
        ld (UpdaterTicks),hl
        ld a,h
        or l
        jr nz,.loop
        ld hl,UPDATER_IDLE_TICKS
        ld (UpdaterTicks),hl
        ld hl,UpdaterSilence
        dec (hl)
        jr z,.silent
        ld a,CMD_PING
        call Proto_SendEmpty
        jr .loop
.silent:
        scf
        ret
.got:
        or a
        ret

; Событие [этап][проценты]: 1 — манифест, 2 — загрузка образа, 3 — проверка.
Updater_ShowProgress:
        ld hl,(ProtoRxLen)
        ld de,2
        or a
        sbc hl,de
        ret nz
        ld a,(ProtoBuf+1)
        cp 101
        ret nc
        call Ui_SetProgress
        ld a,(ProtoBuf)
        cp 1
        ld hl,UiManifest
        jr z,.show
        cp 2
        ld hl,UiDownloading
        jr z,.show
        cp 3
        ld hl,UiVerifying
        ret nz
.show:
        call Ui_SetStatus
        jp Ui_Draw

; Итоговый ответ ESP отправляет перед перезапуском. Пауза — чтобы не принять
; READY ещё от старой загрузки, затем ждём READY от новой.
Updater_WaitRestart:
        ld b,60                         ; около секунды на начало перезапуска
.pause:
        ei
        halt
        djnz .pause
        jp Updater_WaitReady

        INCLUDE "ui.asm"

UpdaterError:       db 0
UpdaterWanted:      db 0
UpdaterSilence:     db 0
UpdaterTicks:       dw 0
UpdaterKeepWaiting: db 0
UpdaterRelation:    db ONLINE_UPDATE_DIFFERENT
UpdaterVersionValid: db 0
