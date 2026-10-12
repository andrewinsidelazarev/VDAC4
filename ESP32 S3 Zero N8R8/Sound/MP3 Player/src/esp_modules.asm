; Модули ESP из /WC/ESP/ на томе, откуда загружен WC (Docs/VDAC4.md, «Модули
; ESP»): STREAM #FD WC Improved; без него — на томе активной панели.
;
; Проигрыватель на ESP — приложение PLAYER.ELF, декодеры — MP3.ELF, AAC.ELF и
; OGG.ELF (поток: по расширению файла), TRACKER.ELF, MIDI.ELF и OPL.ELF; радио —
; MP3.ELF и, если есть, AAC.ELF и OGG.ELF (кодек станции PLAYER узнаёт по её
; потоку). ESP держит их копии в кэше своей flash; плагин перед
; проигрыванием сообщает ESP имя, длину и время записи каждого (MOD_QUERY) и
; передаёт файл, только если его в кэше нет или он изменился (MOD_PUT,
; MOD_COMMIT). Потом запускает приложение (APP_START). Ответов на MOD_QUERY
; (места нет — ESP освобождает его сама), MOD_COMMIT (модуль ESP пишет во
; flash) и APP_START плагин ждёт по времени, секунды, с выходом по Esc
; (Esp_WaitLong).
;
; Банк звуков MIDI.SF2 (до 7 МБ) и ПЗУ таблиц волн OPL4 YRW801.ROM (2 МБ) —
; ресурсы: MOD_QUERY с флагом 1, ESP держит их в PSRAM до перезапуска (или пока
; память не понадобится модулю), так что передаются они первый раз после
; включения. Ход передачи — полосой загрузки. Вместе они в PSRAM не помещаются:
; места нет — AUDIO_OPEN [#20] (PLAYER освобождает оба) и ещё раз. Банк нужен
; каждому MIDI, ПЗУ — только VGM для OPL4: его просит декодер (флаг состояния).
; Набор волн песни MoonBlaster Wave (.MWM) — тоже ресурс: «ИМЯ.MWK» из папки
; песни (имя — в самой песне), до открытия файла.
;
; Поиск в /WC/ESP идёт служебным потоком WC (или клоном потока панели), а
; LOAD512 читает активный поток, поэтому свой файл плагин потом находит заново
; (клон панели, FENTRY по имени 8.3 из ENTRY) и открывает с начала (GFILE) в
; потоке 0; перемотке и обложке (поток 1) это имя тоже нужно.
;
; Нет /WC/ESP, модуля или прошивка без модулей — MP3 и модули играет плеер,
; встроенный в прошивку (переходное время): плагин работает с ним тем же
; протоколом. MIDI, OPL, AAC и Ogg он не умеет: тогда — ошибка с именем того,
; что не вышло.
;
; Код — во второй странице плагина (окно #C000, MODULE P1, main.asm): в неё
; входят только через Page1_Call (входы Esp_Prepare и Esp_LoadRom — в
; esp_data.asm), API WC — только через Wc_Api. Постоянные, переменные, имена и
; тексты — в esp_data.asm, в странице 0: их читает и пишет WC и код страницы 0.

; Постоянные ESP_*, WHY_* — в esp_data.asm.

; Всё перед проигрыванием файла: запомнить свой файл, отдать модули (и банк
; MIDI), запустить приложение, снова открыть свой файл. Вход: (Mp3Kind).
; Выход: CF=1 — MIDI играть нечем, HL — текст ошибки (0 — нажат Esc).
Esp_Prepare:
        xor a
        ld (EspAppRunning),a
        ld (EspReopened),a
        ld (EspNeedsApp),a
        ld a,WHY_MISSING                 ; нет /WC/ESP — «missing /wc/esp/»
        ld (EspWhy),a
        ld hl,UiEmpty
        ld (EspName),hl
        ; Свой файл — по имени, которое WC передал в BC: TENTRY до этого отдаёт
        ; чужую запись (что DOS искал последним), WC файл плагину открывает по
        ; кластеру из панели. FENTRY в каталоге панели делает свой файл
        ; записью DOS — из неё имя 8.3 для поисков потом.
        call Esp_FindOwn
        ld de,Mp3Entry
        ld a,FN_TENTRY
        call Wc_Api
        call Esp_OwnName
        call Esp_ToModules
        jr c,.reopen
        ld hl,EspPlayer
        call Esp_Deliver
        jr c,.reopen
        ld a,(Mp3Kind)
        or a
        jp z,.stream
        cp KIND_RADIO
        jp z,.radio
        ld hl,EspTsfm
        cp KIND_TSFM
        jr z,.decoder
        ld hl,EspSaa
        cp KIND_SAA
        jr z,.decoder
        ld hl,EspTracker
        dec a
        jr z,.decoder
        ld hl,EspMidi
        dec a
        jr z,.decoder
        ld hl,EspOpl
.decoder:
        call Esp_Deliver
        jr c,.reopen
.start:
        ld hl,EspPlayer                  ; не запустилось — в ошибке PLAYER.ELF
        ld (EspName),hl
        xor a                            ; WHY_OTHER
        ld (EspWhy),a
        call Esp_StartApp
        call Esp_Kit
        ; Банк MIDI — после запуска: не хватит места — PLAYER освободит ресурсы.
        ld a,(Mp3Kind)
        cp KIND_MIDI
        jr nz,.reopen
        ld a,(EspAppRunning)
        or a
        jr z,.reopen
        ld hl,EspBank
        call Esp_Resource
        jr nc,.reopen
        xor a
        ld (EspAppRunning),a             ; MIDI без банка — ошибка с его именем
.reopen:
        call Esp_Reopen
        ld hl,0
        ld a,(EspWhy)
        cp WHY_ESC
        scf
        ret z                            ; Esc во время передачи — выйти
        ld a,(EspNeedsApp)               ; AAC и Ogg встроенный плеер не играет
        or a
        jr nz,.needs
        ld a,(Mp3Kind)
        cp KIND_MIDI
        jr c,.done
.needs:
        ld a,(EspAppRunning)
        or a
        jr nz,.done
        ; MIDI, OPL, AAC и Ogg без приложения ESP: причина — с именем того, что
        ; не вышло.
        ld a,(EspWhy)
        ld hl,UiErrorEspTook
        or a                             ; WHY_OTHER
        jr z,.text
        ld hl,UiErrorEspMissing
        dec a
        jr z,.text
        ld hl,UiErrorEspRoom
.text:
        ld de,EspErrorText
        call Ui_CopyText
        ld hl,(EspName)
        call Ui_CopyText
        ld hl,EspErrorText
        scf
        ret
.done:
        or a
        ret
.stream:
        ; Поток с карты: декодер — по расширению; не MP3.ELF — без приложения никак.
        call Esp_StreamName
        ld de,EspMp3
        or a
        sbc hl,de
        add hl,de
        jp z,.decoder
        ld a,1
        ld (EspNeedsApp),a
        jp .decoder
.radio:
        ; Радио: MP3.ELF — всегда (с него PLAYER начинает), AAC.ELF и OGG.ELF —
        ; если есть: кодек станции PLAYER узнаёт по её потоку.
        ld hl,EspMp3
        call Esp_Deliver
        jp c,.reopen
        ld hl,EspAac
        call Esp_Extra
        jp c,.reopen                     ; Esc
        ld hl,EspOgg
        call Esp_Extra
        jp c,.reopen
        jp .start

; Декодер потока с карты — по расширению (Mp3Ext): AAC — AAC.ELF, OGG и OGA —
; OGG.ELF, остальное (MP3, WAV) — MP3.ELF. Выход: HL — имя.
Esp_StreamName:
        ld hl,EspMp3
        ld de,(Mp3Ext)                   ; E — первая буква, D — вторая
        ld a,(Mp3Ext+2)
        ld c,a
        ld a,e
        cp 'A'
        jr nz,.ogg
        ld a,d
        cp 'A'
        ret nz
        ld a,c
        cp 'C'
        ret nz
        ld hl,EspAac
        ret
.ogg:
        cp 'O'
        jr nz,.flac
        ld a,d
        cp 'G'
        ret nz
        ld a,c
        cp 'G'
        jr z,.is_ogg
        cp 'A'
        ret nz
.is_ogg:
        ld hl,EspOgg
        ret
.flac:
        cp 'F'
        ret nz
        ld a,d
        cp 'L'
        ret nz
        ld a,c
        cp 'A'
        ret nz
        ld hl,EspFlac
        ret

; Модуль HL, без которого можно (радио: AAC.ELF, OGG.ELF): нет его в /WC/ESP или
; не вышло — дальше без него. CF=1 — только Esc.
Esp_Extra:
        call Esp_Deliver
        ret nc
        ld a,(EspWhy)
        cp WHY_ESC
        scf
        ret z
        or a
        ret

; FENTRY своего файла по имени из BC входа (длинному или 8.3) в каталоге
; панели. [#00][имя][0] собирается в Mp3Block: файл ещё не читался.
Esp_FindOwn:
        ld hl,(Mp3NamePtr)
        ld de,Mp3Block+1
        ld bc,255
.copy:
        ld a,(hl)
        ld (de),a
        or a
        jr z,.named
        inc hl
        inc de
        dec bc
        ld a,b
        or c
        jr nz,.copy
        ld (de),a                        ; длиннее 255 — обрезано
.named:
        ld (Mp3Block),a                  ; A=0: тип — файл
        ld d,STREAM_CLONE
        call WC_STREAM
        ld hl,Mp3Block
        jp WC_FENTRY

; Имя 8.3 своего файла для FENTRY ([#00][ИМЯ.РАС][0]) из записи Mp3Entry.
Esp_OwnName:
        ld hl,Mp3Entry
        ld de,Mp3FentryName+1
        ld b,8
.name:
        ld a,(hl)
        cp ' '
        jr z,.name_space
        ld (de),a
        inc de
.name_space:
        inc hl
        djnz .name
        ld a,(hl)
        cp ' '
        jr z,.named                      ; без расширения
        ld a,'.'
        ld (de),a
        inc de
        ld b,3
.ext:
        ld a,(hl)
        cp ' '
        jr z,.ext_space
        ld (de),a
        inc de
.ext_space:
        inc hl
        djnz .ext
.named:
        xor a
        ld (de),a
        ld a,(Mp3FentryName+1)
        cp #05                           ; так в каталоге записан первый байт #E5
        ret nz
        ld a,#E5
        ld (Mp3FentryName+1),a
        ret

; Войти в /WC/ESP на томе, откуда загружен WC (STREAM #FD: файл может быть и на
; другом устройстве — Nemo IDE); не вышло — на томе активной панели. CF=1 —
; каталога нет.
Esp_ToModules:
        ld d,STREAM_WC
        call WC_STREAM
        jr z,.wc
        ld d,STREAM_CLONE
        call WC_STREAM
        ld d,0
        ld bc,#FFFF
        call WC_STREAM
        ld d,STREAM_ROOT
        call WC_STREAM
        ld d,1
        ld bc,#FFFF
        call WC_STREAM
        ld d,STREAM_ROOT
        call WC_STREAM
        ld d,0
        ld bc,#FFFF
        call WC_STREAM
        ld hl,EspDirWc
        call WC_FENTRY
        jr z,.missing
        call WC_GDIR
.wc:
        ld hl,EspDirEsp
        call WC_FENTRY
        jr z,.missing
        call WC_GDIR
        or a
        ret
.missing:
        scf
        ret

; Модуль HL (имя с нулём) — в кэш ESP (ресурс, если (EspFlags)=1), если его
; там нет. CF=1 — не вышло, почему — в EspWhy.
Esp_Deliver:
        ld (EspName),hl
        ld a,WHY_OTHER
        ld (EspWhy),a
        ; FENTRY [#00][имя][0]
        ld de,EspFentry+1
        ld bc,13
        ldir
        ld hl,EspFentry
        call WC_FENTRY
        ld a,WHY_MISSING
        jp z,.why                        ; нет файла
        ld (EspSize),hl
        ld (EspSize+2),de
        ld de,EspEntry
        ld a,FN_TENTRY
        call Wc_Api
        ; MOD_QUERY [флаги][длина LE32][время FAT LE32: время, дата][имя, 0]
        ld hl,EspQuery
        ld a,(EspFlags)
        ld (hl),a
        inc hl
        ld de,(EspSize)
        ld (hl),e
        inc hl
        ld (hl),d
        inc hl
        ld de,(EspSize+2)
        ld (hl),e
        inc hl
        ld (hl),d
        inc hl
        ld de,(EspEntry+22)              ; время FAT
        ld (hl),e
        inc hl
        ld (hl),d
        inc hl
        ld de,(EspEntry+24)              ; дата FAT
        ld (hl),e
        inc hl
        ld (hl),d
        inc hl
        ex de,hl
        ld hl,(EspName)
        ld bc,13
        ldir
        ; Ресурс ESP может грузить с флешки до ответа (секунды): так и сказано.
        ld a,(EspFlags)
        or a
        jr z,.query
        ld de,EspStateText
        ld hl,UiStateChecking
        call Ui_CopyText
        ld hl,(EspName)
        call Ui_CopyText
        ld hl,EspStateText
        call Ui_SetState
        call Ui_Draw
.query:
        ld a,CMD_MOD_QUERY
        ld hl,EspQuery
        ld bc,9+13
        call Proto_Send
        jp c,.fail
        ld a,RESP_MOD_QUERY
        ld hl,ESP_QUERY_FRAMES           ; ESP может освобождать место (секунды)
        call Esp_WaitLong
        jp c,.fail
        ld a,(ProtoBuf)
        or a
        ret z                            ; уже в кэше; CF=0
        dec a
        jr z,.send
        dec a
        ld a,WHY_ROOM
        jp z,.why                        ; нет места
        jp .fail                         ; плохое имя
.send:
        ; Resources (SF2, ROM, MWK): same raw DMA as music. ELF delivery keeps
        ; its existing checked protocol. Old firmware retains 512-byte chunks.
        ld a,(EspFlags)
        ld hl,FastMedia
        and (hl)
        ld (EspFastTransfer),a
        ld hl,ESP_CHUNK
        jr z,.transfer_size
        ld hl,MP3_READ_SIZE
.transfer_size:
        ld (EspTransferSize),hl
        ; Передать файл, показывая ход передачи.
        ld de,EspStateText
        ld hl,UiStateSending
        call Ui_CopyText
        ld hl,(EspName)
        call Ui_CopyText
        ld hl,EspStateText
        call Ui_SetState
        ld hl,EspOffset
        ld (UiLoadSent),hl
        ld hl,EspSize
        ld (UiLoadTotal),hl
        call Ui_Draw
        call WC_GFILE
        ld hl,0
        ld (EspOffset),hl
        ld (EspOffset+2),hl
.sector:
        call WC_ESC
        ld a,WHY_ESC
        jp nz,.why
        ld a,(EspOffset+1)
        and (ESP_SHOW_EVERY-1)*2         ; раз в ESP_SHOW_EVERY секторов
        call z,Ui_ShowLoad
        ; Осталось: длина минус смещение (файлы модулей меньше 64 КБ? — нет, считаем 32 бита).
        ld hl,(EspSize)
        ld bc,(EspOffset)
        or a
        sbc hl,bc
        ld (EspLeft),hl
        ld hl,(EspSize+2)
        ld bc,(EspOffset+2)
        sbc hl,bc
        ld a,h
        or l
        ld hl,(EspLeft)
        jr nz,.full
        ld a,h
        or l
        jp z,.commit                     ; всё отдано
        ld de,(EspTransferSize)
        or a
        sbc hl,de
        jr nc,.full
        add hl,de
        jr .sized
.full:
        ld hl,(EspTransferSize)
.sized:
        ld (EspChunk),hl
        ld a,(EspFastTransfer)
        or a
        jr z,.legacy_read
        dec hl
        ld a,h
        srl a
        inc a
        ld b,a
        ld hl,Mp3ReadBuffer
        call WC_LOAD512
        ei
        jp c,.card
        call Fast_Resource
        jr .sent
.legacy_read:
        ld hl,Mp3Block
        ld b,1
        call WC_LOAD512
        ; Сектор не прочитался — доставка обрывается: прежде в MOD_PUT уходило
        ; прежнее содержимое Mp3Block, длина сходилась, и испорченный модуль
        ; ложился в кэш ESP.
        jr c,.card
        ; MOD_PUT [смещение LE32][данные] — данные прямо из Mp3Block.
        ld hl,Mp3Block
        ld (ProtoTail),hl
        ld hl,(EspChunk)
        ld (ProtoTailLen),hl
        xor a
        ld (ProtoHold),a
        ld a,CMD_MOD_PUT
        ld hl,EspOffset
        ld bc,4
        call Proto_Send2
.sent:
        jr c,.fail
        ld a,RESP_MOD_PUT
        ld de,MP3_WAIT
        call Mp3_Wait
        jr c,.fail
        ld a,(ProtoBuf)
        or a
        jr z,.fail
        ld hl,(EspOffset)
        ld bc,(EspChunk)
        add hl,bc
        ld (EspOffset),hl
        jp nc,.sector
        ld hl,(EspOffset+2)
        inc hl
        ld (EspOffset+2),hl
        jp .sector
.commit:
        ; Всё отдано: модуль ESP пишет во flash, это секунды — в строке
        ; состояния так и сказано, ждать — по времени (Esp_WaitLong).
        ld de,EspStateText
        ld hl,UiStateStoring
        call Ui_CopyText
        ld hl,(EspName)
        call Ui_CopyText
        ld hl,EspStateText
        call Ui_SetState
        call Ui_Draw
        ld a,CMD_MOD_COMMIT
        call Proto_SendEmpty
        jr c,.fail
        ld a,RESP_MOD_COMMIT
        ld hl,ESP_STORE_FRAMES
        call Esp_WaitLong
        jr c,.fail
        call Esp_LoadDone
        ld a,(ProtoBuf)
        cp 1
        ret z                            ; CF=0
.card:
        xor a                            ; WHY_OTHER
        jr .why
.fail:
        ld a,(EspWhy)
.why:
        ld (EspWhy),a
        call Esp_LoadDone
        xor a
        ld (ProtoErr),a                  ; отказ прошивки без модулей — не ошибка плеера
        scf
        ret

; Ресурс HL — в PSRAM ESP (MOD_QUERY с флагом 1). Нет места — AUDIO_OPEN [#20]
; (PLAYER освобождает банк MIDI и ПЗУ OPL4, звук стоп) и ещё раз. CF=1 — не
; вышло, почему — в EspWhy; EspReleased=1 — ресурсы освобождались.
Esp_Resource:
        ld a,1
        ld (EspFlags),a
        xor a
        ld (EspReleased),a
        push hl
        call Esp_Deliver
        pop hl
        jr nc,.done
        ld a,(EspWhy)
        cp WHY_ROOM
        scf
        jr nz,.done
        push hl
        ld a,OPEN_RELEASE
        ld (Mp3Arg),a
        ld (EspReleased),a
        ld a,CMD_AUDIO_OPEN
        ld hl,Mp3Arg
        ld bc,1
        call Proto_Send
        ld a,RESP_AUDIO_OPEN
        ld de,MP3_WAIT
        call nc,Mp3_Wait
        pop hl
        call Esp_Deliver
.done:
        ld a,0
        ld (EspFlags),a
        ret

; Песни MoonBlaster (MWM, MFM): ПЗУ OPL4 из /WC/ESP и набор волн MWM — ресурсами,
; пока файл не открыт: декодер берёт их при открытии. Имя набора — 8 знаков с
; байта 276 песни (до пробела), файл «ИМЯ.MWK» — в папке песни (каталог
; панели). Нет его (у песни без набора — «NONE») — песня играет без своих волн.
Esp_Kit:
        ld a,(EspAppRunning)
        or a
        ret z
        ld hl,(Mp3Ext)                   ; «M?M» — MWM или MFM
        ld a,l
        cp 'M'
        ret nz
        ld a,(Mp3Ext+2)
        cp 'M'
        ret nz
        ; ПЗУ OPL4 песням MoonBlaster нужно всегда: сразу, до открытия песни
        ; (не просьбой декодера — тогда песня начиналась бы заново).
        push hl
        call Esp_ToModules
        ld hl,EspRom
        call nc,Esp_Resource
        pop hl
        ld a,(EspWhy)
        cp WHY_ESC
        ret z                            ; Esc во время передачи
        ld a,h
        cp 'W'
        ret nz                           ; у MFM набора нет
        call Esp_Reopen                  ; свой файл с начала: первый сектор
        ld a,(EspReopened)
        or a
        ret z
        ld hl,Mp3Block
        ld b,1
        call WC_LOAD512
        ret c                            ; не прочитался — как без набора
        ld hl,Mp3Block+276
        ld de,EspKit
        ld b,8
.name:
        ld a,(hl)
        cp '!'
        jr c,.named                      ; пробел — конец имени
        ld (de),a
        inc hl
        inc de
        djnz .name
.named:
        ex de,hl
        ld (hl),'.'
        inc hl
        ld (hl),'M'
        inc hl
        ld (hl),'W'
        inc hl
        ld (hl),'K'
        inc hl
        ld (hl),0
        xor a
        ld (EspReopened),a               ; свой файл откроет Esp_Prepare
        ld d,STREAM_CLONE                ; каталог панели — папка песни
        call WC_STREAM
        ld hl,EspKit
        jp Esp_Resource

; ПЗУ OPL4 — декодеру, который его попросил (Mod_Play). CF=0 — файл открыть
; заново (ПЗУ у ESP или звук остановлен освобождением ресурсов), CF=1 — играть
; как есть (ПЗУ нет в /WC/ESP, Esc — EspWhy=WHY_ESC).
Esp_LoadRom:
        call Esp_ToModules
        ld a,WHY_MISSING
        ld (EspWhy),a
        jr c,.back
        ld hl,EspRom
        call Esp_Resource
        jr nc,.back
        ld a,(EspReleased)
        cp 1                             ; освобождались — CF=0: заново
.back:
        push af
        call Esp_Reopen
        call Esp_LoadDone
        pop af
        ret

; APP_START "PLAYER.ELF": [итог int8][имя, 0][версия, 0].
Esp_StartApp:
        ld a,CMD_APP_START
        ld hl,EspPlayer
        ld bc,11
        call Proto_Send
        ret c
        ld a,RESP_APP_START
        ld hl,ESP_START_FRAMES
        call Esp_WaitLong
        jr c,.fail
        ld a,(ProtoBuf)
        cp ESP_APP_OK
        ret nz
        ld a,1
        ld (EspAppRunning),a
        ret
.fail:
        xor a
        ld (ProtoErr),a
        ret

; Приложение заново (APP_STOP, APP_START): ESP выгружает PLAYER.ELF с его
; декодерами и отдаёт всю его память — так освобождается место, которое он
; держал (Mod_Play: «нет места для файла»). Выход: (EspAppRunning) — запустилось.
Esp_Restart:
        xor a
        ld (EspAppRunning),a
        ld a,CMD_APP_STOP
        call Proto_SendEmpty
        ret c
        ld a,RESP_APP_STOP
        ld hl,ESP_START_FRAMES
        call Esp_WaitLong
        jp nc,Esp_StartApp
        xor a
        ld (ProtoErr),a
        ret

; Ответ ESP с кодом A, на который ей нужно время (место под доставку, запись
; модуля во flash, запуск приложения): опрос каждый кадр, не дольше HL кадров;
; Esc — бросить
; (EspWhy=WHY_ESC). Выход — как у Mp3_Wait: CF=0 — ответ в ProtoBuf; CF=1 —
; нет ответа, отказ ESP (ProtoErr=1) или Esc.
Esp_WaitLong:
        ld (EspWaitCode),a
        ld (EspWaitLeft),hl
        xor a
        ld (ProtoErr),a                  ; по нему отказ отличается от тишины
.frame:
        ld a,(EspWaitCode)
        ld de,ESP_FRAME_POLLS
        call Mp3_Wait
        ret nc                           ; ответ
        ld a,(ProtoErr)
        or a
        scf
        ret nz                           ; отказ ESP
        call WC_ESC
        jr nz,.esc
        ei
        halt
        ld hl,(EspWaitLeft)
        dec hl
        ld (EspWaitLeft),hl
        ld a,h
        or l
        jr nz,.frame
        scf
        ret
.esc:
        ld a,WHY_ESC
        ld (EspWhy),a
        scf
        ret

; Свой файл — снова в потоке 0, с начала. Не нашёлся (или длина не та) —
; поток на начало файла плагина (GIPAGPL): ни перемотки, ни обложки.
Esp_Reopen:
        ld d,STREAM_CLONE
        call WC_STREAM
        ld hl,Mp3FentryName
        call WC_FENTRY
        jr z,.lost
        ld bc,(Mp3Total)
        or a
        sbc hl,bc
        jr nz,.lost
        ld hl,(Mp3Total+2)
        or a
        sbc hl,de
        jr nz,.lost
        call WC_GFILE
        ld a,1
        ld (EspReopened),a
        ret
.lost:
        ld a,FN_GIPAGPL
        jp Wc_Api
